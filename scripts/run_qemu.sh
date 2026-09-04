#!/bin/bash
set -e

KERNEL=""
if [ -f "build/bzImage" ]; then
    KERNEL="build/bzImage"
elif [ -f "build/vmlinuz" ]; then
    KERNEL="build/vmlinuz"
else
    echo "[!] Kernel not found in build/ (looked for bzImage and vmlinuz)."
    exit 1
fi

INITRAMFS="build/initramfs.cpio.gz"

if [ ! -f "$INITRAMFS" ]; then
    echo "[!] initramfs.cpio.gz missing from build/."
    exit 1
fi

qemu-system-x86_64 \
    -m 512M \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 rdinit=/init quiet loglevel=3" \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -nographic
