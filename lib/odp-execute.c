/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * Copyright (c) 2013 Simon Horman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "odp-execute.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <stdlib.h>
#include <string.h>

#include "dpif.h"
#include "netlink.h"
#include "ofpbuf.h"
#include "odp-netlink.h"
#include "odp-util.h"
#include "packet-dpif.h"
#include "packets.h"
#include "flow.h"
#include "unaligned.h"
#include "util.h"

/* Masked copy of an ethernet address. 'src' is already properly masked. */
static void
ether_addr_copy_masked(uint8_t *dst, const uint8_t *src,
                       const uint8_t *mask)
{
    int i;

    for (i = 0; i < ETH_ADDR_LEN; i++) {
        dst[i] = src[i] | (dst[i] & ~mask[i]);
    }
}

static void
odp_eth_set_addrs(struct ofpbuf *packet, const struct ovs_key_ethernet *key,
                  const struct ovs_key_ethernet *mask)
{
    struct eth_header *eh = ofpbuf_l2(packet);

    if (eh) {
        if (!mask) {
            memcpy(eh->eth_src, key->eth_src, sizeof eh->eth_src);
            memcpy(eh->eth_dst, key->eth_dst, sizeof eh->eth_dst);
        } else {
            ether_addr_copy_masked(eh->eth_src, key->eth_src, mask->eth_src);
            ether_addr_copy_masked(eh->eth_dst, key->eth_dst, mask->eth_dst);
        }
    }
}

static void
odp_set_ipv4(struct ofpbuf *packet, const struct ovs_key_ipv4 *key,
             const struct ovs_key_ipv4 *mask)
{
    struct ip_header *nh = ofpbuf_l3(packet);

    packet_set_ipv4(
        packet,
        key->ipv4_src | (get_16aligned_be32(&nh->ip_src) & ~mask->ipv4_src),
        key->ipv4_dst | (get_16aligned_be32(&nh->ip_dst) & ~mask->ipv4_dst),
        key->ipv4_tos | (nh->ip_tos & ~mask->ipv4_tos),
        key->ipv4_ttl | (nh->ip_ttl & ~mask->ipv4_ttl));
}

static const ovs_be32 *
mask_ipv6_addr(const ovs_16aligned_be32 *old, const ovs_be32 *addr,
               const ovs_be32 *mask, ovs_be32 *masked)
{
    for (int i = 0; i < 4; i++) {
        masked[i] = addr[i] | (get_16aligned_be32(&old[i]) & ~mask[i]);
    }

    return masked;
}

static void
odp_set_ipv6(struct ofpbuf *packet, const struct ovs_key_ipv6 *key,
             const struct ovs_key_ipv6 *mask)
{
    struct ovs_16aligned_ip6_hdr *nh = ofpbuf_l3(packet);
    ovs_be32 sbuf[4], dbuf[4];
    uint8_t old_tc = ntohl(get_16aligned_be32(&nh->ip6_flow)) >> 20;
    ovs_be32 old_fl = get_16aligned_be32(&nh->ip6_flow) & htonl(0xfffff);

    packet_set_ipv6(
        packet,
        key->ipv6_proto,
        mask_ipv6_addr(nh->ip6_src.be32, key->ipv6_src, mask->ipv6_src, sbuf),
        mask_ipv6_addr(nh->ip6_dst.be32, key->ipv6_dst, mask->ipv6_dst, dbuf),
        key->ipv6_tclass | (old_tc & ~mask->ipv6_tclass),
        key->ipv6_label | (old_fl & ~mask->ipv6_label),
        key->ipv6_hlimit | (nh->ip6_hlim & ~mask->ipv6_hlimit));
}

static void
odp_set_tcp(struct ofpbuf *packet, const struct ovs_key_tcp *key,
             const struct ovs_key_tcp *mask)
{
    struct tcp_header *th = ofpbuf_l4(packet);

    if (OVS_LIKELY(th && ofpbuf_get_tcp_payload(packet))) {
        packet_set_tcp_port(packet,
                            key->tcp_src | (th->tcp_src & ~mask->tcp_src),
                            key->tcp_dst | (th->tcp_dst & ~mask->tcp_dst));
    }
}

