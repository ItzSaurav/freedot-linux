#!/bin/bash
# FreeDot Linux — QEMU Boot Runner
# Boots the compiled Linux kernel with the FreeDot initramfs in headless/serial mode.

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

# Boot parameters:
# -m 512M: allocate 512MB of RAM (within our <= 600MB idle budget)
# -kernel: the compiled Linux LTS kernel binary
# -initrd: our custom rootfs containing /init (PID 1)
# -append: kernel boot flags (serial console output on ttyS0, execute /init first)
# -netdev & -device: virtual e1000 network adapter for networking tests
# -nographic: redirects console output straight to the terminal
qemu-system-x86_64 \
    -m 512M \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 rdinit=/init quiet loglevel=3" \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -nographic