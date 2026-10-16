obj-m := capacity_probe.o
ccflags-y += -I$(SCST_TREE)/scst/include

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		KBUILD_EXTRA_SYMBOLS=$(SCST_TREE)/scst/src/Module.symvers modules

.PHONY: all