static void
odp_set_udp(struct ofpbuf *packet, const struct ovs_key_udp *key,
             const struct ovs_key_udp *mask)
{
    struct udp_header *uh = ofpbuf_l4(packet);

    if (OVS_LIKELY(uh && ofpbuf_get_udp_payload(packet))) {
        packet_set_udp_port(packet,
                            key->udp_src | (uh->udp_src & ~mask->udp_src),
                            key->udp_dst | (uh->udp_dst & ~mask->udp_dst));
    }
}

static void
odp_set_sctp(struct ofpbuf *packet, const struct ovs_key_sctp *key,
             const struct ovs_key_sctp *mask)
{
    struct sctp_header *sh = ofpbuf_l4(packet);

    if (OVS_LIKELY(sh && ofpbuf_get_sctp_payload(packet))) {
        packet_set_sctp_port(packet,
                             key->sctp_src | (sh->sctp_src & ~mask->sctp_src),
                             key->sctp_dst | (sh->sctp_dst & ~mask->sctp_dst));
    }
}

static void
odp_set_tunnel_action(const struct nlattr *a, struct flow_tnl *tun_key)
{
    enum odp_key_fitness fitness;

    fitness = odp_tun_key_from_attr(a, tun_key);
    ovs_assert(fitness != ODP_FIT_ERROR);
}

static void
set_arp(struct ofpbuf *packet, const struct ovs_key_arp *key,
        const struct ovs_key_arp *mask)
{
    struct arp_eth_header *arp = ofpbuf_l3(packet);

    if (!mask) {
        arp->ar_op = key->arp_op;
        memcpy(arp->ar_sha, key->arp_sha, ETH_ADDR_LEN);
        put_16aligned_be32(&arp->ar_spa, key->arp_sip);
        memcpy(arp->ar_tha, key->arp_tha, ETH_ADDR_LEN);
        put_16aligned_be32(&arp->ar_tpa, key->arp_tip);
    } else {
        ovs_be32 ar_spa = get_16aligned_be32(&arp->ar_spa);
        ovs_be32 ar_tpa = get_16aligned_be32(&arp->ar_tpa);

        arp->ar_op = key->arp_op | (arp->ar_op & ~mask->arp_op);
        ether_addr_copy_masked(arp->ar_sha, key->arp_sha, mask->arp_sha);
        put_16aligned_be32(&arp->ar_spa,
                           key->arp_sip | (ar_spa & ~mask->arp_sip));
        ether_addr_copy_masked(arp->ar_tha, key->arp_tha, mask->arp_tha);
        put_16aligned_be32(&arp->ar_tpa,
                           key->arp_tip | (ar_tpa & ~mask->arp_tip));
    }
}

