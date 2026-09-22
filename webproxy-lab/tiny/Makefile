CC = gcc
CFLAGS = -O0 -Wall -I . -g
# CFLAGS = -O2 -Wall -I . -g
OS = $(shell uname -s)
ARCH = $(shell uname -m)
PLATFORM = $(OS)-$(ARCH)
BUILD_DIR = .build/$(PLATFORM)
TINY_BIN = $(BUILD_DIR)/tiny.bin
CSAPP_OBJ = $(BUILD_DIR)/csapp.o
PORT ?= 8000

# 이 플래그는 Linux 환경에서 Pthreads 라이브러리를 포함합니다.
# 다른 시스템에서는 다른 설정이 필요할 수 있습니다.
LIB = -lpthread

.PHONY: all clean run tiny

all: tiny

$(BUILD_DIR):
	mkdir -p $@

$(CSAPP_OBJ): csapp.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c csapp.c -o $@

$(TINY_BIN): tiny.c $(CSAPP_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tiny.c $(CSAPP_OBJ) $(LIB)

tiny: $(TINY_BIN)
	printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'DIR=$$(CDPATH= cd -- "$$(dirname "$$0")" && pwd)' \
		'BIN="$$DIR/$(TINY_BIN)"' \
		'make -s -C "$$DIR" "$(TINY_BIN)" >/dev/null' \
		'exec "$$BIN" "$$@"' > $@
	chmod +x $@

run: tiny
	./tiny $(PORT)

clean:
	rm -rf .build *.o tiny *~
