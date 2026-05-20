# Сборка модуля ядра и пользовательской программы.
# Использование:
#   make            — соберёт всё
#   make module     — только .ko
#   make test       — только myfs_test
#   make clean      — почистить

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m       := myfs.o
myfs-objs   := myfs_main.o myfs_super.o myfs_inode.o myfs_file.o

CXX         ?= g++
CXXFLAGS    ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

.PHONY: all module test clean help

all: module test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: myfs_test

myfs_test: myfs_test.cpp myfs.h
	$(CXX) $(CXXFLAGS) -o $@ myfs_test.cpp

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f myfs_test

help:
	@echo "Targets: all, module, test, clean"
	@echo "Variables: KDIR=$(KDIR) CXX=$(CXX)"
