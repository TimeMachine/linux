#!/bin/bash
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabi- 
make zImage -j4 >/dev/null  2> error
cat error
