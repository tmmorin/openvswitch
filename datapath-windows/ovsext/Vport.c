/*
 * Copyright (c) 2014 VMware, Inc.
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

#include "precomp.h"
#include "Jhash.h"
#include "Switch.h"
#include "Vport.h"
#include "Event.h"
#include "User.h"
#include "Vxlan.h"
#include "IpHelper.h"
#include "Oid.h"
#include "Datapath.h"

#ifdef OVS_DBG_MOD
#undef OVS_DBG_MOD
#endif
#define OVS_DBG_MOD OVS_DBG_VPORT
#include "Debug.h"

#define VPORT_NIC_ENTER(_nic) \
    OVS_LOG_TRACE("Enter: PortId: %x, NicIndex: %d", _nic->PortId, \
                                                     _nic->NicIndex)

#define VPORT_NIC_EXIT(_nic) \
    OVS_LOG_TRACE("Exit: PortId: %x, NicIndex: %d", _nic->PortId, \
                                                    _nic->NicIndex)

#define VPORT_PORT_ENTER(_port) \
    OVS_LOG_TRACE("Enter: PortId: %x", _port->PortId)

#define VPORT_PORT_EXIT(_port) \
    OVS_LOG_TRACE("Exit: PortId: %x", _port->PortId)

#define OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC    100

extern POVS_SWITCH_CONTEXT gOvsSwitchContext;
extern PNDIS_SPIN_LOCK gOvsCtrlLock;

static VOID OvsInitVportWithPortParam(POVS_VPORT_ENTRY vport,
                PNDIS_SWITCH_PORT_PARAMETERS portParam);
static VOID OvsInitVportWithNicParam(POVS_SWITCH_CONTEXT switchContext,
                POVS_VPORT_ENTRY vport, PNDIS_SWITCH_NIC_PARAMETERS nicParam);
static VOID OvsInitPhysNicVport(POVS_VPORT_ENTRY physExtVPort,
                POVS_VPORT_ENTRY virtExtVport, UINT32 nicIndex);
static __inline VOID OvsWaitActivate(POVS_SWITCH_CONTEXT switchContext,
                                     ULONG sleepMicroSec);
static NTSTATUS OvsGetExtInfoIoctl(POVS_VPORT_GET vportGet,
                                   POVS_VPORT_EXT_INFO extInfo);
static NTSTATUS CreateNetlinkMesgForNetdev(POVS_VPORT_EXT_INFO info,
                                           POVS_MESSAGE msgIn,
                                           PVOID outBuffer,
                                           UINT32 outBufLen,
                                           int dpIfIndex);

/*
 * Functions implemented in relaton to NDIS port manipulation.
 */
NDIS_STATUS
HvCreatePort(POVS_SWITCH_CONTEXT switchContext,
             PNDIS_SWITCH_PORT_PARAMETERS portParam)
{
    POVS_VPORT_ENTRY vport;
    LOCK_STATE_EX lockState;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;

    VPORT_PORT_ENTER(portParam);

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            portParam->PortId, 0);
    if (vport != NULL && !vport->hvDeleted) {
        status = STATUS_DATA_NOT_ACCEPTED;
        goto create_port_done;
    } else if (!vport) {
        vport = (POVS_VPORT_ENTRY)OvsAllocateVport();
        if (vport == NULL) {
            status = NDIS_STATUS_RESOURCES;
            goto create_port_done;
        }
    }

    OvsInitVportWithPortParam(vport, portParam);
    InitHvVportCommon(switchContext, vport);

create_port_done:
    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
    VPORT_PORT_EXIT(portParam);
    return status;
}


/*
 * Function updating the port properties
 */
NDIS_STATUS
HvUpdatePort(POVS_SWITCH_CONTEXT switchContext,
             PNDIS_SWITCH_PORT_PARAMETERS portParam)
{
    POVS_VPORT_ENTRY vport;
    LOCK_STATE_EX lockState;
    OVS_VPORT_STATE ovsState;
    NDIS_SWITCH_NIC_STATE nicState;

    VPORT_PORT_ENTER(portParam);

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            portParam->PortId, 0);
    /*
     * Update properties only for NETDEV ports for supprting PS script
     * We don't allow changing the names of the internal or external ports
     */
    if (vport == NULL || ( vport->portType != NdisSwitchPortTypeSynthetic) || 
        ( vport->portType != NdisSwitchPortTypeEmulated)) {
        goto update_port_done;
    }

    /* Store the nic and the OVS states as Nic Create won't be called */
    ovsState = vport->ovsState;
    nicState = vport->nicState;
    
    /*
     * Currently only the port friendly name is being updated
     * Make sure that no other properties are changed
     */
    ASSERT(portParam->PortId == vport->portId);
    ASSERT(portParam->PortState == vport->portState);
    ASSERT(portParam->PortType == vport->portType);

    /*
     * Call the set parameters function the handle all properties
     * change in a single place in case future version supports change of
     * other properties
     */
    OvsInitVportWithPortParam(vport, portParam);
    /* Retore the nic and OVS states */
    vport->nicState = nicState;
    vport->ovsState = ovsState;

update_port_done:
    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
    VPORT_PORT_EXIT(portParam);

    /* Must always return success */
    return NDIS_STATUS_SUCCESS;
}

VOID
HvTeardownPort(POVS_SWITCH_CONTEXT switchContext,
               PNDIS_SWITCH_PORT_PARAMETERS portParam)
{
    POVS_VPORT_ENTRY vport;
    LOCK_STATE_EX lockState;

    VPORT_PORT_ENTER(portParam);

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            portParam->PortId, 0);
    if (vport) {
        /* add assertion here
         */
        vport->portState = NdisSwitchPortStateTeardown;
        vport->ovsState = OVS_STATE_PORT_TEAR_DOWN;
    } else {
        OVS_LOG_WARN("Vport not present.");
    }
    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);

    VPORT_PORT_EXIT(portParam);
}



VOID
HvDeletePort(POVS_SWITCH_CONTEXT switchContext,
             PNDIS_SWITCH_PORT_PARAMETERS portParams)
{
    POVS_VPORT_ENTRY vport;
    LOCK_STATE_EX lockState;

    VPORT_PORT_ENTER(portParams);

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            portParams->PortId, 0);

    /*
     * XXX: we can only destroy and remove the port if its datapath port
     * counterpart was deleted. If the datapath port counterpart is present,
     * we only mark the vport for deletion, so that a netlink command vport
     * delete will delete the vport.
    */
    if (vport) {
        if (vport->portNo == OVS_DPPORT_NUMBER_INVALID) {
            OvsRemoveAndDeleteVport(switchContext, vport);
        } else {
            vport->hvDeleted = TRUE;
        }
    } else {
        OVS_LOG_WARN("Vport not present.");
    }
    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);

    VPORT_PORT_EXIT(portParams);
}


