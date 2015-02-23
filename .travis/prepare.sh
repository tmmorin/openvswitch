#!/bin/bash

sudo apt-get update -qq
sudo apt-get install -qq libssl-dev llvm-dev
sudo apt-get install -qq gcc-multilib

git clone git://git.kernel.org/pub/scm/devel/sparse/chrisl/sparse.git
cd sparse && make && sudo make install PREFIX=/usr && cd ..
