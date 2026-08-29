# FreeDot Linux

A reproducible, minimal Linux system built to explore Linux bootstrapping, userspace initialization, networking, storage, and containerization primitives under strict resource constraints.

## Constraints & Targets
- Target Image Size: <= 600 MB
- Target Idle RAM: <= 600 MB
- Target Architecture: x86_64
- Host Dev RAM Limit: 2 GB
- Kernel: Upstream Linux LTS
- Init/Userspace: Minimal custom init / BusyBox
