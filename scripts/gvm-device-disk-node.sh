#!/usr/bin/env bash
# Existing dedicated owner disks; this launcher never creates or formats them.
set -euo pipefail
node=${1:?Usage: gvm-device-disk-node.sh 0|1 [method=tcp|rdma] [QEMU args]}
shift
method=${1:-method=${GVM_METHOD:-rdma}}
if [[ $method == method=* ]]; then
    if [[ $# -gt 0 ]]; then shift; fi
else
    method=method=${GVM_METHOD:-rdma}
fi
case "$node" in 0) addr=09 ;; 1) addr=0a ;; *) echo 'node must be 0 or 1' >&2; exit 2 ;; esac
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
disk=${GVM_DATA_IMAGE:-/home/xtl/start/gvm-device-data-node$node.raw}
if [[ ! -f $disk ]]; then
    echo "Existing dedicated data image is missing: $disk" >&2
    exit 2
fi
export GVM_PRIVATE_IO=${GVM_PRIVATE_IO:-1}
export GVM_PRIVATE_IO_ZCOPY=${GVM_PRIVATE_IO_ZCOPY:-direct}
export GVM_MACHINE=q35
export GVM_USER_NET=0
export GVM_GUEST_EXTRA_ARGS=${GVM_GUEST_EXTRA_ARGS:-clocksource=hpet}
exec "$root/scripts/gvm-device-node.sh" "$node" "$method" \
    -object memory-backend-ram,id=n0,size=2G \
    -object memory-backend-ram,id=n1,size=2G \
    -numa node,nodeid=0,cpus=0-1,memdev=n0 \
    -numa node,nodeid=1,cpus=2-3,memdev=n1 \
    -numa dist,src=0,dst=1,val=20 -numa dist,src=1,dst=0,val=20 \
    -object iothread,id=owner-io,poll-max-ns=0 \
    -drive "file=$disk,format=raw,if=none,id=data-owner-$node,cache=none,aio=threads" \
    -device "virtio-blk-pci,drive=data-owner-$node,id=data-owner-$node,bus=pcie.0,addr=$addr.0,serial=GVM-DATA-OWNER$node,ioeventfd=on,vectors=5,loom-aggregate=on,loom-numa-node=$node,iothread=owner-io" \
    "$@"
