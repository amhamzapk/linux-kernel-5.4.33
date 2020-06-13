# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Realtek network device drivers.
#
r8169-objs += r8169_main.o r8169_firmware.o
obj-$(CONFIG_R8169) += r8169.o

all:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
