#!/usr/bin/env bash
# One node of a four-vCPU distributed guest; no logs, pidfiles or overlays.
set -euo pipefail
node=${1:?Usage: gvm-device-node.sh 0|1 [additional QEMU arguments]}
shift
case "$node" in 0|1) ;; *) echo 'node must be 0 or 1' >&2; exit 2 ;; esac
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export LOOM_DEVICE_AGGREGATION=1
args=(
    -enable-kvm -machine kernel-irqchip=split -smp 4 -m 4G
    -display none -monitor none
    -qmp tcp:127.0.0.1:4444,server=on,wait=off
    -local-cpu "cpus=2,start=$((node * 2)),iplist=${GVM_IPLIST:-172.0.1.2 172.0.1.3}"
    -drive "file=${GVM_ROOT_IMAGE:-/home/xtl/start/debian-12-40G.raw},format=raw,id=rootfs,if=none,file.locking=off"
    -device ide-hd,drive=rootfs,id=rootfs
    -device "edu,id=edu-owner-$node,addr=0$((5 + node)).0,loom-aggregate=on,loom-numa-node=$node"
)
# Some host builds omit libslirp; the serial console needs no user network.
case ${GVM_USER_NET:-1} in
    1) args+=(-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22
              -device virtio-net-pci,netdev=net0,addr=03.0) ;;
    0) ;;
    *) echo 'GVM_USER_NET must be 0 or 1' >&2; exit 2 ;;
esac
if [[ $node == 0 ]]; then
    args+=(
        -serial tcp:127.0.0.1:5555,server=on,wait=off
        -kernel "${GVM_GUEST_KERNEL:-/home/xtl/start/bzImage}"
        -append 'notsc nokaslr console=ttyS0 root=/dev/sda1 memmap=64K$0x08000000'
    )
else
    args+=(-serial null)
fi
if [[ ${GVM_QUIET:-0} == 1 ]]; then
    exec "$root/build/qemu-system-x86_64" "${args[@]}" "$@" >/dev/null
fi
exec "$root/build/qemu-system-x86_64" "${args[@]}" "$@"