/*
 * Functions implemented in relaton to NDIS NIC manipulation.
 */
NDIS_STATUS
HvCreateNic(POVS_SWITCH_CONTEXT switchContext,
            PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    POVS_VPORT_ENTRY vport;
    UINT32 portNo = 0;
    UINT32 event = 0;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;

    LOCK_STATE_EX lockState;

    VPORT_NIC_ENTER(nicParam);

    /* Wait for lists to be initialized. */
    OvsWaitActivate(switchContext, OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC);

    if (!switchContext->isActivated) {
        OVS_LOG_WARN("Switch is not activated yet.");
        /* Veto the creation of nic */
        status = NDIS_STATUS_NOT_SUPPORTED;
        goto done;
    }

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext, nicParam->PortId, 0);
    if (vport == NULL) {
        OVS_LOG_ERROR("Create NIC without Switch Port,"
                      " PortId: %x, NicIndex: %d",
                      nicParam->PortId, nicParam->NicIndex);
        status = NDIS_STATUS_INVALID_PARAMETER;
        goto add_nic_done;
    }

    if (nicParam->NicType == NdisSwitchNicTypeExternal &&
        nicParam->NicIndex != 0) {
        POVS_VPORT_ENTRY virtExtVport =
            (POVS_VPORT_ENTRY)switchContext->virtualExternalVport;

        vport = (POVS_VPORT_ENTRY)OvsAllocateVport();
        if (vport == NULL) {
            status = NDIS_STATUS_RESOURCES;
            goto add_nic_done;
        }
        OvsInitPhysNicVport(vport, virtExtVport, nicParam->NicIndex);
        status = InitHvVportCommon(switchContext, vport);
        if (status != NDIS_STATUS_SUCCESS) {
            OvsFreeMemory(vport);
            goto add_nic_done;
        }
    }
    OvsInitVportWithNicParam(switchContext, vport, nicParam);
    portNo = vport->portNo;
    if (vport->ovsState == OVS_STATE_CONNECTED) {
        event = OVS_EVENT_CONNECT | OVS_EVENT_LINK_UP;
    } else if (vport->ovsState == OVS_STATE_NIC_CREATED) {
        event = OVS_EVENT_CONNECT;
    }

add_nic_done:
    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
    if (portNo != OVS_DPPORT_NUMBER_INVALID && event) {
        OvsPostEvent(portNo, event);
    }

done:
    VPORT_NIC_EXIT(nicParam);
    OVS_LOG_TRACE("Exit: status %8x.\n", status);

    return status;
}


/* Mark already created NIC as connected. */
VOID
HvConnectNic(POVS_SWITCH_CONTEXT switchContext,
             PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    LOCK_STATE_EX lockState;
    POVS_VPORT_ENTRY vport;
    UINT32 portNo = 0;

    VPORT_NIC_ENTER(nicParam);

    /* Wait for lists to be initialized. */
    OvsWaitActivate(switchContext, OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC);

    if (!switchContext->isActivated) {
        OVS_LOG_WARN("Switch is not activated yet.");
        goto done;
    }

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            nicParam->PortId,
                                            nicParam->NicIndex);

    if (!vport) {
        OVS_LOG_WARN("Vport not present.");
        NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
        ASSERT(0);
        goto done;
    }

    vport->ovsState = OVS_STATE_CONNECTED;
    vport->nicState = NdisSwitchNicStateConnected;
    portNo = vport->portNo;

    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);

    /* XXX only if portNo != INVALID or always? */
    OvsPostEvent(portNo, OVS_EVENT_LINK_UP);

    if (nicParam->NicType == NdisSwitchNicTypeInternal) {
        OvsInternalAdapterUp(portNo, &nicParam->NetCfgInstanceId);
    }

done:
    VPORT_NIC_EXIT(nicParam);
}

VOID
HvUpdateNic(POVS_SWITCH_CONTEXT switchContext,
            PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    POVS_VPORT_ENTRY vport;
    LOCK_STATE_EX lockState;

    UINT32 status = 0, portNo = 0;

    VPORT_NIC_ENTER(nicParam);

    /* Wait for lists to be initialized. */
    OvsWaitActivate(switchContext, OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC);

    if (!switchContext->isActivated) {
        OVS_LOG_WARN("Switch is not activated yet.");
        goto update_nic_done;
    }

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            nicParam->PortId,
                                            nicParam->NicIndex);
    if (vport == NULL) {
        OVS_LOG_WARN("Vport search failed.");
        goto update_nic_done;
    }
    switch (nicParam->NicType) {
    case NdisSwitchNicTypeExternal:
    case NdisSwitchNicTypeInternal:
        RtlCopyMemory(&vport->netCfgInstanceId, &nicParam->NetCfgInstanceId,
                      sizeof (GUID));
        break;
    case NdisSwitchNicTypeSynthetic:
    case NdisSwitchNicTypeEmulated:
        if (!RtlEqualMemory(vport->vmMacAddress, nicParam->VMMacAddress,
                           sizeof (vport->vmMacAddress))) {
            status |= OVS_EVENT_MAC_CHANGE;
            RtlCopyMemory(vport->vmMacAddress, nicParam->VMMacAddress,
                          sizeof (vport->vmMacAddress));
        }
        break;
    default:
        ASSERT(0);
    }
    if (!RtlEqualMemory(vport->permMacAddress, nicParam->PermanentMacAddress,
                        sizeof (vport->permMacAddress))) {
        RtlCopyMemory(vport->permMacAddress, nicParam->PermanentMacAddress,
                      sizeof (vport->permMacAddress));
        status |= OVS_EVENT_MAC_CHANGE;
    }
    if (!RtlEqualMemory(vport->currMacAddress, nicParam->CurrentMacAddress,
                        sizeof (vport->currMacAddress))) {
        RtlCopyMemory(vport->currMacAddress, nicParam->CurrentMacAddress,
                      sizeof (vport->currMacAddress));
        status |= OVS_EVENT_MAC_CHANGE;
    }

    if (vport->mtu != nicParam->MTU) {
        vport->mtu = nicParam->MTU;
        status |= OVS_EVENT_MTU_CHANGE;
    }
    vport->numaNodeId = nicParam->NumaNodeId;
    portNo = vport->portNo;

    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
    if (status && portNo) {
        OvsPostEvent(portNo, status);
    }