static void
odp_execute_set_action(struct dpif_packet *packet, const struct nlattr *a)
{
    enum ovs_key_attr type = nl_attr_type(a);
    const struct ovs_key_ipv4 *ipv4_key;
    const struct ovs_key_ipv6 *ipv6_key;
    struct pkt_metadata *md = &packet->md;

    switch (type) {
    case OVS_KEY_ATTR_PRIORITY:
        md->skb_priority = nl_attr_get_u32(a);
        break;

    case OVS_KEY_ATTR_TUNNEL:
        odp_set_tunnel_action(a, &md->tunnel);
        break;

    case OVS_KEY_ATTR_SKB_MARK:
        md->pkt_mark = nl_attr_get_u32(a);
        break;

    case OVS_KEY_ATTR_ETHERNET:
        odp_eth_set_addrs(&packet->ofpbuf, nl_attr_get(a), NULL);
        break;

    case OVS_KEY_ATTR_IPV4:
        ipv4_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv4));
        packet_set_ipv4(&packet->ofpbuf, ipv4_key->ipv4_src,
                        ipv4_key->ipv4_dst, ipv4_key->ipv4_tos,
                        ipv4_key->ipv4_ttl);
        break;

    case OVS_KEY_ATTR_IPV6:
        ipv6_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv6));
        packet_set_ipv6(&packet->ofpbuf, ipv6_key->ipv6_proto,
                        ipv6_key->ipv6_src, ipv6_key->ipv6_dst,
                        ipv6_key->ipv6_tclass, ipv6_key->ipv6_label,
                        ipv6_key->ipv6_hlimit);
        break;

    case OVS_KEY_ATTR_TCP:
        if (OVS_LIKELY(ofpbuf_get_tcp_payload(&packet->ofpbuf))) {
            const struct ovs_key_tcp *tcp_key
                = nl_attr_get_unspec(a, sizeof(struct ovs_key_tcp));

            packet_set_tcp_port(&packet->ofpbuf, tcp_key->tcp_src,
                                tcp_key->tcp_dst);
        }
        break;

    case OVS_KEY_ATTR_UDP:
        if (OVS_LIKELY(ofpbuf_get_udp_payload(&packet->ofpbuf))) {
            const struct ovs_key_udp *udp_key
                = nl_attr_get_unspec(a, sizeof(struct ovs_key_udp));

            packet_set_udp_port(&packet->ofpbuf, udp_key->udp_src,
                                udp_key->udp_dst);
        }
        break;

    case OVS_KEY_ATTR_SCTP:
        if (OVS_LIKELY(ofpbuf_get_sctp_payload(&packet->ofpbuf))) {
            const struct ovs_key_sctp *sctp_key
                = nl_attr_get_unspec(a, sizeof(struct ovs_key_sctp));

            packet_set_sctp_port(&packet->ofpbuf, sctp_key->sctp_src,
                                 sctp_key->sctp_dst);
        }
        break;

    case OVS_KEY_ATTR_MPLS:
        set_mpls_lse(&packet->ofpbuf, nl_attr_get_be32(a));
        break;

    case OVS_KEY_ATTR_ARP:
        set_arp(&packet->ofpbuf, nl_attr_get(a), NULL);
        break;

    case OVS_KEY_ATTR_DP_HASH:
        md->dp_hash = nl_attr_get_u32(a);
        dpif_packet_set_dp_hash(packet, md->dp_hash);
        break;

    case OVS_KEY_ATTR_RECIRC_ID:
        md->recirc_id = nl_attr_get_u32(a);
        break;

    case OVS_KEY_ATTR_UNSPEC:
    case OVS_KEY_ATTR_ENCAP:
    case OVS_KEY_ATTR_ETHERTYPE:
    case OVS_KEY_ATTR_IN_PORT:
    case OVS_KEY_ATTR_VLAN:
    case OVS_KEY_ATTR_ICMP:
    case OVS_KEY_ATTR_ICMPV6:
    case OVS_KEY_ATTR_ND:
    case OVS_KEY_ATTR_TCP_FLAGS:
    case __OVS_KEY_ATTR_MAX:
    default:
        OVS_NOT_REACHED();
    }
}

#define get_mask(a, type) ((const type *)(const void *)(a + 1) + 1)

