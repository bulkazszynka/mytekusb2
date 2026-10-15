obj-m += snd-usb-mytek.o
snd-usb-mytek-y := chip.o comm.o control.o firmware.o pcm.o

ifneq ($(KBUILD_EXTMOD),)
# invoked by kbuild
ifeq ($(MYTEK_DEBUG),1)
ccflags-y += -g -DDEBUG
endif

else
# invoked from the command line or by DKMS

KERNEL_BUILD ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNEL_BUILD)/build


all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

debug:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MYTEK_DEBUG=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f version.h

# Plain install for the running kernel only; prefer scripts/dkms-install.sh
# which rebuilds the module automatically on every kernel update.
install:
	rm -f /lib/modules/$(KERNEL_BUILD)/extras/snd-usb-mytek.ko
	mkdir -p /lib/modules/$(KERNEL_BUILD)/extra
	cp snd-usb-mytek.ko /lib/modules/$(KERNEL_BUILD)/extra
	depmod -a $(KERNEL_BUILD)

uninstall:
	rm -f /lib/modules/$(KERNEL_BUILD)/extra/snd-usb-mytek.ko
	depmod -a $(KERNEL_BUILD)

kremove:
	for x in $(shell find /lib/modules/$(KERNEL_BUILD) -iname snd-usb-mytek\*); do \
	rm -f $$x; \
	done

dkms-install:
	./scripts/dkms-install.sh

dkms-uninstall:
	./scripts/dkms-install.sh --remove

.PHONY: all modules debug clean install uninstall kremove dkms-install dkms-uninstall
endif
