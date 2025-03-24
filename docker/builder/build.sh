#!/usr/bin/env bash
set -e

#ccache -s # uncomment to display CCache statistics
# mkdir -p /server/build_docker
# cd /server/build_docker
CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=$CURDIR/../../build_docker
mkdir -p $BUILD_DIR
cd $BUILD_DIR
cmake -G Ninja .. "-DCMAKE_C_COMPILER=$(command -v clang-16)" "-DCMAKE_CXX_COMPILER=$(command -v clang++-16)"

# Set the number of build jobs to the half of number of virtual CPU cores (rounded up).
# By default, ninja use all virtual CPU cores, that leads to very high memory consumption without much improvement in build time.
# Note that modern x86_64 CPUs use two-way hyper-threading (as of 2018).
# Without this option my laptop with 16 GiB RAM failed to execute build due to full system freeze.
NUM_JOBS=$(( ($(nproc || grep -c ^processor /proc/cpuinfo) + 1) / 2 ))

ninja -j $NUM_JOBS
