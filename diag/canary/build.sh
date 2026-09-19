#!/bin/sh
# Assemble the canary and produce the RT-11 .SAV image.  Needs macro11 in PATH.
set -e
macro11 CANARY.MAC -o canary.obj -l canary.lst
python3 obj2sav.py canary.obj CANARY.SAV 1000