update_nic_done:
    VPORT_NIC_EXIT(nicParam);
}


VOID
HvDisconnectNic(POVS_SWITCH_CONTEXT switchContext,
                PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    POVS_VPORT_ENTRY vport;
    UINT32 portNo = 0;
    LOCK_STATE_EX lockState;
    BOOLEAN isInternalPort = FALSE;

    VPORT_NIC_ENTER(nicParam);

    /* Wait for lists to be initialized. */
    OvsWaitActivate(switchContext, OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC);

    if (!switchContext->isActivated) {
        OVS_LOG_WARN("Switch is not activated yet.");
        goto done;
    }

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            nicParam->PortId,
                                            nicParam->NicIndex);

    if (!vport) {
        OVS_LOG_WARN("Vport not present.");
        NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
        goto done;
    }

    vport->nicState = NdisSwitchNicStateDisconnected;
    vport->ovsState = OVS_STATE_NIC_CREATED;
    portNo = vport->portNo;

    if (vport->ovsType == OVS_VPORT_TYPE_INTERNAL) {
        isInternalPort = TRUE;
    }

    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);

    /* XXX if portNo != INVALID or always? */
    OvsPostEvent(portNo, OVS_EVENT_LINK_DOWN);

    if (isInternalPort) {
        OvsInternalAdapterDown();
    }

done:
    VPORT_NIC_EXIT(nicParam);
}


VOID
HvDeleteNic(POVS_SWITCH_CONTEXT switchContext,
            PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    LOCK_STATE_EX lockState;
    POVS_VPORT_ENTRY vport;
    UINT32 portNo = 0;

    VPORT_NIC_ENTER(nicParam);
    /* Wait for lists to be initialized. */
    OvsWaitActivate(switchContext, OVS_VPORT_DEFAULT_WAIT_TIME_MICROSEC);

    if (!switchContext->isActivated) {
        OVS_LOG_WARN("Switch is not activated yet.");
        goto done;
    }

    NdisAcquireRWLockWrite(switchContext->dispatchLock, &lockState, 0);
    vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                            nicParam->PortId,
                                            nicParam->NicIndex);

    if (!vport) {
        OVS_LOG_WARN("Vport not present.");
        NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
        goto done;
    }

    portNo = vport->portNo;
    if (vport->portType == NdisSwitchPortTypeExternal &&
        vport->nicIndex != 0) {
        OvsRemoveAndDeleteVport(switchContext, vport);
    }
    vport->nicState = NdisSwitchNicStateUnknown;
    vport->ovsState = OVS_STATE_PORT_CREATED;

    NdisReleaseRWLock(switchContext->dispatchLock, &lockState);
    /* XXX if portNo != INVALID or always? */
    OvsPostEvent(portNo, OVS_EVENT_DISCONNECT);

done:
    VPORT_NIC_EXIT(nicParam);
}


/*
 * OVS Vport related functionality.
 */
POVS_VPORT_ENTRY
OvsFindVportByPortNo(POVS_SWITCH_CONTEXT switchContext,
                     UINT32 portNo)
{
    POVS_VPORT_ENTRY vport;
    PLIST_ENTRY head, link;
    UINT32 hash = OvsJhashBytes((const VOID *)&portNo, sizeof(portNo),
                                OVS_HASH_BASIS);
    head = &(switchContext->portNoHashArray[hash & OVS_VPORT_MASK]);
    LIST_FORALL(head, link) {
        vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portNoLink);
        if (vport->portNo == portNo) {
            return vport;
        }
    }
    return NULL;
}


POVS_VPORT_ENTRY
OvsFindVportByOvsName(POVS_SWITCH_CONTEXT switchContext,
                      PSTR name)
{
    POVS_VPORT_ENTRY vport;
    PLIST_ENTRY head, link;
    UINT32 hash;
    SIZE_T length = strlen(name) + 1;

    hash = OvsJhashBytes((const VOID *)name, length, OVS_HASH_BASIS);
    head = &(switchContext->ovsPortNameHashArray[hash & OVS_VPORT_MASK]);

    LIST_FORALL(head, link) {
        vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, ovsNameLink);
        if (!strcmp(name, vport->ovsName)) {
            return vport;
        }
    }

    return NULL;
}

/* OvsFindVportByHvName: "name" is assumed to be null-terminated */
POVS_VPORT_ENTRY
OvsFindVportByHvName(POVS_SWITCH_CONTEXT switchContext,
                     PSTR name)
{
    POVS_VPORT_ENTRY vport = NULL;
    PLIST_ENTRY head, link;
    /* 'portFriendlyName' is not NUL-terminated. */
    SIZE_T length = strlen(name);
    SIZE_T wstrSize = length * sizeof(WCHAR);
    UINT i;

    PWSTR wsName = OvsAllocateMemory(wstrSize);
    if (!wsName) {
        return NULL;
    }
    for (i = 0; i < length; i++) {
        wsName[i] = name[i];
    }

    for (i = 0; i < OVS_MAX_VPORT_ARRAY_SIZE; i++) {
        head = &(switchContext->portIdHashArray[i]);
        LIST_FORALL(head, link) {
            vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portIdLink);

            /*
             * NOTE about portFriendlyName:
             * If the string is NULL-terminated, the Length member does not
             * include the terminating NULL character.
             */
            if (vport->portFriendlyName.Length == wstrSize &&
                RtlEqualMemory(wsName, vport->portFriendlyName.String,
                               vport->portFriendlyName.Length)) {
                goto Cleanup;
            }

            vport = NULL;
        }
    }

Cleanup:
    OvsFreeMemory(wsName);

    return vport;
}

POVS_VPORT_ENTRY
OvsFindVportByPortIdAndNicIndex(POVS_SWITCH_CONTEXT switchContext,
                                NDIS_SWITCH_PORT_ID portId,
                                NDIS_SWITCH_NIC_INDEX index)
{
    if (switchContext->virtualExternalVport &&
            portId == switchContext->virtualExternalPortId &&
            index == switchContext->virtualExternalVport->nicIndex) {
        return (POVS_VPORT_ENTRY)switchContext->virtualExternalVport;
    } else if (switchContext->internalVport &&
               portId == switchContext->internalPortId &&
               index == switchContext->internalVport->nicIndex) {
        return (POVS_VPORT_ENTRY)switchContext->internalVport;
    } else {
        PLIST_ENTRY head, link;
        POVS_VPORT_ENTRY vport;
        UINT32 hash;
        hash = OvsJhashWords((UINT32 *)&portId, 1, OVS_HASH_BASIS);
        head = &(switchContext->portIdHashArray[hash & OVS_VPORT_MASK]);
        LIST_FORALL(head, link) {
            vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portIdLink);
            if (portId == vport->portId && index == vport->nicIndex) {
                return vport;
            }
        }
        return NULL;
    }
}

