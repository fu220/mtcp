#-------------------------------------------------------------------------#
# DPDK Interface Kernel Module Makefile
# Simplified version for DPDK 20.11+
#-------------------------------------------------------------------------#

# DPDK paths - adjust to your DPDK build directory
DPDK_BUILD_DIR ?= ../dpdk/build

# Kernel module build
obj-m = dpdk_iface.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD ?= $(shell pwd)

# Application name
appname = dpdk_iface_main

#-------------------------------------------------------------------------#
# Verbosity control
ifeq ($(V),) # no echo
	export MSG = @echo
	export HIDE = @
else
	export MSG = @\#
	export HIDE =
endif

#-------------------------------------------------------------------------#
# Simple DPDK build using pkg-config
all: $(appname) modules

modules:
	$(MSG) "  Building kernel module..."
	$(HIDE) $(MAKE) -C $(KDIR) M=$(PWD) modules

$(appname): $(appname).c
	$(MSG) "  CC $<"
	$(HIDE) $(CC) -g -o $(appname) $(appname).c \
		$$(pkg-config --cflags --libs libdpdk) \
		-lpthread -ldl

clean:
	$(MSG) "  Cleaning kernel module..."
	$(HIDE) $(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MSG) "  CLEAN  $(appname)"
	$(HIDE) rm -rf *~ *.o *.ko $(appname) .*.cmd *.mod.c *.mod .tmp_versions Module.symvers modules.order

run: all
	sudo ./$(appname)

# Set pkg-config path and run
run-with-dpdk: all
	PKG_CONFIG_PATH=$(DPDK_BUILD_DIR)/lib/x86_64-linux-gnu/pkgconfig sudo ./$(appname)

.PHONY: all modules clean run run-with-dpdk
#-------------------------------------------------------------------------#
