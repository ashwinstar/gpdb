#!/bin/bash

totalBytes=1000000

for size in 2 4 8 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768
do
  ((numOfMessages=totalBytes / $size))
  echo "========Run num=$numOfMessages size=$size ========"
  ./shmbufftest d
  ./shmbufftest s $numOfMessages $size &
  ./shmbufftest r
  echo "=============================="
done
