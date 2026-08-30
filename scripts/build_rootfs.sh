#!/bin/bash
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
ROOTFS_DIR="$BUILD_DIR/rootfs"

echo "[FreeDot] Packing rootfs into initramfs.cpio.gz..."
cd "$ROOTFS_DIR"
find . -print0 | cpio --null -ov --format=newc | gzip -9 > "$BUILD_DIR/initramfs.cpio.gz"
echo "[FreeDot] Done! Generated $BUILD_DIR/initramfs.cpio.gz ($(ls -lh "$BUILD_DIR/initramfs.cpio.gz" | awk '{print $5}'))"