static void
odp_execute_masked_set_action(struct dpif_packet *packet,
                              const struct nlattr *a)
{
    struct pkt_metadata *md = &packet->md;
    enum ovs_key_attr type = nl_attr_type(a);
    struct mpls_hdr *mh;

    switch (type) {
    case OVS_KEY_ATTR_PRIORITY:
        md->skb_priority = nl_attr_get_u32(a)
            | (md->skb_priority & ~*get_mask(a, uint32_t));
        break;

    case OVS_KEY_ATTR_SKB_MARK:
        md->pkt_mark = nl_attr_get_u32(a)
            | (md->pkt_mark & ~*get_mask(a, uint32_t));
        break;

    case OVS_KEY_ATTR_ETHERNET:
        odp_eth_set_addrs(&packet->ofpbuf, nl_attr_get(a),
                          get_mask(a, struct ovs_key_ethernet));
        break;

    case OVS_KEY_ATTR_IPV4:
        odp_set_ipv4(&packet->ofpbuf, nl_attr_get(a),
                     get_mask(a, struct ovs_key_ipv4));
        break;

    case OVS_KEY_ATTR_IPV6:
        odp_set_ipv6(&packet->ofpbuf, nl_attr_get(a),
                     get_mask(a, struct ovs_key_ipv6));
        break;

    case OVS_KEY_ATTR_TCP:
        odp_set_tcp(&packet->ofpbuf, nl_attr_get(a),
                    get_mask(a, struct ovs_key_tcp));
        break;

    case OVS_KEY_ATTR_UDP:
        odp_set_udp(&packet->ofpbuf, nl_attr_get(a),
                    get_mask(a, struct ovs_key_udp));
        break;

    case OVS_KEY_ATTR_SCTP:
        odp_set_sctp(&packet->ofpbuf, nl_attr_get(a),
                     get_mask(a, struct ovs_key_sctp));
        break;

    case OVS_KEY_ATTR_MPLS:
        mh = ofpbuf_l2_5(&packet->ofpbuf);
        if (mh) {
            put_16aligned_be32(&mh->mpls_lse, nl_attr_get_be32(a)
                               | (get_16aligned_be32(&mh->mpls_lse)
                                  & ~*get_mask(a, ovs_be32)));
        }
        break;

    case OVS_KEY_ATTR_ARP:
        set_arp(&packet->ofpbuf, nl_attr_get(a),
                get_mask(a, struct ovs_key_arp));
        break;

    case OVS_KEY_ATTR_DP_HASH:
        md->dp_hash = nl_attr_get_u32(a)
            | (dpif_packet_get_dp_hash(packet) & ~*get_mask(a, uint32_t));
        dpif_packet_set_dp_hash(packet, md->dp_hash);
        break;

    case OVS_KEY_ATTR_RECIRC_ID:
        md->recirc_id = nl_attr_get_u32(a)
            | (md->recirc_id & ~*get_mask(a, uint32_t));
        break;

    case OVS_KEY_ATTR_TUNNEL:    /* Masked data not supported for tunnel. */
    case OVS_KEY_ATTR_UNSPEC:
    case OVS_KEY_ATTR_ENCAP:
    case OVS_KEY_ATTR_ETHERTYPE:
    case OVS_KEY_ATTR_IN_PORT:
    case OVS_KEY_ATTR_VLAN:
    case OVS_KEY_ATTR_ICMP:
    case OVS_KEY_ATTR_ICMPV6:
    case OVS_KEY_ATTR_ND:
    case OVS_KEY_ATTR_TCP_FLAGS:
    case __OVS_KEY_ATTR_MAX:
    default:
        OVS_NOT_REACHED();
    }
}

static void
odp_execute_sample(void *dp, struct dpif_packet *packet, bool steal,
                   const struct nlattr *action,
                   odp_execute_cb dp_execute_action)
{
    const struct nlattr *subactions = NULL;
    const struct nlattr *a;
    size_t left;

    NL_NESTED_FOR_EACH_UNSAFE (a, left, action) {
        int type = nl_attr_type(a);

        switch ((enum ovs_sample_attr) type) {
        case OVS_SAMPLE_ATTR_PROBABILITY:
            if (random_uint32() >= nl_attr_get_u32(a)) {
                if (steal) {
                    dpif_packet_delete(packet);
                }
                return;
            }
            break;

        case OVS_SAMPLE_ATTR_ACTIONS:
            subactions = a;
            break;

        case OVS_SAMPLE_ATTR_UNSPEC:
        case __OVS_SAMPLE_ATTR_MAX:
        default:
            OVS_NOT_REACHED();
        }
    }

    odp_execute_actions(dp, &packet, 1, steal, nl_attr_get(subactions),
                        nl_attr_get_size(subactions), dp_execute_action);
}

