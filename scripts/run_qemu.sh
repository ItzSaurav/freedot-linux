#!/bin/bash
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "[FreeDot] Launching QEMU..."
qemu-system-x86_64 \
    -kernel "$BUILD_DIR/vmlinuz" \
    -initrd "$BUILD_DIR/initramfs.cpio.gz" \
    -append "console=ttyS0 quiet" \
    -nographic \
    -m 512M
