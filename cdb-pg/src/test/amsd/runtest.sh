#!/bin/bash

echo "LAUNCHING AMS..."
amsd &
AMSPID=$!

#just wait a bit for the amsd to get setup
sleep 2

echo "LAUNCHING TEST..."
./test-amsd
echo "TEST FINISHED..."

echo "Dumping AMS stats..."
kill -s INFO $AMSPID 

echo "KILLING AMS (pid=$AMSPID)..."
kill $AMSPID