void
odp_execute_actions(void *dp, struct dpif_packet **packets, int cnt, bool steal,
                    const struct nlattr *actions, size_t actions_len,
                    odp_execute_cb dp_execute_action)
{
    const struct nlattr *a;
    unsigned int left;
    int i;

    NL_ATTR_FOR_EACH_UNSAFE (a, left, actions, actions_len) {
        int type = nl_attr_type(a);
        bool last_action = (left <= NLA_ALIGN(a->nla_len));

        switch ((enum ovs_action_attr) type) {
            /* These only make sense in the context of a datapath. */
        case OVS_ACTION_ATTR_OUTPUT:
        case OVS_ACTION_ATTR_TUNNEL_PUSH:
        case OVS_ACTION_ATTR_TUNNEL_POP:
        case OVS_ACTION_ATTR_USERSPACE:
        case OVS_ACTION_ATTR_RECIRC:
            if (dp_execute_action) {
                /* Allow 'dp_execute_action' to steal the packet data if we do
                 * not need it any more. */
                bool may_steal = steal && last_action;

                dp_execute_action(dp, packets, cnt, a, may_steal);

                if (last_action) {
                    /* We do not need to free the packets. dp_execute_actions()
                     * has stolen them */
                    return;
                }
            }
            break;

        case OVS_ACTION_ATTR_HASH: {
            const struct ovs_action_hash *hash_act = nl_attr_get(a);

            /* Calculate a hash value directly.  This might not match the
             * value computed by the datapath, but it is much less expensive,
             * and the current use case (bonding) does not require a strict
             * match to work properly. */
            if (hash_act->hash_alg == OVS_HASH_ALG_L4) {
                struct flow flow;
                uint32_t hash;

                for (i = 0; i < cnt; i++) {
                    flow_extract(&packets[i]->ofpbuf, &packets[i]->md, &flow);
                    hash = flow_hash_5tuple(&flow, hash_act->hash_basis);

                    /* We also store the hash value with each packet */
                    dpif_packet_set_dp_hash(packets[i], hash ? hash : 1);
                }
            } else {
                /* Assert on unknown hash algorithm.  */
                OVS_NOT_REACHED();
            }
            break;
        }

        case OVS_ACTION_ATTR_PUSH_ETH: {
            const struct ovs_action_push_eth *eth = nl_attr_get(a);

            for (i = 0; i < cnt; i++) {
                struct ofpbuf *ofp = &packets[i]->ofpbuf;

                push_eth(ofp, eth->addresses.eth_dst, eth->addresses.eth_src,
                        eth->eth_type);
            }
            break;
        }

        case OVS_ACTION_ATTR_POP_ETH: {
            for (i = 0; i < cnt; i++) {
                pop_eth(&packets[i]->ofpbuf);
            }
            break;
        }

        case OVS_ACTION_ATTR_PUSH_VLAN: {
            const struct ovs_action_push_vlan *vlan = nl_attr_get(a);

            for (i = 0; i < cnt; i++) {
                struct ofpbuf *buf = &packets[i]->ofpbuf;

                eth_push_vlan(buf, htons(ETH_TYPE_VLAN), vlan->vlan_tci);
            }
            break;
        }

        case OVS_ACTION_ATTR_POP_VLAN:
            for (i = 0; i < cnt; i++) {
                struct ofpbuf *buf = &packets[i]->ofpbuf;

                eth_pop_vlan(buf);
            }
            break;

        case OVS_ACTION_ATTR_PUSH_MPLS: {
            const struct ovs_action_push_mpls *mpls = nl_attr_get(a);

            for (i = 0; i < cnt; i++) {
                struct ofpbuf *buf = &packets[i]->ofpbuf;

                push_mpls(buf, mpls->mpls_ethertype, mpls->mpls_lse);
            }
            break;
         }

        case OVS_ACTION_ATTR_POP_MPLS:
            for (i = 0; i < cnt; i++) {
                struct ofpbuf *buf = &packets[i]->ofpbuf;

                pop_mpls(buf, nl_attr_get_be16(a));
            }
            break;

        case OVS_ACTION_ATTR_SET:
            for (i = 0; i < cnt; i++) {
                odp_execute_set_action(packets[i], nl_attr_get(a));
            }
            break;

        case OVS_ACTION_ATTR_SET_MASKED:
            for (i = 0; i < cnt; i++) {
                odp_execute_masked_set_action(packets[i], nl_attr_get(a));
            }
            break;

        case OVS_ACTION_ATTR_SAMPLE:
            for (i = 0; i < cnt; i++) {
                odp_execute_sample(dp, packets[i], steal && last_action, a,
                                   dp_execute_action);
            }

            if (last_action) {
                /* We do not need to free the packets. odp_execute_sample() has
                 * stolen them*/
                return;
            }
            break;

        case OVS_ACTION_ATTR_UNSPEC:
        case __OVS_ACTION_ATTR_MAX:
            OVS_NOT_REACHED();
        }
    }

    if (steal) {
        for (i = 0; i < cnt; i++) {
            dpif_packet_delete(packets[i]);
        }
    }
}