POVS_VPORT_ENTRY
OvsAllocateVport(VOID)
{
    POVS_VPORT_ENTRY vport;
    vport = (POVS_VPORT_ENTRY)OvsAllocateMemory(sizeof (OVS_VPORT_ENTRY));
    if (vport == NULL) {
        return NULL;
    }
    RtlZeroMemory(vport, sizeof (OVS_VPORT_ENTRY));
    vport->ovsState = OVS_STATE_UNKNOWN;
    vport->hvDeleted = FALSE;
    vport->portNo = OVS_DPPORT_NUMBER_INVALID;

    InitializeListHead(&vport->ovsNameLink);
    InitializeListHead(&vport->portIdLink);
    InitializeListHead(&vport->portNoLink);

    return vport;
}

static VOID
OvsInitVportWithPortParam(POVS_VPORT_ENTRY vport,
                          PNDIS_SWITCH_PORT_PARAMETERS portParam)
{
    vport->portType = portParam->PortType;
    vport->portState = portParam->PortState;
    vport->portId = portParam->PortId;
    vport->nicState = NdisSwitchNicStateUnknown;
    vport->isExternal = FALSE;
    vport->isBridgeInternal = FALSE;

    switch (vport->portType) {
    case NdisSwitchPortTypeExternal:
        vport->isExternal = TRUE;
        vport->ovsType = OVS_VPORT_TYPE_NETDEV;
        break;
    case NdisSwitchPortTypeInternal:
        vport->ovsType = OVS_VPORT_TYPE_INTERNAL;
        break;
    case NdisSwitchPortTypeSynthetic:
    case NdisSwitchPortTypeEmulated:
        vport->ovsType = OVS_VPORT_TYPE_NETDEV;
        break;
    }
    RtlCopyMemory(&vport->hvPortName, &portParam->PortName,
                  sizeof (NDIS_SWITCH_PORT_NAME));
    /* For external and internal ports, 'portFriendlyName' is overwritten
     * later. */
    RtlCopyMemory(&vport->portFriendlyName, &portParam->PortFriendlyName,
                  sizeof(NDIS_SWITCH_PORT_FRIENDLYNAME));

    switch (vport->portState) {
    case NdisSwitchPortStateCreated:
        vport->ovsState = OVS_STATE_PORT_CREATED;
        break;
    case NdisSwitchPortStateTeardown:
        vport->ovsState = OVS_STATE_PORT_TEAR_DOWN;
        break;
    case NdisSwitchPortStateDeleted:
        vport->ovsState = OVS_STATE_PORT_DELETED;
        break;
    }
}


static VOID
OvsInitVportWithNicParam(POVS_SWITCH_CONTEXT switchContext,
                         POVS_VPORT_ENTRY vport,
                         PNDIS_SWITCH_NIC_PARAMETERS nicParam)
{
    ASSERT(vport->portId == nicParam->PortId);
    ASSERT(vport->ovsState == OVS_STATE_PORT_CREATED);

    UNREFERENCED_PARAMETER(switchContext);

    RtlCopyMemory(vport->permMacAddress, nicParam->PermanentMacAddress,
                  sizeof (nicParam->PermanentMacAddress));
    RtlCopyMemory(vport->currMacAddress, nicParam->CurrentMacAddress,
                  sizeof (nicParam->CurrentMacAddress));

    if (nicParam->NicType == NdisSwitchNicTypeSynthetic ||
        nicParam->NicType == NdisSwitchNicTypeEmulated) {
        RtlCopyMemory(vport->vmMacAddress, nicParam->VMMacAddress,
                      sizeof (nicParam->VMMacAddress));
        RtlCopyMemory(&vport->vmName, &nicParam->VmName,
                      sizeof (nicParam->VmName));
    } else {
        RtlCopyMemory(&vport->netCfgInstanceId, &nicParam->NetCfgInstanceId,
                      sizeof (nicParam->NetCfgInstanceId));
    }
    RtlCopyMemory(&vport->nicName, &nicParam->NicName,
                  sizeof (nicParam->NicName));
    vport->mtu = nicParam->MTU;
    vport->nicState = nicParam->NicState;
    vport->nicIndex = nicParam->NicIndex;
    vport->numaNodeId = nicParam->NumaNodeId;

    switch (vport->nicState) {
    case NdisSwitchNicStateCreated:
        vport->ovsState = OVS_STATE_NIC_CREATED;
        break;
    case NdisSwitchNicStateConnected:
        vport->ovsState = OVS_STATE_CONNECTED;
        break;
    case NdisSwitchNicStateDisconnected:
        vport->ovsState = OVS_STATE_NIC_CREATED;
        break;
    case NdisSwitchNicStateDeleted:
        vport->ovsState = OVS_STATE_PORT_CREATED;
        break;
    }
}

/*
 * --------------------------------------------------------------------------
 * Copies the relevant NDIS port properties from a virtual (pseudo) external
 * NIC to a physical (real) external NIC.
 * --------------------------------------------------------------------------
 */
static VOID
OvsInitPhysNicVport(POVS_VPORT_ENTRY physExtVport,
                    POVS_VPORT_ENTRY virtExtVport,
                    UINT32 physNicIndex)
{
    physExtVport->portType = virtExtVport->portType;
    physExtVport->portState = virtExtVport->portState;
    physExtVport->portId = virtExtVport->portId;
    physExtVport->nicState = NdisSwitchNicStateUnknown;
    physExtVport->ovsType = OVS_VPORT_TYPE_NETDEV;
    physExtVport->isExternal = TRUE;
    physExtVport->isBridgeInternal = FALSE;
    physExtVport->nicIndex = (NDIS_SWITCH_NIC_INDEX)physNicIndex;

    RtlCopyMemory(&physExtVport->hvPortName, &virtExtVport->hvPortName,
                  sizeof (NDIS_SWITCH_PORT_NAME));

    /* 'portFriendlyName' is overwritten later. */
    RtlCopyMemory(&physExtVport->portFriendlyName,
                  &virtExtVport->portFriendlyName,
                  sizeof(NDIS_SWITCH_PORT_FRIENDLYNAME));

    physExtVport->ovsState = OVS_STATE_PORT_CREATED;
}

/*
 * --------------------------------------------------------------------------
 * Initializes a tunnel vport.
 * --------------------------------------------------------------------------
 */
