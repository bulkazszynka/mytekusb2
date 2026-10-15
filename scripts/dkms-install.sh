#!/bin/bash
# Install (or remove) snd-usb-mytek as a DKMS module, so it is rebuilt
# automatically for every new kernel.
#
#   sudo ./scripts/dkms-install.sh            install / upgrade
#   sudo ./scripts/dkms-install.sh --remove   remove all DKMS versions
#
# Requirements: dkms, gcc/make and the kernel headers of the running kernel
# (Debian/Ubuntu: sudo apt install dkms linux-headers-$(uname -r)).
set -euo pipefail

NAME=snd-usb-mytek
SRC_DIR=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$SRC_DIR/dkms.conf")
KVER=${KVER:-$(uname -r)}

if [ "$(id -u)" -ne 0 ]; then
	exec sudo KVER="$KVER" "$0" "$@"
fi

say() { echo "==> $*"; }
warn() { echo "WARNING: $*" >&2; }

remove_all() {
	local v
	for v in $(dkms status -m $NAME 2>/dev/null | sed -n "s|^$NAME/\([^,:]*\).*|\1|p" | sort -u); do
		say "removing DKMS $NAME/$v"
		dkms remove -m $NAME -v "$v" --all || true
		rm -rf "/usr/src/$NAME-$v"
	done
}

if [ "${1:-}" = "--remove" ]; then
	remove_all
	depmod -a || true
	say "done"
	exit 0
fi

command -v dkms >/dev/null || { echo "dkms is not installed (sudo apt install dkms)"; exit 1; }
if [ ! -e "/lib/modules/$KVER/build" ]; then
	echo "Kernel headers for $KVER are missing (sudo apt install linux-headers-$KVER)"
	exit 1
fi

# Older manual installs ('make install') would shadow or confuse the DKMS one.
old=$(find /lib/modules -path '*/extra*/snd-usb-mytek.ko*' 2>/dev/null || true)
if [ -n "$old" ]; then
	backup=/var/backups/snd-usb-mytek-manual
	say "moving manually installed module(s) to $backup:"
	for f in $old; do
		echo "    $f"
		mkdir -p "$backup/$(echo "$f" | cut -d/ -f4)"
		mv "$f" "$backup/$(echo "$f" | cut -d/ -f4)/"
	done
fi

remove_all

say "copying sources to /usr/src/$NAME-$VERSION"
mkdir -p "/usr/src/$NAME-$VERSION"
cp "$SRC_DIR"/*.c "$SRC_DIR"/*.h "$SRC_DIR"/Makefile "$SRC_DIR"/dkms.conf \
	"/usr/src/$NAME-$VERSION/"
rm -f "/usr/src/$NAME-$VERSION/version.h" "/usr/src/$NAME-$VERSION"/*.mod.c

say "building and installing for $KVER"
dkms add -m $NAME -v "$VERSION"
if ! dkms install -m $NAME -v "$VERSION" -k "$KVER"; then
	echo "DKMS build failed, see /var/lib/dkms/$NAME/$VERSION/build/make.log"
	exit 1
fi
modfile=$(find "/lib/modules/$KVER/updates/dkms" -name "$NAME.ko*" 2>/dev/null | head -1)

# Secure Boot: the module must be signed with a key enrolled in MOK.
if command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -q enabled; then
	if [ -n "$modfile" ] && ! modinfo "$modfile" | grep -q '^sig_id'; then
		warn "Secure Boot is enabled but the module is NOT signed; it will not load."
		warn "Configure a MOK signing key for DKMS (see README.md, "Secure Boot")."
	else
		say "Secure Boot enabled; module is signed ($(modinfo -F signer "$modfile"))"
	fi
fi

if [ ! -e /lib/firmware/mytek/mytekap.ihx ] || [ ! -e /lib/firmware/mytek/mytekcf.bin ] ||
   [ ! -e /lib/firmware/mytek/mytekl2.ihx ]; then
	warn "firmware files missing in /lib/firmware/mytek (see FIRMWARE)"
fi

if lsmod | grep -q '^snd_usb_mytek'; then
	if modprobe -r snd_usb_mytek 2>/dev/null; then
		modprobe snd_usb_mytek
		say "reloaded the module"
	else
		say "the old module is in use; power-cycle the Mytek (or reboot) to use the new one"
	fi
else
	modprobe snd_usb_mytek || true
fi
dkms status -m $NAME
say "installed $NAME $(modinfo -F version "$modfile" 2>/dev/null) for $KVER: $modfile"
