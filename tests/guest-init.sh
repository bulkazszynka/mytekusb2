#!/bin/sh
# Runs as /init inside the QEMU test guest.
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
mkdir -p /tmp; mount -t tmpfs tmp /tmp
export LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu
log() { echo "=== TEST: $*"; }
emu_stats() { kill -USR1 $EMU; sleep 0.5; }
wait_card() {
	for i in $(seq 1 50); do [ -e /proc/asound/card0/mytek ] && return 0; sleep 0.2; done
	echo "card did not appear"; return 1
}
play() { # fmt rate file [extra aplay args]
	log "play $1 @ $2"
	aplay -q -D hw:0,0 -f $1 -r $2 -c 2 -t raw --buffer-size=$(($2 / 2)) $4 /data/$3 ; echo "aplay exit=$?"
	grep -v -E '^(driver_version|state|rate|zlp_fill|out_lead|in_packet_size|out_packet_size|packets_per_sec):' /proc/asound/card0/mytek | tr '\n' ' '; echo
	dmesg -c | tee -a /tmp/klog | grep -v -E 'callbacks suppressed' | tail -8
	emu_stats
}

insmod /snd-usb-mytek.ko || echo "insmod failed"
dmesg -n 4
dmesg -c > /tmp/klog

log "basic: attach emulated device"
/mytek-emu & EMU=$!
wait_card && cat /proc/asound/cards
play S32_LE 48000 s32_48k.raw
play S24_LE 44100 s24_44k.raw
play S32_LE 96000 s32_96k.raw
play S32_LE 192000 s32_192k.raw
play S24_LE 176400 s24_176k.raw
log "mono must be refused"
aplay -q -D hw:0,0 -f S32_LE -r 48000 -c 1 -t raw -d 1 /dev/zero; echo "aplay mono exit=$?"
log "pause/resume and small period"
aplay -q -D hw:0,0 -f S32_LE -r 48000 -c 2 -t raw --period-size=600 --buffer-size=4800 /data/s32_48k.raw; echo "aplay exit=$?"
emu_stats
kill $EMU; sleep 1

log "zero-length feedback packets every 37 packets (zlp_fill=1)"
/mytek-emu -z 37 & EMU=$!; wait_card
play S32_LE 96000 s32_96k.raw
echo 2 > /sys/module/snd_usb_mytek/parameters/zlp_fill
log "same with zlp_fill=2 (driver 1.x)"
play S32_LE 96000 s32_96k.raw
echo 1 > /sys/module/snd_usb_mytek/parameters/zlp_fill
kill $EMU; sleep 1

log "feedback packet errors every 50 packets + out packet errors every 100 urbs"
/mytek-emu -e 50 -x 100 & EMU=$!; wait_card
play S32_LE 48000 s32_48k.raw
log "out_lead=0 (driver 1.x pacing)"
echo 0 > /sys/module/snd_usb_mytek/parameters/out_lead
play S32_LE 48000 s32_48k.raw
echo 6 > /sys/module/snd_usb_mytek/parameters/out_lead
kill $EMU; sleep 1

log "unplug during playback"
/mytek-emu -d 2 & EMU=$!; wait_card
aplay -q -D hw:0,0 -f S32_LE -r 192000 -c 2 -t raw /data/s32_192k.raw; echo "aplay exit=$? (error expected)"
sleep 2
cat /proc/asound/cards
log "replug after unplug"
/mytek-emu & EMU=$!; wait_card && cat /proc/asound/cards
play S32_LE 48000 s32_48k.raw
log "unplug while idle"
kill $EMU; sleep 2
log "rmmod"
rmmod snd_usb_mytek; echo "rmmod exit=$?"
sleep 1
log "kernel log check"
dmesg >> /tmp/klog
grep -E 'BUG:|KASAN|WARNING:|possible circular|possible recursive|INFO: task|Call Trace|UBSAN' /tmp/klog && echo "KERNEL-PROBLEMS-FOUND" || echo "KERNEL-LOG-CLEAN"
log "done"
poweroff -f