NTSTATUS
OvsInitTunnelVport(POVS_VPORT_ENTRY vport,
                   OVS_VPORT_TYPE ovsType,
                   UINT16 dstPort)
{
    NTSTATUS status = STATUS_SUCCESS;

    vport->isBridgeInternal = FALSE;
    vport->ovsType = ovsType;
    vport->ovsState = OVS_STATE_PORT_CREATED;
    switch (ovsType) {
    case OVS_VPORT_TYPE_GRE:
        break;
    case OVS_VPORT_TYPE_GRE64:
        break;
    case OVS_VPORT_TYPE_VXLAN:
        status = OvsInitVxlanTunnel(vport, dstPort);
        break;
    default:
        ASSERT(0);
    }
    return status;
}

/*
 * --------------------------------------------------------------------------
 * Initializes a bridge internal vport ie. a port of type
 * OVS_VPORT_TYPE_INTERNAL but not present on the Hyper-V switch.
 * --------------------------------------------------------------------------
 */
NTSTATUS
OvsInitBridgeInternalVport(POVS_VPORT_ENTRY vport)
{
    vport->isBridgeInternal = TRUE;
    vport->ovsType = OVS_VPORT_TYPE_INTERNAL;
    /* Mark the status to be connected, since there is no other initialization
     * for this port. */
    vport->ovsState = OVS_STATE_CONNECTED;
    return STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 * For external vports 'portFriendlyName' provided by Hyper-V is over-written
 * by synthetic names.
 * --------------------------------------------------------------------------
 */
static VOID
AssignNicNameSpecial(POVS_VPORT_ENTRY vport)
{
    size_t len;

    if (vport->portType == NdisSwitchPortTypeExternal) {
        if (vport->nicIndex == 0) {
            ASSERT(vport->nicIndex == 0);
            RtlStringCbPrintfW(vport->portFriendlyName.String,
                               IF_MAX_STRING_SIZE,
                               L"%s.virtualAdapter", OVS_DPPORT_EXTERNAL_NAME_W);
        } else {
            RtlStringCbPrintfW(vport->portFriendlyName.String,
                               IF_MAX_STRING_SIZE,
                               L"%s.%lu", OVS_DPPORT_EXTERNAL_NAME_W,
                               (UINT32)vport->nicIndex);
        }
    } else {
        RtlStringCbPrintfW(vport->portFriendlyName.String,
                           IF_MAX_STRING_SIZE,
                           L"%s", OVS_DPPORT_INTERNAL_NAME_W);
    }

    RtlStringCbLengthW(vport->portFriendlyName.String, IF_MAX_STRING_SIZE,
                       &len);
    vport->portFriendlyName.Length = (USHORT)len;
}


/*
 * --------------------------------------------------------------------------
 * Functionality common to any port on the Hyper-V switch. This function is not
 * to be called for a port that is not on the Hyper-V switch.
 *
 * Inserts the port into 'portIdHashArray' and caches the pointer in the
 * 'switchContext' if needed.
 *
 * For external NIC, assigns the name for the NIC.
 * --------------------------------------------------------------------------
 */
NDIS_STATUS
InitHvVportCommon(POVS_SWITCH_CONTEXT switchContext,
                  POVS_VPORT_ENTRY vport)
{
    UINT32 hash;
    ASSERT(vport->portNo == OVS_DPPORT_NUMBER_INVALID);

    switch (vport->portType) {
    case NdisSwitchPortTypeExternal:
        /*
         * Overwrite the 'portFriendlyName' of this external vport. The reason
         * for having this in common code is to be able to call it from the NDIS
         * Port callback as well as the NDIS NIC callback.
         */
        AssignNicNameSpecial(vport);

        if (vport->nicIndex == 0) {
            switchContext->virtualExternalPortId = vport->portId;
            switchContext->virtualExternalVport = vport;
        } else {
            switchContext->numPhysicalNics++;
        }
        break;
    case NdisSwitchPortTypeInternal:
        ASSERT(vport->isBridgeInternal == FALSE);

        /* Overwrite the 'portFriendlyName' of the internal vport. */
        AssignNicNameSpecial(vport);
        switchContext->internalPortId = vport->portId;
        switchContext->internalVport = vport;
        break;
    case NdisSwitchPortTypeSynthetic:
    case NdisSwitchPortTypeEmulated:
        break;
    }

    /*
     * It is important to not insert vport corresponding to virtual external
     * port into the 'portIdHashArray' since the port should not be exposed to
     * OVS userspace.
     */
    if (vport->portType == NdisSwitchPortTypeExternal &&
        vport->nicIndex == 0) {
        return NDIS_STATUS_SUCCESS;
    }

    /*
     * NOTE: OvsJhashWords has portId as "1" word. This should be ok, even
     * though sizeof(NDIS_SWITCH_PORT_ID) = 4, not 2, because the
     * hyper-v switch seems to use only 2 bytes out of 4.
     */
    hash = OvsJhashWords(&vport->portId, 1, OVS_HASH_BASIS);
    InsertHeadList(&switchContext->portIdHashArray[hash & OVS_VPORT_MASK],
                   &vport->portIdLink);
    switchContext->numHvVports++;
    return NDIS_STATUS_SUCCESS;
}

/*
 * --------------------------------------------------------------------------
 * Functionality common to any port added from OVS userspace.
 *
 * Inserts the port into 'portIdHashArray', 'ovsPortNameHashArray' and caches
 * the pointer in the 'switchContext' if needed.
 * --------------------------------------------------------------------------
 */
NDIS_STATUS
InitOvsVportCommon(POVS_SWITCH_CONTEXT switchContext,
                   POVS_VPORT_ENTRY vport)
{
    UINT32 hash;

    switch(vport->ovsType) {
    case OVS_VPORT_TYPE_VXLAN:
        ASSERT(switchContext->vxlanVport == NULL);
        switchContext->vxlanVport = vport;
        switchContext->numNonHvVports++;
        break;
    case OVS_VPORT_TYPE_INTERNAL:
        if (vport->isBridgeInternal) {
            switchContext->numNonHvVports++;
        }
    default:
        break;
    }

    /*
     * Insert the port into the hash array of ports: by port number and ovs
     * and ovs (datapath) port name.
     * NOTE: OvsJhashWords has portNo as "1" word. This is ok, because the
     * portNo is stored in 2 bytes only (max port number = MAXUINT16).
     */
    hash = OvsJhashWords(&vport->portNo, 1, OVS_HASH_BASIS);
    InsertHeadList(&gOvsSwitchContext->portNoHashArray[hash & OVS_VPORT_MASK],
                   &vport->portNoLink);

    hash = OvsJhashBytes(vport->ovsName, strlen(vport->ovsName) + 1,
                         OVS_HASH_BASIS);
    InsertHeadList(&gOvsSwitchContext->ovsPortNameHashArray[hash & OVS_VPORT_MASK],
                   &vport->ovsNameLink);

    return STATUS_SUCCESS;
}


/*
 * --------------------------------------------------------------------------
 * Provides functionality that is partly complementatry to
 * InitOvsVportCommon()/InitHvVportCommon().
 * --------------------------------------------------------------------------
 */
VOID
OvsRemoveAndDeleteVport(POVS_SWITCH_CONTEXT switchContext,
                        POVS_VPORT_ENTRY vport)
{
    BOOLEAN hvSwitchPort = FALSE;

    if (vport->isExternal) {
        if (vport->nicIndex == 0) {
            ASSERT(switchContext->numPhysicalNics == 0);
            switchContext->virtualExternalPortId = 0;
            switchContext->virtualExternalVport = NULL;
            OvsFreeMemory(vport);
            return;
        } else {
            ASSERT(switchContext->numPhysicalNics);
            switchContext->numPhysicalNics--;
            hvSwitchPort = TRUE;
        }
    }

    switch (vport->ovsType) {
    case OVS_VPORT_TYPE_INTERNAL:
        if (!vport->isBridgeInternal) {
            switchContext->internalPortId = 0;
            switchContext->internalVport = NULL;
            OvsInternalAdapterDown();
            hvSwitchPort = TRUE;
        }
        break;
    case OVS_VPORT_TYPE_VXLAN:
        OvsCleanupVxlanTunnel(vport);
        switchContext->vxlanVport = NULL;
        break;
    case OVS_VPORT_TYPE_GRE:
    case OVS_VPORT_TYPE_GRE64:
        break;
    case OVS_VPORT_TYPE_NETDEV:
        hvSwitchPort = TRUE;
    default:
        break;
    }

    RemoveEntryList(&vport->ovsNameLink);
    RemoveEntryList(&vport->portIdLink);
    RemoveEntryList(&vport->portNoLink);
    if (hvSwitchPort) {
        switchContext->numHvVports--;
    } else {
        switchContext->numNonHvVports--;
    }
    OvsFreeMemory(vport);
}


NDIS_STATUS
OvsAddConfiguredSwitchPorts(POVS_SWITCH_CONTEXT switchContext)
{
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    ULONG arrIndex;
    PNDIS_SWITCH_PORT_PARAMETERS portParam;
    PNDIS_SWITCH_PORT_ARRAY portArray = NULL;
    POVS_VPORT_ENTRY vport;

    OVS_LOG_TRACE("Enter: switchContext:%p", switchContext);

    status = OvsGetPortsOnSwitch(switchContext, &portArray);
    if (status != NDIS_STATUS_SUCCESS) {
        goto cleanup;
    }

    for (arrIndex = 0; arrIndex < portArray->NumElements; arrIndex++) {
         portParam = NDIS_SWITCH_PORT_AT_ARRAY_INDEX(portArray, arrIndex);

         if (portParam->IsValidationPort) {
             continue;
         }

         vport = (POVS_VPORT_ENTRY)OvsAllocateVport();
         if (vport == NULL) {
             status = NDIS_STATUS_RESOURCES;
             goto cleanup;
         }
         OvsInitVportWithPortParam(vport, portParam);
         status = InitHvVportCommon(switchContext, vport);
         if (status != NDIS_STATUS_SUCCESS) {
             OvsFreeMemory(vport);
             goto cleanup;
         }
    }
cleanup:
    if (status != NDIS_STATUS_SUCCESS) {
        OvsClearAllSwitchVports(switchContext);
    }

    if (portArray != NULL) {
        OvsFreeMemory(portArray);
    }
    OVS_LOG_TRACE("Exit: status: %x", status);
    return status;
}


NDIS_STATUS
OvsInitConfiguredSwitchNics(POVS_SWITCH_CONTEXT switchContext)
{
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    PNDIS_SWITCH_NIC_ARRAY nicArray = NULL;
    ULONG arrIndex;
    PNDIS_SWITCH_NIC_PARAMETERS nicParam;
    POVS_VPORT_ENTRY vport;

    OVS_LOG_TRACE("Enter: switchContext: %p", switchContext);
    /*
     * Now, get NIC list.
     */
    status = OvsGetNicsOnSwitch(switchContext, &nicArray);
    if (status != NDIS_STATUS_SUCCESS) {
        goto cleanup;
    }
    for (arrIndex = 0; arrIndex < nicArray->NumElements; ++arrIndex) {

        nicParam = NDIS_SWITCH_NIC_AT_ARRAY_INDEX(nicArray, arrIndex);

        /*
         * XXX: Check if the port is configured with a VLAN. Disallow such a
         * configuration, since we don't support tag-in-tag.
         */

        /*
         * XXX: Check if the port is connected to a VF. Disconnect the VF in
         * such a case.
         */

        if (nicParam->NicType == NdisSwitchNicTypeExternal &&
            nicParam->NicIndex != 0) {
            POVS_VPORT_ENTRY virtExtVport =
                   (POVS_VPORT_ENTRY)switchContext->virtualExternalVport;

            vport = OvsAllocateVport();
            if (vport) {
                OvsInitPhysNicVport(vport, virtExtVport,
                                    nicParam->NicIndex);
                status = InitHvVportCommon(switchContext, vport);
                if (status != NDIS_STATUS_SUCCESS) {
                    OvsFreeMemory(vport);
                    vport = NULL;
                }
            }
        } else {
            vport = OvsFindVportByPortIdAndNicIndex(switchContext,
                                                    nicParam->PortId,
                                                    nicParam->NicIndex);
        }
        if (vport == NULL) {
            OVS_LOG_ERROR("Fail to allocate vport");
            continue;
        }
        OvsInitVportWithNicParam(switchContext, vport, nicParam);
        if (nicParam->NicType == NdisSwitchNicTypeInternal) {
            OvsInternalAdapterUp(vport->portNo, &nicParam->NetCfgInstanceId);
        }
    }
cleanup:

    if (nicArray != NULL) {
        OvsFreeMemory(nicArray);
    }
    OVS_LOG_TRACE("Exit: status: %x", status);
    return status;
}

/*
 * --------------------------------------------------------------------------
 * Deletes ports added from the Hyper-V switch as well as OVS usersapce. The
 * function deletes ports in 'portIdHashArray'. This will delete most of the
 * ports that are in the 'portNoHashArray' as well. Any remaining ports
 * are deleted by walking the the 'portNoHashArray'.
 * --------------------------------------------------------------------------
 */
VOID
OvsClearAllSwitchVports(POVS_SWITCH_CONTEXT switchContext)
{
    for (UINT hash = 0; hash < OVS_MAX_VPORT_ARRAY_SIZE; hash++) {
        PLIST_ENTRY head, link, next;

        head = &(switchContext->portIdHashArray[hash & OVS_VPORT_MASK]);
        LIST_FORALL_SAFE(head, link, next) {
            POVS_VPORT_ENTRY vport;
            vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portIdLink);
            OvsRemoveAndDeleteVport(switchContext, vport);
        }
    }
    /*
     * Remove 'virtualExternalVport' as well. This port is not part of the
     * 'portIdHashArray'.
     */
    if (switchContext->virtualExternalVport) {
        OvsRemoveAndDeleteVport(switchContext,
            (POVS_VPORT_ENTRY)switchContext->virtualExternalVport);
    }

    for (UINT hash = 0; hash < OVS_MAX_VPORT_ARRAY_SIZE; hash++) {
        PLIST_ENTRY head, link, next;

        head = &(switchContext->portNoHashArray[hash & OVS_VPORT_MASK]);
        LIST_FORALL_SAFE(head, link, next) {
            POVS_VPORT_ENTRY vport;
            vport = CONTAINING_RECORD(link, OVS_VPORT_ENTRY, portNoLink);
            ASSERT(OvsIsTunnelVportType(vport->ovsType) ||
                   (vport->ovsType == OVS_VPORT_TYPE_INTERNAL &&
                    vport->isBridgeInternal));
            OvsRemoveAndDeleteVport(switchContext, vport);
        }
    }

    ASSERT(switchContext->virtualExternalVport == NULL);
    ASSERT(switchContext->internalVport == NULL);
    ASSERT(switchContext->vxlanVport == NULL);
}


