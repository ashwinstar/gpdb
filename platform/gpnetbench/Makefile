SHELL=/bin/bash

default: all

BLD_TOP=$(shell echo `pwd`/../../)
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty

EXT_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/

CC=$(strip $(BLD_CC))

SERVER_OBJS=gpnetbenchServer.o
CLIENT_OBJS=gpnetbench.o

CFLAGS=-Werror -Wall -g -O2

all: gpnetbenchServer gpnetbench

gpnetbenchServer: $(SERVER_OBJS)
	$(CC) -o gpnetbenchServer $(SERVER_OBJS)

gpnetbench: $(CLIENT_OBJS)
	$(CC) -o gpnetbench $(CLIENT_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -rf *.o gpnetbenchServer gpnetbench
