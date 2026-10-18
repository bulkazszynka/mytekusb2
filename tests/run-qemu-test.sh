#!/bin/bash
# End-to-end test of snd-usb-mytek against an emulated DAC (tests/mytek-emu.c)
# in a QEMU guest. Needs a kernel tree built with SND, USBIP_VHCI_HCD (and
# preferably KASAN/PROVE_LOCKING), see documentation/testing.md.
#   tests/run-qemu-test.sh <kernel-build-dir> <workdir>
set -euo pipefail
KB=$1; W=$2
T=$(cd "$(dirname "$0")" && pwd); SRC=$(dirname "$T")
R=$W/initramfs
rm -rf "$R"; mkdir -p $R/{bin,data,lib/x86_64-linux-gnu,lib64,usr/share,proc,sys,dev,tmp}
cp /usr/bin/busybox $R/bin/; for c in $(/usr/bin/busybox --list | grep -vx busybox); do ln -sf busybox $R/bin/$c; done
cp /usr/bin/aplay $R/bin/; for l in $(ldd /usr/bin/aplay | grep -o '/[^ ]*'); do mkdir -p $R$(dirname $l); cp -L $l $R$l; done
cp -r /usr/share/alsa $R/usr/share/
gcc -O2 -static -o $R/mytek-emu $T/mytek-emu.c
gcc -O2 -o $R/alsa-rates $T/alsa-rates.c -l:libasound.so.2
cp $T/guest-init.sh $R/init
(cd $R/data
 python3 $T/gen-ramp.py S32_LE 48000 2 s32_48k.raw; python3 $T/gen-ramp.py S24_LE 44100 2 s24_44k.raw
 python3 $T/gen-ramp.py S32_LE 96000 2 s32_96k.raw; python3 $T/gen-ramp.py S32_LE 192000 2 s32_192k.raw
 python3 $T/gen-ramp.py S24_LE 176400 2 s24_176k.raw
 python3 $T/gen-ramp.py S32_LE 352800 2 s32_352k.raw
 python3 $T/gen-ramp.py DSD_U8 352800 2 dsd64_u8.raw
 python3 $T/gen-ramp.py DSD_U16_LE 176400 2 dsd64_u16le.raw; python3 $T/gen-ramp.py DSD_U16_BE 176400 2 dsd64_u16be.raw
 python3 $T/gen-ramp.py DSD_U32_LE 88200 2 dsd64_u32le.raw; python3 $T/gen-ramp.py DSD_U32_BE 88200 2 dsd64_u32be.raw
 python3 $T/gen-ramp.py DSD_U8 705600 2 dsd128_u8.raw
 python3 $T/gen-ramp.py DSD_U16_LE 352800 2 dsd128_u16le.raw; python3 $T/gen-ramp.py DSD_U16_BE 352800 2 dsd128_u16be.raw
 python3 $T/gen-ramp.py DSD_U32_LE 176400 2 dsd128_u32le.raw; python3 $T/gen-ramp.py DSD_U32_BE 176400 2 dsd128_u32be.raw)
rm -rf $W/mod; cp -r $SRC $W/mod; rm -rf $W/mod/.git $W/mod/cert
make -C $W/mod KDIR=$KB modules >/dev/null
cp $W/mod/snd-usb-mytek.ko $R/
(cd $R && find . | cpio -o -H newc 2>/dev/null | gzip -1) > $W/initramfs.gz
timeout 900 qemu-system-x86_64 -accel tcg,thread=multi -m 2048 -smp 4 -nographic -no-reboot \
	-kernel $KB/arch/x86/boot/bzImage -initrd $W/initramfs.gz \
	-append "console=ttyS0 panic=-1 quiet" | tee $W/qemu.log