NTSTATUS
OvsConvertIfCountedStrToAnsiStr(PIF_COUNTED_STRING wStr,
                                CHAR *str,
                                UINT16 maxStrLen)
{
    ANSI_STRING astr;
    UNICODE_STRING ustr;
    NTSTATUS status;
    UINT32 size;

    ustr.Buffer = wStr->String;
    ustr.Length = wStr->Length;
    ustr.MaximumLength = IF_MAX_STRING_SIZE;

    astr.Buffer = str;
    astr.MaximumLength = maxStrLen;
    astr.Length = 0;

    size = RtlUnicodeStringToAnsiSize(&ustr);
    if (size > maxStrLen) {
        return STATUS_BUFFER_OVERFLOW;
    }

    status = RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE);

    ASSERT(status == STATUS_SUCCESS);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    ASSERT(astr.Length <= maxStrLen);
    str[astr.Length] = 0;
    return STATUS_SUCCESS;
}


NTSTATUS
OvsGetExtInfoIoctl(POVS_VPORT_GET vportGet,
                   POVS_VPORT_EXT_INFO extInfo)
{
    POVS_VPORT_ENTRY vport;
    size_t len;
    LOCK_STATE_EX lockState;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN doConvert = FALSE;

    RtlZeroMemory(extInfo, sizeof (POVS_VPORT_EXT_INFO));
    NdisAcquireRWLockRead(gOvsSwitchContext->dispatchLock, &lockState,
                          NDIS_RWL_AT_DISPATCH_LEVEL);
    if (vportGet->portNo == 0) {
        StringCbLengthA(vportGet->name, OVS_MAX_PORT_NAME_LENGTH - 1, &len);
        vport = OvsFindVportByHvName(gOvsSwitchContext, vportGet->name);
    } else {
        vport = OvsFindVportByPortNo(gOvsSwitchContext, vportGet->portNo);
    }
    if (vport == NULL || (vport->ovsState != OVS_STATE_CONNECTED &&
                          vport->ovsState != OVS_STATE_NIC_CREATED)) {
        NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);
        if (vportGet->portNo) {
            OVS_LOG_WARN("vport %u does not exist any more", vportGet->portNo);
        } else {
            OVS_LOG_WARN("vport %s does not exist any more", vportGet->name);
        }
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto ext_info_done;
    }
    extInfo->dpNo = vportGet->dpNo;
    extInfo->portNo = vport->portNo;
    RtlCopyMemory(extInfo->macAddress, vport->currMacAddress,
                  sizeof (vport->currMacAddress));
    RtlCopyMemory(extInfo->permMACAddress, vport->permMacAddress,
                  sizeof (vport->permMacAddress));
    if (vport->ovsType == OVS_VPORT_TYPE_NETDEV) {
        RtlCopyMemory(extInfo->vmMACAddress, vport->vmMacAddress,
                      sizeof (vport->vmMacAddress));
    }
    extInfo->nicIndex = vport->nicIndex;
    extInfo->portId = vport->portId;
    extInfo->type = vport->ovsType;
    extInfo->mtu = vport->mtu;
    /*
     * TO be revisit XXX
     */
    if (vport->ovsState == OVS_STATE_NIC_CREATED) {
       extInfo->status = OVS_EVENT_CONNECT | OVS_EVENT_LINK_DOWN;
    } else if (vport->ovsState == OVS_STATE_CONNECTED) {
       extInfo->status = OVS_EVENT_CONNECT | OVS_EVENT_LINK_UP;
    } else {
       extInfo->status = OVS_EVENT_DISCONNECT;
    }
    if (extInfo->type == OVS_VPORT_TYPE_NETDEV &&
        (vport->ovsState == OVS_STATE_NIC_CREATED  ||
         vport->ovsState == OVS_STATE_CONNECTED)) {
        doConvert = TRUE;
    } else {
        extInfo->vmUUID[0] = 0;
        extInfo->vifUUID[0] = 0;
    }
    NdisReleaseRWLock(gOvsSwitchContext->dispatchLock, &lockState);
    NdisReleaseSpinLock(gOvsCtrlLock);
    if (doConvert) {
        status = OvsConvertIfCountedStrToAnsiStr(&vport->portFriendlyName,
                                                 extInfo->name,
                                                 OVS_MAX_PORT_NAME_LENGTH);
        if (status != STATUS_SUCCESS) {
            OVS_LOG_INFO("Fail to convert NIC name.");
            extInfo->vmUUID[0] = 0;
        }

        status = OvsConvertIfCountedStrToAnsiStr(&vport->vmName,
                                                 extInfo->vmUUID,
                                                 OVS_MAX_VM_UUID_LEN);
        if (status != STATUS_SUCCESS) {
            OVS_LOG_INFO("Fail to convert VM name.");
            extInfo->vmUUID[0] = 0;
        }

        status = OvsConvertIfCountedStrToAnsiStr(&vport->nicName,
                                                 extInfo->vifUUID,
                                                 OVS_MAX_VIF_UUID_LEN);
        if (status != STATUS_SUCCESS) {
            OVS_LOG_INFO("Fail to convert nic UUID");
            extInfo->vifUUID[0] = 0;
        }
        /*
         * for now ignore status
         */
        status = STATUS_SUCCESS;
    }

