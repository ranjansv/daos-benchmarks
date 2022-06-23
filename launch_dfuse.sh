#!/bin/bash
MOUNTPOINT=$1
POOL_UUID=$2
CONT_UUID=$3

mkdir -p $MOUNTPOINT
#Enabling cache causes readers to stall at BeginStep().
#dfuse --mountpoint=$MOUNTPOINT --pool=$POOL_UUID --container=$CONT_UUID &
dfuse --mountpoint=$MOUNTPOINT --pool=$POOL_UUID --container=$CONT_UUID --disable-caching &
