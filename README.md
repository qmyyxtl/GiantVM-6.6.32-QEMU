# GiantVM 6.6.32 — RDMA QEMU

本仓库保存与 [RDMA KVM](https://github.com/qmyyxtl/GiantVM-6.6.32-KVM)
配套的 GiantVM QEMU，基于 **QEMU 9.0.90**。
两台物理机上的 QEMU 协同运行一个 guest OS，guest 看到跨端 CPU，
以及各有 owner 的独立 PCI 设备。两块盘仍是 disk1、disk2，不合并为逻辑卷。

## 当前版本

本次整理从原 `device-aggregation` 分支 `0be1943958` 起保留提交历史；
新增仓库入口说明和 2026-10-09 IRQ locality 验证记录，功能代码保持不变。
`main` 与 `device-aggregation` 在整理时指向同一版本。
配套 KVM 功能基线为 `878db8c75`，host 内核名 `6.6.32-krdma+`。

## 功能与边界

- `-local-cpu` 描述本节点 CPU 范围和 IPoIB 节点列表。
- `loom-aggregate=on,loom-numa-node=N` 为设备标记 owner；
  `LOOM_DEVICE_AGGREGATION=1` 启用现有设备聚合实现。
  这些是代码保留的参数名，未在仓库迁移时重命名。
- 跨端 PCI 配置和设备访问由现有设备转发逻辑处理；
  QEMU 对 guest RAM 的相关访问经过 DSM，DMA pin 遵守内核所有权协议。
- 已验证跨端独立 raw 数据盘读写、flush、跨端回读；
  VFIO 网卡有独立启动/测试工具，当前带宽回归未挂网卡，
  不能将本轮结果解读为真实网卡聚合吞吐测试。
- **双盘并行目前没有带宽增益**；共享状态/页面争用仍待定位。

## 构建

构建依赖包括编译器、Python 3、Ninja、pkg-config、GLib/Pixman 开发包；
其余依赖依 configure 检查与启用功能补齐。

```bash
git clone git@github.com:qmyyxtl/GiantVM-6.6.32-QEMU.git
cd GiantVM-6.6.32-QEMU
mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm --enable-vnc --disable-werror
ninja -j8 qemu-system-x86_64
./qemu-system-x86_64 --version
```

本次整理只验证源码和文档状态，没有重新构建或重启现有实例。

## 两端启动

先准备匹配 host 内核、正常 RDMA/IPoIB 链路、既有 root 镜像，
以及 master 使用的 guest bzImage。当前验证 guest 为
`6.12.0-vanilla-vfio #20`，4 vCPU / 4 GiB，`clocksource=hpet`。
下面使用已部署的专用 1 GiB raw；新机器需先另行准备数据盘。

在两端各自的本仓库目录执行；需要 host 上的 KVM/DSM 权限。
以下保留当前部署使用的镜像路径，按实际环境替换。

Garen/master：

```bash
sudo env GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  GVM_GUEST_KERNEL=/home/xtl/start/bzImage-6.12-vfio-bss2m \
  GVM_GUEST_EXTRA_ARGS=clocksource=hpet \
  ./scripts/gvm-device-node.sh 0 \
  -drive file=/home/xtl/start/gvm-device-data-node0.raw,format=raw,if=none,id=data-owner-0,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-0,id=data-owner-0,addr=09.0,serial=GVM-DATA-OWNER0,ioeventfd=off,vectors=0,loom-aggregate=on,loom-numa-node=0
```

Ezreal/slave（不指定 guest bzImage）：

```bash
sudo env GVM_USER_NET=0 GVM_IPLIST='172.0.1.4 172.0.1.2' \
  ./scripts/gvm-device-node.sh 1 \
  -drive file=/home/xtl/start/gvm-device-data-node1.raw,format=raw,if=none,id=data-owner-1,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-1,id=data-owner-1,addr=0a.0,serial=GVM-DATA-OWNER1,ioeventfd=off,vectors=0,loom-aggregate=on,loom-numa-node=1
```

QMP 在每端 `127.0.0.1:4444`，master 串口在 `127.0.0.1:5555`。
使用现有串口客户端登录 guest root 后再运行串口执行工具。

## guest 中断配置与带宽测试

不修改 guest 内核代码；在 guest root shell 配置运行时 IRQ affinity。
先根据 `/proc/interrupts` 和 PCI/virtio 对应关系确认 IRQ；
下列 10/11 仅适用于当前 `vectors=0` 的测试拓扑，其他设备布局会改变编号。

```bash
echo 0 > /proc/irq/10/smp_affinity_list  # Garen 的 vda -> guest CPU0
echo 2 > /proc/irq/11/smp_affinity_list  # Ezreal 的 vdb -> guest CPU2
cat /proc/irq/10/effective_affinity_list
cat /proc/irq/11/effective_affinity_list
cat /proc/interrupts
```

中断迁移可能到下一次中断才生效；应在 I/O 后确认 effective affinity 和计数。
核对 irqbalance 是否会改写；当前 guest 中该服务 inactive。
本配置重启后失效；若持久化，应按设备身份发现 IRQ 再设置，不能普遍硬编码 10/11。

在 master host 运行既有测试工具（专用测试数据盘会被覆盖）：

```bash
./scripts/gvm-device-serial-exec.py \
  ./scripts/gvm-device-guest-disk-bandwidth.py \
  --guest-args='--seconds 15 --rounds 3' \
  --timeout 240 --preserve-on-timeout
```

默认先完整预写两盘，再轮换测试单盘0、单盘1、双盘并行。
仅对已经完整预写的既有测试盘，使用 `--skip-prefill` 跳过准备。
worker 在 CPU0/CPU2 固定后才创建并首触缓冲区；每端写自己的 owner 盘。
工具通过串口内存传入代码，不创建 guest 脚本、日志或结果文件。

2026-10-09，修改 IRQ 后三轮 15 秒、1 MiB、QD=8 中位数：

| 工作负载 | MiB/s | 两端 DSM 慢路径 fault / 写入 MiB 范围 |
| --- | ---: | ---: |
| Garen 单盘 | 968.5 | 6.9–11.2 |
| Ezreal 单盘 | 940.3 | 8.6–9.9 |
| 双盘并行合计 | 555.8 | 149–174 |

并行带宽只有最快单盘的 0.574 倍、两端单盘中位数之和的 0.291 倍。
计数包含后台活动且非原子；fault 的热点页面尚未逐页定位。
1 GiB 数据集循环覆盖，不能据此宣称介质稳态性能。
完整因果对照与已知启动 WARN、历史 deadlock 见
[实验与设计记录](docs/gvm-device-aggregation.md)。

## 工具入口

| 工具 | 用途 |
| --- | --- |
| `scripts/gvm-device-node.sh` | 启动单个分布式节点 |
| `scripts/gvm-rdma-link.py` | RDMA/IPoIB 链路相关工具 |
| `scripts/gvm-device-vfio-node.py` | VFIO 设备测试入口 |
| `scripts/gvm-device-hardware-node.py` | 真实网络/存储后端；模型设备，不是 VFIO |
| `scripts/gvm-device-guest-disk-test.py` | 独立磁盘正确性验证 |
| `scripts/gvm-device-guest-disk-bandwidth.py` | owner-aware 单盘/双盘写带宽 |
| `scripts/gvm-device-serial-exec.py` | 通过已登录串口在内存中执行 guest 工具 |

保留上游 `README.rst`、版权、许可证和历史署名。许可见 `COPYING` 等原文件。
guest 镜像、部署 raw、编译产物不纳入本次整理。