ext_info_done:
    return status;
}

/*
 * --------------------------------------------------------------------------
 *  Command Handler for 'OVS_WIN_NETDEV_CMD_GET'.
 * --------------------------------------------------------------------------
 */
NTSTATUS
OvsGetNetdevCmdHandler(POVS_USER_PARAMS_CONTEXT usrParamsCtx,
                       UINT32 *replyLen)
{
    NTSTATUS status = STATUS_SUCCESS;
    POVS_MESSAGE msgIn = (POVS_MESSAGE)usrParamsCtx->inputBuffer;
    POVS_MESSAGE msgOut = (POVS_MESSAGE)usrParamsCtx->outputBuffer;
    NL_ERROR nlError = NL_ERROR_SUCCESS;
    OVS_VPORT_GET vportGet;
    OVS_VPORT_EXT_INFO info;

    static const NL_POLICY ovsNetdevPolicy[] = {
        [OVS_WIN_NETDEV_ATTR_NAME] = { .type = NL_A_STRING,
                                       .minLen = 2,
                                       .maxLen = IFNAMSIZ },
    };
    PNL_ATTR netdevAttrs[ARRAY_SIZE(ovsNetdevPolicy)];

    /* input buffer has been validated while validating transaction dev op. */
    ASSERT(usrParamsCtx->inputBuffer != NULL &&
           usrParamsCtx->inputLength > sizeof *msgIn);

    if (msgOut == NULL || usrParamsCtx->outputLength < sizeof *msgOut) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    if (!NlAttrParse((PNL_MSG_HDR)msgIn,
        NLMSG_HDRLEN + GENL_HDRLEN + OVS_HDRLEN,
        NlMsgAttrsLen((PNL_MSG_HDR)msgIn),
        ovsNetdevPolicy, netdevAttrs, ARRAY_SIZE(netdevAttrs))) {
        return STATUS_INVALID_PARAMETER;
    }

    OvsAcquireCtrlLock();

    vportGet.portNo = 0;
    RtlCopyMemory(&vportGet.name, NlAttrGet(netdevAttrs[OVS_VPORT_ATTR_NAME]),
                  NlAttrGetSize(netdevAttrs[OVS_VPORT_ATTR_NAME]));

    status = OvsGetExtInfoIoctl(&vportGet, &info);
    if (status == STATUS_DEVICE_DOES_NOT_EXIST) {
        nlError = NL_ERROR_NODEV;
        OvsReleaseCtrlLock();
        goto cleanup;
    }

    status = CreateNetlinkMesgForNetdev(&info, msgIn,
                 usrParamsCtx->outputBuffer, usrParamsCtx->outputLength,
                 gOvsSwitchContext->dpNo);
    if (status == STATUS_SUCCESS) {
        *replyLen = msgOut->nlMsg.nlmsgLen;
    }
    OvsReleaseCtrlLock();

cleanup:
    if (nlError != NL_ERROR_SUCCESS) {
        POVS_MESSAGE_ERROR msgError = (POVS_MESSAGE_ERROR)
            usrParamsCtx->outputBuffer;

        BuildErrorMsg(msgIn, msgError, nlError);
        *replyLen = msgError->nlMsg.nlmsgLen;
    }

    return STATUS_SUCCESS;
}


/*
 * --------------------------------------------------------------------------
 *  Utility function to construct an OVS_MESSAGE for the specified vport. The
 *  OVS_MESSAGE contains the output of a netdev command.
 * --------------------------------------------------------------------------
 */
static NTSTATUS
CreateNetlinkMesgForNetdev(POVS_VPORT_EXT_INFO info,
                           POVS_MESSAGE msgIn,
                           PVOID outBuffer,
                           UINT32 outBufLen,
                           int dpIfIndex)
{
    NL_BUFFER nlBuffer;
    BOOLEAN ok;
    OVS_MESSAGE msgOut;
    PNL_MSG_HDR nlMsg;
    UINT32 netdevFlags = 0;

    NlBufInit(&nlBuffer, outBuffer, outBufLen);

    BuildReplyMsgFromMsgIn(msgIn, &msgOut, 0);
    msgOut.ovsHdr.dp_ifindex = dpIfIndex;

    ok = NlMsgPutHead(&nlBuffer, (PCHAR)&msgOut, sizeof msgOut);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailU32(&nlBuffer, OVS_WIN_NETDEV_ATTR_PORT_NO,
                         info->portNo);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailU32(&nlBuffer, OVS_WIN_NETDEV_ATTR_TYPE, info->type);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailString(&nlBuffer, OVS_WIN_NETDEV_ATTR_NAME,
                            info->name);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailUnspec(&nlBuffer, OVS_WIN_NETDEV_ATTR_MAC_ADDR,
             (PCHAR)info->macAddress, sizeof (info->macAddress));
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ok = NlMsgPutTailU32(&nlBuffer, OVS_WIN_NETDEV_ATTR_MTU, info->mtu);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (info->status != OVS_EVENT_CONNECT) {
        netdevFlags = OVS_WIN_NETDEV_IFF_UP;
    }
    ok = NlMsgPutTailU32(&nlBuffer, OVS_WIN_NETDEV_ATTR_IF_FLAGS,
                         netdevFlags);
    if (!ok) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * XXX: add netdev_stats when we have the definition available in the
     * kernel.
     */

    nlMsg = (PNL_MSG_HDR)NlBufAt(&nlBuffer, 0, 0);
    nlMsg->nlmsgLen = NlBufSize(&nlBuffer);

    return STATUS_SUCCESS;
}

static __inline VOID
OvsWaitActivate(POVS_SWITCH_CONTEXT switchContext, ULONG sleepMicroSec)
{
    while ((!switchContext->isActivated) &&
          (!switchContext->isActivateFailed)) {
        /* Wait for the switch to be active and
         * the list of ports in OVS to be initialized. */
        NdisMSleep(sleepMicroSec);
    }
}
