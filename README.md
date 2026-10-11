# GiantVM 6.6.32 — RDMA QEMU

本仓库保存与 [RDMA KVM](https://github.com/xue-123-song/GVM-kernel/tree/device-aggregation)
配套的 GiantVM QEMU，基于 **QEMU 9.0.90**。
两台物理机上的 QEMU 协同运行一个 guest OS，guest 看到跨端 CPU，
以及各有 owner 的独立 PCI 设备。两块盘仍是 disk1、disk2，不合并为逻辑卷。

## 当前版本

当前开发在原 origin `git@github.com:xue-123-song/GVM-qemu.git` 的
`device-aggregation` 分支。`0deb973ad9` 实现控制面目录推送，
配套 `86812dc787` 修复远端 CPU 占位线程暂停状态，验证记录见末尾。
配套 KVM 源码基线为 `c05e13ce3`，两端 host 内核名仍为 `6.6.32-krdma+`。

供同事使用的 GiantVM 新仓库保留 2026-10-09 整理时的快照；
该快照保留此前历史，`main` 与 `device-aggregation` 当时指向同一版本，
本轮没有更新该副本。

## 功能与边界

- `-local-cpu` 描述本节点 CPU 范围和 IPoIB 节点列表。
- `loom-aggregate=on,loom-numa-node=N` 为设备标记 owner；
  `LOOM_DEVICE_AGGREGATION=1` 启用现有设备聚合实现。
  这些是代码保留的参数名，未在仓库迁移时重命名。
- 跨端 PCI 配置和设备访问由现有设备转发逻辑处理；
  QEMU 对 guest RAM 的相关访问经过 DSM，DMA pin 遵守内核所有权协议。
- 可选 owner IOMMU 模式通过标准 ACPI DMAR 按设备归属提供控制单元；
  已接入 VFIO 标准映射生命周期，并验证两端独立 X710 VF 的真实收发；
  网卡吞吐和非 DSM 开销优化仍未完成。
- 已验证跨端独立 raw 数据盘读写、flush、跨端回读；
  VFIO 网卡有独立启动/测试工具，当前带宽回归未挂网卡，
  不能将本轮结果解读为真实网卡聚合吞吐测试。
- 最新双盘读测试已观察到带宽增益：2935 MiB/s，约为两端单盘吞吐之和的 83%；
  尚未达到线性扩展，剩余开销仍待进一步区分。

## 构建

构建依赖包括编译器、Python 3、Ninja、pkg-config、GLib/Pixman 开发包；
其余依赖依 configure 检查与启用功能补齐。

```bash
git clone -b device-aggregation git@github.com:xue-123-song/GVM-qemu.git
cd GVM-qemu
mkdir -p build
cd build
../configure --target-list=x86_64-softmmu --enable-kvm --enable-vnc --disable-werror
ninja -j8 qemu-system-x86_64
./qemu-system-x86_64 --version
```

历史整理记录与后续原 origin 上的开发记录分别保留；当前构建和验证见末尾。

## 两端启动

先准备匹配 host 内核、正常 RDMA/IPoIB 链路、既有 root 镜像，
以及 master 使用的 guest bzImage。当前验证 guest 为
`6.12.0-vanilla-vfio #20`，4 vCPU / 4 GiB，`clocksource=hpet`。
下面是使用既有专用 1 GiB raw 的基础 INTx 启动示例；
最新 MSI-X/IOThread 参数见末尾。新机器需先另行准备数据盘。

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

## 2026-10-10：不修改 guest 的 owner 本地 I/O 路径

后续开发继续在原 origin `git@github.com:xue-123-song/GVM-qemu.git` 的
`device-aggregation` 分支进行；上面的 GiantVM 新仓库是供同事使用的快照，
本轮未向该副本推送。

`500b5fa0e8` 将 BAR 路由改为先检查 owner 本地目录。本地命中仍校验设备
generation，但不再刷新无关远端目录。两端 CPU 对两端 EDU 寄存器读写及
PCI memory-enable 关闭/恢复通过；探针观察到约 1.24 万次本地磁盘 doorbell，
对应的远端目录 RPC 为零。远端访问与未知地址仍保留原有刷新行为，
因此 HPET/串口等非 PCI 访问的目录轮询尚未消除，不能宣称控制面失效机制已完成。

聚合模式禁止本地 irqfd，以免完成中断绕过 QEMU 的跨节点 MSI 路由。
virtio PCI 应在选择 guest notifier 时直接使用用户态 notifier，而不是先尝试
被禁止的 irqfd 再导致 virtio-blk 数据路径回退。MSI-X、owner 本地 ioeventfd
和 IOThread 可以组合使用；普通非聚合模式的 irqfd 选择保持原样。

在两端各自增加一个 IOThread，并将本地数据盘连接到该线程：
```
-object iothread,id=owner-io,poll-max-ns=0
-device virtio-blk-pci,...,ioeventfd=on,vectors=5,iothread=owner-io,loom-aggregate=on,loom-numa-node=N
```
这里的省略号需替换为前面例子的 drive/id/addr/serial；不改变 raw 后端
`cache=none,aio=threads`，不启用 IOThread 忙轮询。QMP `query-iothreads`
与线程级探针均确认两端队列在各自 IOThread 上处理，数据校验读测试通过。
当前四队列 MSI-X 中断为 managed IRQ，直接写 affinity 可能返回 EPERM；
应核对队列与 CPU 的实际对应关系，不能照搬上面的 INTx 配置。

4 vCPU / 4 GiB / 双 NUMA，1 MiB O_DIRECT Linux AIO 顺序读；
单盘两 worker、每 worker QD4；双盘四 worker、总 QD16。
worker 在对应 owner CPU 上固定后首触缓冲区；使用既有 1 GiB raw 数据，
计时读取降低频率，每阶段 8 秒、三轮，无性能探针：

| 配置 | Garen 单盘 | Ezreal 单盘 | 双盘合计 |
| --- | ---: | ---: | ---: |
| 本地 BAR 优化，ioeventfd=off | 1895.9 | 1617.0 | 1662.5 |
| 本地 BAR 优化，ioeventfd=on + IOThread | 1562.0 | 1712.8 | 2303.0 |

单位 MiB/s，数值为三轮中位数。双盘提高约 39%，但 Garen 单盘下降约 18%；
双盘只达到两端单盘吞吐之和的约 70%，尚未达到线性扩展。
两组之间重启了 guest，DSM 页状态会变化，不能把全部差值归因于单项代码。
小容量循环读取和硬件 RAID 缓存也限制了结果的外推，不代表持续介质带宽。
本轮没有挂网卡，没有测量 VFIO 网卡聚合吞吐。

跨节点完成路径也已验证：在 CPU2/3 上读取 Garen 盘、在 CPU0/1 上读取
Ezreal 盘，数据校验通过。Garen 盘 req.0/1 分别在远端 CPU2/3 增加
902/857 次中断；Ezreal 盘 req.2/3 分别在远端 CPU0/1 增加 754/768 次中断。
该短测用于验证路由正确性，缓冲区处于远端，不能作为本地吞吐对照。

## 2026-10-10：将目录更新移到控制面

运行时不再为每次 BAR/非 PCI 访问拉取远端设备目录。owner 在 PCI 配置写入、
BAR 重配置和 reset 中发布 448 字节前缀，并在控制操作返回前等待所有 peer 确认。
启动时仍用目录 RPC 拉取完整描述符；协议版本升为 3，所有节点必须一起更新。
扩展配置读取仍使用 owner RPC，与原来的转发契约一致。

- 目录更新和确认直接由 RDMA 接收线程处理，只获取目录缓存锁，不获取 BQL。
  普通请求由每个 peer 的 FIFO 工作线程处理并注册 RCU；队列最多 1024 项。
  控制通知绕开普通 response-CQ 锁，避免配置 RPC 中的反向确认互等。
- reset 开始发布 odd sequence，结束发布新的 generation/even sequence；
  快照等待稳定前缀。确认或 reset 发布超过 5 秒未完成时停止聚合，
  不重试结果未知的 I/O；这不是节点故障后的持续可用机制。
- 收到更新后增加本地 epoch，代理映射只在 epoch 改变时刷新。
  普通跨端 MMIO 转发也先走聚合路由；代理回调拒绝失效的旧 BAR 地址，
  INTx 使用已发布的 generation，避免接收新 generation 后仍检查旧描述符。
- `86812dc787` 单独修正远端 vCPU 占位线程的 pause/resume 状态。
  这类线程不执行 guest；本节点无需等待它消费 vCPU halt_cond。
  普通 CPU 的处理保持原样。

验证使用同一个 `6.12.0-vanilla-vfio #20` guest，没有修改 host/guest 内核、
guest 驱动或 DSM 协议。已通过两端 CPU 对两端未绑定驱动的 EDU 做 BAR
重定位、新地址寄存器读写、旧地址失效及恢复；两端并发各五次重配置通过。
两端 stop/cont、整机 reset 后重新登录通过；普通四 vCPU TCG 配置的
stop/cont/reset/退出也通过，后者仅验证通用控制逻辑，不是普通 KVM 性能对照。

包含重配置和双盘读的 15 秒探针窗口中，两端各发布 51 次控制更新，
处理 3029/2959 次磁盘队列通知；HPET 访问合计 135089 次，另有串口访问。
两端目录 RPC 均为 0。PCI 设备的实际远端访问和 slave 的 HPET owner 转发
仍按原契约执行，不能将目录 RPC 归零解释为所有跨端 I/O RPC 归零。

沿用前节 1 MiB 顺序读、单盘两 worker QD8/双盘四 worker QD16，
首触缓冲区在设备 owner 的 CPU 上，每阶段 8 秒，三轮，无性能探针。
代码通过串口在内存中执行，不创建脚本、日志或结果文件；本轮不做预写，
读结果校验既有两盘的数据模式。这里是两 worker 的内存测试程序，
不应将上面默认单 worker 的旧脚本直接视为相同测试。

| 配置 | Garen 单盘 | Ezreal 单盘 | 双盘合计 |
| --- | ---: | ---: | ---: |
| 本地 BAR 快路径 + IOThread，仍轮询目录 | 1562.0 | 1712.8 | 2303.0 |
| 控制面目录推送 + IOThread | 1823.8 | 1732.4 | 2935.0 |

单位 MiB/s，三轮中位数。双盘增加约 27%，达到两端单盘吞吐之和的约 83%；
Garen 单盘增加约 17%，Ezreal 约 1%。不同阶段间重启过 guest，DSM 页状态
以及小容量循环读/RAID 缓存会影响结果，不能把全部差值归因于目录优化。

使用远端 CPU 读 owner 盘的功能检查也通过：Garen 盘 req.0/1 在 CPU2/3
增加 785/695 次中断；Ezreal 盘 req.2/3 在 CPU0/1 增加 752/826 次中断，
数据校验成功。该检查的缓冲区在远端，不能拿其吞吐替代本地性测试。

host 线程固定到 RDMA 卡所在 NUMA node 1 的不同物理核，只使双盘中位数
约提高 4%，同时使 Ezreal 单盘下降约 15%；已恢复原亲和性，没有保留为推荐配置。
完成路径分段探针显示映射/解除映射及 ring 更新包含大量 DSM pin/unpin；
高频探针显著影响吞吐，不能据此把剩余性能损失全部归为 DSM。

当前仍需处理或测量：非 PCI 实际设备转发、本地目录扫描/队列成本、
无需 guest DMA bridge 的真实网卡 DMA 路径及双网卡并行吞吐。
本轮仍未挂网卡，尚未完成“除 DSM 外尽可能降低开销”的整体目标。

### BAR 路由描述符复制优化（2026-10-10）

BAR 地址查找和远端 BAR 回调只读取 generation、BAR 地址/大小等前 192 字节，
不再每次复制包含 PCI 配置的 4288 字节完整描述符。控制路径仍使用完整
snapshot；接收缓存锁、reset 奇数 sequence 等待、generation 校验和失效机制保留。
未改 guest 内核或驱动，HPET 配置保持不变。

两端编译和 checkpatch 通过；CPU0/2 对两端 EDU 的 BAR 移动、memory enable
关闭/恢复、旧地址失效、并发重配置通过。原磁盘读测试三轮中位数：
Garen 单盘双 worker 1783.9、Ezreal 单盘双 worker 1722.9、
双盘四 worker 2933.9 MiB/s。双盘与优化前 2935.0 基本持平，
没有证据证明吞吐提高；不能把复制字节数下降当成实测性能收益。
运行实例仍为四 vCPU、两节点各两 CPU、4 GiB、owner IOThread、MSI-X。

正常关闭上一版实例时，master 返回 0，slave 返回 SIGSEGV；
两端已重新启动并通过上述检查。退出异常尚未定位，不能宣称正常退出已验证。
当前 VFIO staging 路径仍依赖 guest DMA bridge；透明真实网卡支持和
双网卡吞吐目标尚未完成。

### 分布式 QEMU 退出顺序修正（2026-10-10）

旧版在收到 shutdown 请求时就唤醒远端 CPU 占位线程，使其退出并执行
kvm_arch_destroy_vcpu/munmap。此时 VM 停止流程尚未完成，状态通知可能仍遍历
这些 vCPU 注册的回调，合并 MMIO ring 也可能仍引用已释放的 kvm_run 映射。
现场确认旧版 slave 在 kvm_flush_coalesced_mmio_buffer 的 ring 读取处 SIGSEGV；
调整映射生命周期时还暴露 master 的 vm_state_notify 崩溃。

最终只修改退出顺序：请求阶段仅通知主循环；qemu_cleanup 先执行 vm_shutdown，
完成停止通知和 block drain，再唤醒远端 CPU 退出。不保留额外的 ring alias
迁移改动，也不改 guest 或 KVM 内核。仅含该顺序修正的版本在两端重新启动、
root 登录、双盘四 worker 读取校验、guest sync/poweroff 后，两端 QEMU 均返回 0。
普通双 CPU/64 MiB TCG 实例的 QMP quit 也返回 0；这不是普通 KVM 性能基线。
短时读取属于功能回归，不作为新的吞吐优化结论。

透明 VFIO 下一步需要标准 vIOMMU 的设备级映射通知：分别保留 IOVA 与 GPA，
将远端设备映射/失效送至 owner，并在解除映射前发布已完成的 DMA 写入。
当前 master 的 vIOMMU 控制状态不会自动配置 slave；仅添加 intel-iommu 参数
不能解决问题。现有 staging 仍依赖 DMA bridge，尚未实现上述透明路径。

### Q35 / 标准 vIOMMU 基础验证（2026-10-10）

现有 6.12.0-vanilla-vfio guest 内核已内置 SATA_AHCI、INTEL_IOMMU、I40E 和 IAVF，
本次不改 guest 内核/驱动。两端 machine 改为 q35,kernel-irqchip=split，
添加 intel-iommu,caching-mode=on,intremap=off；master guest 命令行增加
intel_iommu=on iommu.strict=1，保留 clocksource=hpet、四 CPU 和两节点 NUMA。

Q35 巨虚已启动并 root 登录。guest 显示 DMAR enabled、Translated domain、
strict invalidation、Queued invalidation 和独立 IOMMU groups；两个 owner 盘
仍可见，双盘四 worker 的短时只读数据校验通过。该检查不作为新的吞吐基线。

两端运行状态直接检查：master dmar_enabled=true、qi_enabled=true、
root=0x4b78000；slave dmar_enabled=false、qi_enabled=false、root=0。
这证明单纯添加 vIOMMU 参数不会同步远端设备 DMA 映射。

启动器 scripts/gvm-device-node.sh 新增 GVM_MACHINE=pc|q35（默认 pc）和
GVM_IOMMU=0|1（默认 0，开启时要求 q35），便于继续验证；本轮运行参数仍使用
既有 owner IOThread/双 NUMA 配置。仅主节点开启 guest DMA remapping，
不是已完成的跨节点 vIOMMU/VFIO 方案，暂不应直接据此挂远端真实网卡。

下一步应将 master 上远端 PCI proxy 的标准 IOMMU 映射/失效通知发送到 owner，
保留 IOVA、GPA、权限和映射生命周期；失效确认前必须完成 owner 的 DMA 回写。
不能让 slave 独立消费同一失效队列并提前发布完成，也不能假设现有 staging
中 IOVA=GPA 的约定适用于翻译后的 DMA 地址。HPET 不作调整。

### 非恒等 DMA 地址与 AHCI 映射标记（2026-10-10）

staging buffer 现在分别保存 IOVA 和 GPA；pool_copy 使用 IOVA，
guest RAM 读写使用 GPA。新增 loom_vfio_dma_register(bc, iova, gpa, size, direction)，
为标准 IOMMU 翻译通知提供入口。旧 bridge REGISTER 仍以 GPA=IOVA 调用，
不会因此自动接入标准 IOMMU。重复注册到不同 GPA、冲突的重叠翻译以及地址溢出被拒绝。

在未修改的 guest 中，通过 Python stdin 分配并锁定一页内存，在 QEMU 中用
临时空 container 调用实际 staging 函数，IOVA=0x4000000000、GPA=0x089a8000。
验证初始拷贝、device-to-CPU 修改 0xbc、CPU-to-pool 修改 0xcd、未修改字节保持 0xa5，
以及注销前将 0xde 写回 guest；guest 直接检查最终内容通过。
旧 identity REGISTER 的重复引用、两次 UNREGISTER、最终 buffer count=0 也通过。
这是 staging 同步函数验证，未通过 VFIO ioctl 映射真实网卡，也未验证跨节点 IOMMU 通知。

Q35 重启过程中另发现根盘 AHCI 超时并最终 VFS panic。源码/运行现场显示：
CLB 的 map_page 输出错误地写到 res_fis_is_dsm，lst_is_dsm 始终为 false，
命令头路径因此跳过已有的 DSM pin/unpin。修正为写入 lst_is_dsm 后，新实例
正常启动、root 登录，并通过双盘四 worker 短时只读校验。不能凭一次启动
断言 Q35/AHCI 的所有问题均已排除。

两端已编译，guest 内核/驱动和 host KVM 未修改，HPET 保留。
下一步评估按 owner 报告独立 DMAR 控制窗口和设备 scope，避免本地 DMA 映射
必须经过 master；该 owner IOMMU 路径及真实双网卡吞吐尚未实现。

### Owner 本地 IOMMU 控制路径（2026-10-10）

LOOM_OWNER_IOMMU=1 为每个 owner 提供独立的标准 VT-d 寄存器窗口，
地址为 0xfed90000 + owner * 4096。master 的 ACPI DMAR 表按设备 BDF 归属
生成多个 DRHD；非聚合设备和 IOAPIC 属于节点 0。每个窗口只由对应 owner
的 Intel IOMMU 模型处理，拥有独立 root table 和 queued-invalidation 状态。
本地访问直接 dispatch；远端访问使用现有释放 BQL 后等待的 owner RPC。
不是将同一队列复制给多端消费。启动目录同时校验各节点的 owner-IOMMU 模式。

在 Garen/Ezreal 两节点实测，未修改的 guest Linux 识别 dmar0/dmar1：
05.0、09.0、根盘 AHCI 属于 dmar0；06.0、0a.0 属于 dmar1。
两端 dmar_enabled/qi_enabled 均为 true，root 分别为 0x4bee000/0x4bec000。
CPU0/2 分别读取两个单元各 64 次，版本均为 0x10。
探针显示 Garen 本地 CPU0 dispatch 64 次、Ezreal 本地 CPU2 dispatch 64 次；
仅跨 owner 读取各产生 64 次 RPC。完成一次完整 system_reset 并重新登录后，
两个单元的 GSTS 均为 0xc4000000，TE/QIES 保持启用，四组读取检查再次通过。
双盘四 worker 短时只读校验通过；该轮不作为新的吞吐基线。

启动器增加 GVM_OWNER_IOMMU=1（默认 0），要求 GVM_MACHINE=q35、
GVM_IOMMU=1；两端使用相同设置，并保留现有 NUMA、owner IOThread 和
clocksource=hpet 配置。该模式目前要求 intremap=off、pasid=off、
device-iotlb=off，未验证三/四节点运行。

本轮已实现 owner 本地的 IOMMU 控制基础，但 VFIO 标准 MAP/UNMAP 通知还没有
自动接入 staging buffer 注册/回写生命周期；真实双网卡验证和性能目标仍未完成。

### VFIO staging 范围失效生命周期（2026-10-10）

loom_vfio_dma_unmap 现在处理覆盖多个映射和部分映射的失效范围，
保留左右存活的 pool 与 buffer，分别保留原 GPA、权限和引用信息。
backend 成功解除硬件 DMA 后，先将失效区间的设备修改发布到 guest，
再释放该区间的 staging；存活区间不会在这一步被整块回写或释放。
发布失败时保留 staging 并向调用者返回错误。container 销毁直接清理资源，
不在销毁阶段重新访问 guest RAM。

两端编译；以实际 loom-dma.c 源码和模拟 RAM 后端运行内存内测试，
覆盖中间拆分、首尾删除、带空洞的多映射失效、写回失败后重试、
地址空间末尾失效、未对齐拆分拒绝、剩余区间双向同步及最终清理。
该验证没有创建磁盘上的实验文件，不是物理 VFIO/DMA 收发验证。
自动接入标准 MAP 通知、真实网卡以及吞吐测量仍需继续推进。
guest 内核、驱动和 HPET 未修改。

### 标准 VFIO MAP 通知接入（2026-10-10）

vfio_iommu_map_notify 使用标准通知中的 IOVA 和 translated_addr/GPA，
调用新的 vfio_container_dma_map_iommu。staging 模式先初始化 pool 和同步
buffer，再交给 VFIO backend，避免硬件观察到尚未填充的 pool。
设备只读映射使用 to-device 同步，允许设备写入的映射使用双向同步。
大叶映射按最多 64 MiB 分段注册；初始化或 backend MAP 失败时回滚。
普通非 staging 模式仍直接使用原 RAM 地址。

gvm-device-vfio-node.py 默认使用 q35、owner IOMMU 和双 NUMA，
不挂 DMA bridge，guest 使用已有标准 IAVF/I40E 驱动。
--legacy-dma-bridge 保留旧测试入口；数据盘改为现有 MSI-X/owner IOThread 参数。
HPET 保留。该入口仍只接受独立 IOMMU group 的 SR-IOV VF，不解绑 host PF。

实际源码配合模拟 RAM/backend 的内存测试通过：初始化先于 backend MAP、
自动映射/失效生命周期、只读权限、大叶分段、初始化失败和 backend 失败回滚、
普通非 staging 路径。此记录尚不证明真实网卡成功初始化或真实 DMA 收发，
需继续进行硬件验证，不能据此宣称吞吐已改善。

### 双 X710 VF 的标准驱动硬件验证（2026-10-10）

两端 0000:0c:00.0 / ens101f0np0 的 host PF 仍使用 i40e、链路 UP，
各创建一个 VF 0000:0c:02.0，独立 IOMMU group，由启动器绑定 vfio-pci。
Garen VF MAC=02:47:56:4d:00:01，Ezreal VF MAC=02:47:56:4d:01:01。
host 的管理端口和 ibs4f0 DSM 链路未解绑。当前 VF 保留供后续测试。

两端用 scripts/gvm-device-vfio-node.py 启动，分别传入 node 0/1、
--host 0000:0c:02.0、--data-disk /home/xtl/start/gvm-device-data-node0.raw
或 node1.raw。env 使用 GVM_IPLIST='172.0.1.4 172.0.1.2'、
GVM_GUEST_EXTRA_ARGS=clocksource=hpet。前提是已创建 VF 并配置上述 MAC，
且两端旧实验 QEMU 已退出。启动器退出时恢复 VF 原 driver；
当前实验仍在运行。

guest 未修改的 6.12.0-vanilla-vfio #20 正常启动并登录，
原生 iavf 驱动识别 00:07.0/enp0s7/dmar0 与 00:08.0/enp0s8/dmar1，
两张独立网卡均报告 10 Gbps 链路 UP。guest clocksource 确认 hpet。

收发使用两个临时子网，避免 guest 内部本地路由绕过硬件：
- enp0s7=198.18.10.2/24，目标 Ezreal host PF 上的 198.18.10.1/24。
- enp0s8=198.18.11.2/24，目标 Garen host PF 上的 198.18.11.1/24。

两组初始 5 次 ping 均无丢包；对端 host PF 的 tcpdump 直接看到上述 VF
MAC 发出的 ICMP request 及 reply，未写 pcap 文件。驱动计数随后刷新，
两张卡 RX/TX 都增长、错误数为 0，不能以首次立即读取的 0 判定无收发。

IRQ affinity 按设备 owner 设置为 07.0 -> CPU0-1、08.0 -> CPU2-3，
活跃队列后续 effective affinity 转到 owner CPU；尚未再次触发的 IRQ
可能仍显示旧 effective affinity，需要下一次实际中断确认。
配置后各 10 次 ping 无丢包；期间使用过 GDB 暂停检查，因此这不是性能对照。
初始 RTT 已有约 20-60 ms，不能宣称网卡开销低或达到线性吞吐。

关闭两张接口后，两端实际 staging map/buffer 均降到 66 个 / 270336 字节，
重新开启接口后再次各 5 次 ping 无丢包，检查未发现 DMA fault 或 TX timeout。
Garen 重新开启后的首批回复出现最高约 1 秒延迟，此处只作为生命周期功能检查。
接口开启时曾观察 Garen 1178 个 / 4825088 字节、Ezreal 1114 个 / 4562944 字节。
现有同步遍历 buffer，pool_copy 再遍历 map 链表，重复查找会随映射数放大，
下一步应去除这部分非 DSM 成本，并测量同步、复制与队列处理时间。

两端测试 IP alias 当前保留；结束此硬件测试时分别删除：
Ezreal: sudo ip addr del 198.18.10.1/24 dev ens101f0np0
Garen: sudo ip addr del 198.18.11.1/24 dev ens101f0np0
停止本实验 QEMU、VF 恢复 iavf 后，确认 VF 未被其他进程使用，再将各 PF
sriov_numvfs 恢复 0。不得在 VF 仍直通时直接删除。Garen 的其他 libvirt VM 未操作。

### DMA pool 地址缓存（2026-10-10）

标准 IOMMU 的单映射 buffer 在注册时缓存 pool HVA，sync_buffer 直接复制，
不再为每个 buffer 或改变的字节段扫描 map 链表。部分失效的临时视图和存活
片段同时调整缓存地址；缓存不指向 LoomDMAMap 对象，map 结构拆分不使存活
pool 地址失效。跨多个映射的旧 bridge buffer 保留原查找路径。

真实源码配合模拟 RAM/backend 的生命周期测试通过，包括中间拆分后的
双向同步、失效范围最终写回、跨 map buffer fallback、映射失败回滚。
固定 Ezreal host CPU0，1178 个 4 KiB buffer，连续 200 次无变化 from-device
同步、三轮：修改前每次约 5210-5225 us，修改后约 551-557 us。
此测试隔离 DSM 和硬件，不是网卡吞吐结果；实际网络效果仍需实测。
guest 和 HPET 均未修改。

地址缓存的硬件回归：两端编译后正常 poweroff 旧实例，两端 QEMU 返回 0、
VF 恢复 iavf；新实例正常启动并登录，两张卡仍由未修改的 guest iavf 驱动工作。
每张卡保持 owner CPU/IRQ 范围，连续三轮各 20 次 ping（100 ms 间隔），
无 GDB/探针介入计时，均无丢包。三轮平均 RTT 的中位数：
Garen VF 25.061 -> 8.536 ms，Ezreal VF 22.324 -> 7.523 ms。
这是小包延迟观测，不是吞吐或线性扩展结论；前后重启可能改变 DSM 页面状态，
不能用这个观测单独计算全部延迟的归因比例。

新版本两张接口分别关闭/重开后各 5 次 ping 无丢包，未发现 DMA fault/TX timeout。
计时结束后只读检查：两端各 1114 个 buffer、4562944 字节，全部有缓存 pool 地址。
仍会扫描所有注册 buffer 并进行比较/复制、访问 guest RAM；剩余非 DSM 成本
需要继续测量，当前不能宣称路径已经最优。HPET 保持 hpet。

### 未变化 DMA buffer 的复制与真实 TCP 诊断（2026-10-10）

from-device 同步直接比较缓存 pool 与 baseline；无变化时不复制到 scratch。
有变化时仍取 scratch 快照再按改变的字节合并。保留 DMA 读屏障、方向过滤，
跨映射旧 buffer 继续走原路径。设备在比较后产生的修改由后续 exchange 观察，
与原 memcpy 快照具有相同的并发修改边界。

1178 个 4 KiB buffer 的隔离测试，固定 Ezreal CPU0、每轮 200 次同步：
修改前每次约 551-557 us；修改后约 363-370 us。
实际源码测试确认无变化时 scratch 不写入，最后一个字节的 DMA 修改、
CPU 更新、失效最终写回、部分拆分和旧跨 map 路径均通过。
该数据隔离 DSM/硬件，不代表实际 TCP 已达到低开销或线性扩展。

为定位真实性能，额外使用 Python stdin TCP sink/worker，无脚本、日志、pcap
或结果文件。每 worker owner CPU first-touch 1 MiB 固定字节 buffer，
单卡两 worker、双卡四 worker；每 worker 至少发送 6 秒，包含尾部排空与
receiver 确认时间，吞吐为确认的总字节数 / 最慢 worker 的完成时间。
接收端逐字节验证固定模式和总长度。这是诊断工具，不是 iperf3 标准基线。

在此前仅地址缓存的版本、四队列/512 RX/TX ring 下，三轮中位数：
Garen 单卡 3.297 MiB/s、Ezreal 单卡 0.499 MiB/s、双卡 0.663 MiB/s。
各轮差异很大，双卡未达到线性扩展；不能依据 ping 成功认定网络吞吐正常。
guest Tcp RetransSegs=1、TCPTimeouts=1 且对应 SYN；
两条额外数据连接的 TCP_INFO total_retrans=0，未发现大量数据重传。
在一次带探针的诊断中，Garen from-device 同步 4420 次、平均 641 us，
to-device 同步 2742 次、平均 1317 us（10 秒窗口，时长含 DSM，不能全归因于复制）。
采样不用于正式吞吐值。整批 buffer 反复同步仍有显著成本。

两张 VF 均可通过标准 ethtool -L combined 2 重配为两个队列，
匹配每端两 CPU；重配后 ping 通过。单轮诊断 Garen/Ezreal/双卡分别为
6.113/0.516/0.527 MiB/s，连接差异仍大，因此不能称为解决瓶颈。
下一步需继续检查 ring/有效 DMA 范围，避免每次数据路径访问都同步所有 pool；
guest 驱动与 HPET 不修改，真实双卡吞吐目标仍未完成。

新版本硬件回归：两端旧 QEMU 正常退出 0，新实例启动/root 登录，两张原生 iavf
VF 收发通过，clocksource 仍为 hpet。随后用标准 ethtool 设置每卡 combined=2、
RX/TX ring=128；单轮同一 TCP 诊断分别为 Garen 34.619 MiB/s、
Ezreal 20.106 MiB/s、双卡四 worker 11.227 MiB/s。数据内容/长度均由 receiver
校验通过，guest NIC rx_discards/tx_discards/tx_errors 均为 0；
Tcp RetransSegs 总计 2，TCP 校验错误为 0。较小 ring 的结果仍不支持线性扩展。
这里同时变化了复制路径、重启后的 DSM 布局和 ring 配置，
不能将吞吐变化全部归因于那 11 行复制优化。

计时结束后只读检查，Garen/Ezreal 分别有 390/452 个 staging buffer，
1597440/1851392 字节，均有缓存 pool 地址；不能仅从 ring 长度推算实际映射总数。
双盘四 worker 的 6 秒 O_DIRECT 只读内容校验通过，短时约 3184 MiB/s，
不作为新的正式磁盘吞吐基线。TCP sink 已退出，不保留后台实验服务或文件。

后续重点是按有效 DMA 范围缩小同步，并检查原生驱动轮询完成描述符时的
可见性边界：当前 from-device 交换由 MMIO/IRQ 触发，未实现独立的 DMA 更新
发布机制。该边界可能影响 IRQ 屏蔽期间的 NAPI/完成轮询，尚未证明是低吞吐根因。
不能为了减少扫描直接跳过完成回写。现有 iavf 寄存器将 QTX/QRX tail 与
VFINT_DYN_CTL 等中断控制分开，而当前 VFIO 写回调统一执行提交同步；
需要按实际设备协议验证其优化空间，未知设备保持正确的通用路径。

### X710 VF 中断控制写入的提交同步（2026-10-10）

持续 TCP 诊断的 12 秒窗口中，Garen BAR0 0x3800/0x3804 分别出现
2411/1114 次完成的提交方向同步，累计 866983/408875 us；
TX tail 0x0 的提交同步累计 2162690 us。采样含探针和 DSM 等待，
不能将累计时间视为可独立消除的吞吐损失，亦不作为正式吞吐基线。

VFIO 在读取物理 PCI 配置、guest ID override 之前匹配 8086:154c，
排除 mediated device。仅 BAR0 的对齐 32 位 VFINT_DYN_CTLN1[0..63]
及 VFINT_DYN_CTL01 写入省掉 guest-to-device 扫描；这些寄存器控制
中断或请求已提交工作的 writeback，不提交新描述符。每次仍先发布
from-device 修改；队列 tail、AQ、其他 BAR/宽度/设备保持完整同步。
这项改变不实现后台 DMA 一致性，也不跳过完成同步。

实际 helpers.c 写回调的 memfd 隔离验证覆盖 0..65535 地址、1/2/4/8
字节访问、其他 BAR/设备、同步顺序及同步失败阻止 MMIO，均通过；
Ezreal QEMU 构建通过。尚需新进程上的硬件回归和相同条件性能比较。

捕获 CONFIG_VSI_QUEUES 得到两条 128 项 TX/RX ring 的实际 DMA 地址。
两次设备 pool 与 baseline 快照中未发现未发布 DD；这是有限快照，
不足以排除 IRQ 屏蔽期间的瞬态完成可见性问题。HPET 与 guest 驱动不变。

### IRQ 写入硬件回归与 DSM 定点诊断（2026-10-10）

代码 d6848154d5 已通过两端构建并在新的两端 QEMU 中运行。
双 VF 的标准 iavf 两队列/128 项 ring 重配、关闭重开、ping 均通过，
rx_discards/tx_discards/tx_errors 为 0，未发现 DMA fault/TX timeout。
实际 BAR 写入探针观察到 0x3800/0x3804 共 5311 次写入，仅调用完成方向同步；
提交方向调用为 0，tail 保留两方向调用。HPET 未修改。

无探针的三轮 TCP 诊断（每 worker 至少 6 秒，含排空/接收确认）中位数：
Garen 单卡双 worker 29.278、Ezreal 单卡双 worker 24.067、双卡四 worker
20.739 MiB/s。所有接收内容与长度正确，双卡仍未线性扩展。
不能用此前单轮与这次三轮结果认定修改带来了某一吞吐增益，重启也影响 DSM。
双盘四 worker 的 O_RDONLY/O_DIRECT 只读内容回归通过，短时约 3148 MiB/s，
不作为正式磁盘性能基线。没有保留实验文件或后台 TCP sink。

按用户最新要求，暂停反复吞吐对比，优先定位 DSM；若证据支持，可修改 OS，
HPET 保持原样。kvm_dsm_fetch 入口记录请求种类、GFN、调用栈和返回累计时间，
同时用 loom_vfio_dma_sync 的入口/返回区分完成/提交同步中的远端请求。
计数是实际 fetch 请求（含 INVALIDATE/READ/WRITE），不是全部 EPT fault，
不是单纯页面数据传输次数。BPF 不采集文件，计时含探针成本。

一次短时双卡业务的 25 秒采样，覆盖业务的两个 5 秒窗口中：
Garen 121199、Ezreal 120358 次 fetch，约 12120/12036 次每秒；
后续两个无业务窗口分别 11081/11183 次，约 1108/1118 次每秒。
窗口覆盖初始化/收发/排空，不能拿 10 秒计数除以 6 秒推算严格活动期速率。
25 秒中普通调用类累计 fetch 时长 Garen 2.711 秒、Ezreal 2.606 秒；
完成同步中的累计 fetch 时长仅 3992/2343 us，Ezreal 提交同步另 1570 us。
绝大多数栈是 EPT -> kvm_dsm_vcpu_acquire_page -> kvm_dsm_page_fault。
这些累计时长不是 CPU 利用率或完整 DSM 成本，未覆盖外层锁、EPT 重建、TLB 等。

两端最热 GFN 11270（GPA 0x2c06000）对应 guest Kernel data。
通过 /proc/kallsyms、/proc/iomem 及 kvm_page_fault 的准确 guest_rip/页内偏移
交叉定位：页偏移 0 的锁更新、偏移 4 的缓存值更新来自 read_hpet，
同页还放着 mmlist_lock/tasklist_lock/softirq_vec/pidmap_lock/bit_wait_table。
这是共享 HPET 读锁的真实共享，加上邻居对象的页面假共享，不能只靠对齐
消除共享锁自身的竞争。其他热页是 jiffies 与锁、tk_core/timekeeper_lock、
调度 nohz 状态；故 DMA pool 扫描不是本次跨机请求数的主要来源。

后续优先区分业务新增热页、内核对象真共享/假共享以及 owner-local 分配与
softirq 执行；OS 修改以证据为依据。保留 HPET，不把更换时钟作为优化方案。

### IOMMU QI 控制结构的页面假共享（2026-10-10）

进一步通过准确 guest RIP 定位业务新增的 GPA 0x4afa000：
qi_submit_sync、其锁和 cache_tag_flush_range/np 在该页触发频繁缺页。
使用目标 guest 编译选项和头文件在 memfd 中取得结构偏移，随后只读
/proc/kcore 遍历 dmar_drhd_units，确认 dmar1(regbase fed91000) 的 q_inval
位于 GPA 0x4afa640，dmar0(fed90000) 位于 0x4afa680，两个独立失效队列
的锁/head/tail/free_cnt 共用一个 4 KiB 页面，非逻辑上的同一个锁。
15 秒含短时双卡收发的 EPT 采样，该共享页两端分别 12168/12309 次缺页。

已按用户允许修改 OS 的最新指示，在 Garen /home/xtl/GVM-guest-kernel
原 origin 分支 device-aggregation 提交 db67bfba7：
drivers/iommu/intel/dmar.c 的 q_inval 分配大小按 PAGE_SIZE 向上取整，
desc_status 同样保留完整页面并保持零初始化。仍使用 kmalloc/kzalloc 与
原 kfree，保留 GFP_ATOMIC、锁、QI 提交/完成及严格 IOMMU 失效语义。
当前 q_inval=40 字节、desc_status=1024 字节，各变为 4096 字节分配；
每 IOMMU 额外约 7 KiB 分配空间。用户原有 kernel/sched/fair.c 修改未纳入提交。
未调整 HPET。新旧 read_hpet 均为 236 字节、地址 ffffffff8106ea00，
机器码 SHA256 均为 0036f6dfd63d8177140e7a2ce5f3490a72c3181ac1077d6e5e54b4b339c4f363。

构建使用 make -C /home/xtl/GVM-guest-kernel LOCALVERSION= -j8 bzImage，
release 保持 6.12.0-vanilla-vfio，运行版本 #22；与原部署 .config 无差异。
直接使用已有构建产物，不覆盖原部署镜像，不创建实验脚本/日志/结果文件。
master helper 的启动覆盖须在 sudo 之后传入，普通 shell 环境会被 sudo 清掉：

```sh
sudo -n env GVM_GUEST_KERNEL=/home/xtl/GVM-guest-kernel/arch/x86/boot/bzImage \
  python3 /home/xtl/GVM-qemu/scripts/gvm-device-vfio-node.py 0 \
  --host 0000:0c:02.0 --data-disk /home/xtl/start/gvm-device-data-node0.raw
```

新 guest 中 dmar1 的 qi/status 为 GPA 0x495c000/0x495d000，dmar0 为
0x495e000/0x495f000：四者均 4 KiB 对齐、各有独立页面。双 VF 原生驱动
收发、两队列/128 项 ring 重配通过；一次连通失败的现场发现 Ezreal
测试 IP 198.18.10.1 消失，恢复地址后通过，未为此重启 VM/host。

25 秒包含一次至少 6 秒双卡四 worker 数据验证的采样，四个新元数据页
EPT 缺页合计 Garen 29、Ezreal 125；对应 fetch 请求 79/127（含 INVALIDATE）。
与前述 15 秒旧页采样时长/数据量不同，旧页还有邻居对象，不据此推算
吞吐或完整 DSM 降幅。页面布局与低热点计数支持具体假共享路径已缓解。
总体 DSM 仍高，两个活动窗口的普通 fetch 约 12022/12212 次每秒，
热点仍在 read_hpet 共享锁、jiffies/timekeeper、定时器等；不能声称瓶颈已解决。
带探针短时 TCP 约 14.22 MiB/s，内容和长度校验通过，不作性能提升结论。
只读 O_RDONLY/O_DIRECT 双盘四 worker 内容回归通过，短时 3052 MiB/s。
最终 guest HPET=hpet，NIC rx_discards/tx_discards/tx_errors 均 0，
未发现 DMA fault/QI submission/invalidation error/TX timeout。
所有 TCP sink 已退出，host 内核不变，Lux 和 Garen 原有 libvirt VM 未动。

### 定时器本地化与 VFIO 完成发布顺序（2026-10-10）

guest 原 origin 的 5d6dbedcf 修改 kernel/sched/core.c：sched_init 在
boot CPU 上初始化全部 runqueue，导致远端 CPU 的未启用 hrtick_timer
仍指向 CPU0 的 hrtimer clock_base。调用栈为 task_tick_fair ->
hrtimer_active，CPU2/3 周期性读取 CPU0 页的 seqcount。只读 /proc/kcore
确认初始 CPU1/2/3 的 hrtick base 均为 CPU0，上线时重新初始化 inactive
timer 后各自指向本地。保留 active timer、原同步语义和 HPET；用户的
fair.c 未提交修改保留。当前 guest 构建 #25，源码 HEAD 仍为 5d6dbedcf。

短时业务的 15 秒探针窗口中，每端约 98 万次 4 KiB 的提交同步 DSM_MEMCPY
读取、约 4 GB 扫描；相应提交同步 fetch 仅 19/3 次。这不是 RDMA 数据量：
很多成本来自本地页检查、锁、ioctl 和复制。尝试按只写 IOMMU 权限跳过
RX CPU 扫描，QEMU c174924b1b 已由 93bff2e687 回退，guest PTE 临时改动
也已恢复。实际同步代码在 memfd 中可复现反例：DMA 写 0x22，CPU 回收
缓冲区改为 0x33，下一轮 DMA 又写 0x22；跳过提交刷新后 baseline 仍为
0x22，完成差分检测漏掉该字节，guest 错误保留 0x33。保留双向刷新会正确
恢复 0x22。因此 DMA 方向不足以替代回收/完成生命周期信息，不能据此
直接省掉永久映射 RX 缓冲区的提交同步。

QEMU e66dad55f9 将设备写回改为两阶段，cdbfb2b777 保留 partial unmap
后片段的原注册位置。目标 iavf 的 coherent descriptor 注册早于 streaming
payload：先按旧到新顺序快照，再按新到旧顺序发布，只发布已有快照，
避免数据扫描之后又读取刚完成的描述符。旧代码的受控交错测试可出现
descriptor_done=1、payload=00 而设备 payload=44；新代码此时不暴露
完成位，下一次同步正确发布。24 种完成时机（含部分解除映射）通过，
原有 map/unmap、最后发布、失败重试、只读及跨映射回归通过。
这一顺序依赖描述符先于数据缓冲区注册；不是任意设备协议的通用证明。

两台机器已 Git 同步并构建、启动上述 QEMU。两张真实 VF 各接收 4 MiB，
长度/内容均通过；host 侧耗时约 0.80/0.99 秒、重传 63/46，仍有重传。
最终 ethtool 中两 VF 的硬件 rx_discards 恰为 63/46，tx_discards 和
tx_errors 均为 0；后续需定位 RX 队列资源及处理延迟，不能只看 DSM 数量。
回退方向优化后的此前同尺寸样本为 2.28/2.69 秒、重传 62/107；
重启、IRQ 亲和性和 DSM 页布局不同，不能归因全部差异于完成顺序修改。
新代码一次双卡四 worker 发送内容校验通过，短时仅 6.26 MiB/s，
不能声称网络性能已改善；双盘 O_RDONLY/O_DIRECT 四 worker 回归通过，
短时 3170 MiB/s，不作为新正式基线。无 DSM 的实际源码 microtest，
1178 个未改变的 4 KiB buffer，每次完成扫描旧版中位 364 us，
两阶段版本约 399 us；正确性修复仍增加了遍历成本，后续需继续优化。

设备重配中一次 ethtool -G 报 reset 等待超时，现场显示设置随后为
2 queue / 128 RX / 128 TX 且链路恢复；未为此重启 host。启动 printk
时间戳存在跨 CPU 偏差，但当前 monotonic 取样正常，HPET 配置/代码不变。
两台 host 的 systemd-networkd 重配置可能删除临时测试 IP；本次 Garen
198.18.11.1 消失已现场确认，恢复地址后连通。测试须前后核实测试 IP
和直连路由，不能把这些超时全部归因于设备/DSM。所有 TCP sink/接收进程
已退出，未保存实验文件；Lux、host 内核、原 libvirt VM 均未改动。

### 合并 DMA 提交读取与 DSM 现场（2026-10-10）

QEMU 3317c14343 修正 physmem 中 DSM RAM 复制失败的返回值：负 errno
或正的未复制字节数均为 MEMTX_ERROR，读失败清空输出，部分写保留 dirty
通知，避免调用者把失败结果提交到 DMA baseline。实际函数提取到 memfd
测试，负错误、正 partial-copy、普通 RAM 路径通过。

5367c7124a 在 CPU→设备提交同步中合并注册顺序中相邻 GPA 的读取，
每批最多 64 KiB，可向前或向后延伸；不读取空洞、不重排 merge、不跳过
标准 RX 映射，设备→CPU 的两阶段完成发布顺序保持。暂存区每个 DMA
context 只分配一次，释放时回收。真实源码测试覆盖正反顺序、独立页内容、
跨空洞、64 KiB 上限、设备字节保留、读取失败/重试、map/unmap/partial
unmap 及 24 种完成交错；两端实际 QEMU 构建通过。

两端启动新版、guest #25，原生 iavf 两队列/128 项 RX/TX ring。
一次双端各 4 MiB RX 的长度/内容均通过，host 耗时约 0.586/0.687 秒、
TCP 重传 0/1，刷新统计后两 VF rx_discards 增量均 0。之前的短样本
未节流时丢包 3/0，而 16 KiB/1 ms 目标小批次发送为 0/0；不是严格
限速或稳定吞吐对照，不能由这些样本认定 RX 丢包已根治。

覆盖本次 RX 与空闲期的 15 秒实际提交同步探针：
Garen 296 次完整提交同步，58600 次 DSM_MEMCPY 读，487403520 字节；
Ezreal 288 次完整提交同步，29282 次读，420564992 字节。
可观察到 8–64 KiB 调用。若同一批字节全部按 4 KiB 读取，分别需要
118995/102677 次调用；本样本调用数少约 50.8%/71.5%。这是相对于
同字节数逐页调用的计算，非相同条件旧版 A/B，也不是减少了这些 DSM
页面访问或 RDMA 数据量。探针包含成本，不能将同步耗时视为纯 CPU。

一次未采样完整 DSM 的 6 秒双卡四 worker TX 校验通过、短时 33.58
MiB/s、四个连接 TCP total_retrans=0；同轮双盘四 worker
O_RDONLY/O_DIRECT 回归通过、短时 3235 MiB/s，均非正式性能基线。
随后带 DSM 探针的 TX 为 21.67 MiB/s、数据仍通过，但出现 guest
enqueue_dl_entity 的重复入队 WARN，本轮不可作为干净性能结果。

18 秒含双卡发送的 DSM 探针（kvm_dsm_fetch 的 req 是 arg3，
GFN 位于 req+8）显示活动秒每端约 1.3万–1.7万次 fetch，空闲秒约
几百次。主要来自 vCPU；同步内提交/完成 fetch Garen 21/73、
Ezreal 9/8 次。最高热点 GFN11270，随后 14428、11271、14429，
对应既有 HPET/时间维护共享路径；并非网卡 payload 的大量搬迁。
非同步 fetch 累积阻塞耗时 Garen 1.963 秒、Ezreal 1.922 秒，
是各线程累积，不能视为整个业务的壁钟占比。HPET 代码和配置不改。

警告现场为 CPU2 migration/2 → move_queued_task → enqueue_task_fair
→ dl_server_start → enqueue_dl_entity，WARN_ON_ONCE(on_dl_rq)。
当前 Linux6.12 guest 缺少上游 b53127db1dbf（重复入队修复）及
c7f7e9c73178（只为 active server 记账）。症状/源码与上游问题吻合，
尚不能仅凭 WARN 证明是此问题或排除 DSM。已正常关闭自己的两端 QEMU，
再回移这组修复构建 guest；未重启 host，未触碰 Lux/原 libvirt VM。
测试进程已清理，无新增实验脚本、日志或结果文件。

guest 回移提交为 8d19eb6e8/ab3a8e924，已推送
https://github.com/qmyyxtl/GVM-guest-kernel/tree/device-aggregation 。
#26 完整构建、两端重新启动并登录；read_hpet 地址/长度/机器码 SHA256
与 #25 完全相同。两 VF 队列/ring 重配置、ping、两进程共 320 次跨节点
亲和性切换及后续双卡四 worker TX、双盘四 worker只读回归通过，
未再出现 enqueue_dl_entity 警告，不能以有限运行证明永不复现。
#26 的 RX 内容正确，但更快的突发发送出现 rx_discards 12/65、
TCP 重传 12/65，仍需定位 RX 队列处理延迟。最终短时 TX 25.48 MiB/s，
四个连接重传 0/0/0/1；只读双盘 3222 MiB/s。两者仅为功能回归观测，
未运行同条件多轮单卡/双卡或单盘/双盘正式对照，不能声称性能提升或
线性扩展。所有 receiver/sink/observer 已退出，自己的 QEMU 保留运行。

### 完成发布遍历优化与 AdminQ 新现场（2026-10-10）

QEMU 0545661c37 保持旧→新快照、新→旧发布的两阶段顺序，
缓存 buffers 链表尾部，只串联本轮已有快照的待发布 buffer。
每轮不分配新对象；注册、解除注册及部分解除映射维护尾指针，
失败重试重新建立快照链。CPU→设备的完整扫描和 GPA 合并读取不变。

同一 Ezreal 上提取实际源码到 memfd，普通 RAM mock、不含 DSM：
1178 个未变化的 4 KiB buffer、每轮 200 次完成同步，旧源码三个样本
401.3/398.1/398.2 us，新源码 366.9/365.5/365.6 us，下降约 8.2%。
旧版分阶段 warm 样本为找尾 8.9 us、快照 356.0 us、
发布遍历 30.6 us。只覆盖这类缓冲区的本地开销，不能推算真实 NIC 带宽。
实际源码 map/unmap/partial unmap/最后发布/失败重试、64 KiB GPA 批次、
24 种完成交错，以及最旧 buffer 切分/删除、唯一 buffer 删除后重注册、
写回失败重试均通过；两端实际构建、Git 同步、正常重启自己的 QEMU。

guest 仍 #26，HPET/guest 源码/host 内核均不改。两 VF 重配至两队列、
128 RX/TX ring 及 ping 通过。双端各 4 MiB 接收内容/长度正确，
host 重传 4/19；双卡四 worker 发送内容/长度正确、重传全 0，
短时 18.80 MiB/s；双盘四 worker O_RDONLY/O_DIRECT 回归通过、
短时 3197 MiB/s。这些仅是回归观测，未证明真实吞吐提升或线性扩展。

发现 owner0 的硬件统计未刷新：软件 Tx/Rx 队列计数增长，但 hardware
rx_bytes/tx_bytes/rx_discards 全为 0，继续等待 10 秒仍未更新。
owner1 rx_discards 为 17；不能把 owner0 的 0 当作零丢包。
10 秒 host PF i40e_vc_process_vf_msg 探针中，Ezreal 收到 5 次
GET_STATS(opcode15)，Garen 无新请求。guest 的可回收 tracefs 临时
kprobe 显示两个 adapter 都在 RUNNING(state13)：owner0 watchdog
约 432 次/10秒，current_op=GET_STATS(15)、aq_required=0x20000000
(REQUEST_STATS) 一直未清除；owner1 watchdog 5 次、op0/aq0，
相应 request_stats 5 次。源码对 pending current_op+asq_done 不重发，
aq_required 非零时以 20 ms 重排，因此 owner0 等待统计回复并频繁唤醒。
这是具体的控制路径异常/额外开销；仍需沿 AQ 完成/邮箱 IRQ/消息解析
定位丢失的位置，尚未证明与新优化有关，不能只归因 DSM 或时钟。
未修改/关闭 HPET，未绕过响应检查或强行清除 current_op。

最终 guest 无 WARNING/BUG/DMA fault/QI error/TX timeout；
临时 guest kprobe 全部移除、tracing_on 恢复，host BPF 已结束，
receiver/sink/serial observer 已退出，无新增实验文件。
两端自己的 QEMU 保持运行，Lux 和 Garen 原有 libvirt VM 未动。

### BAR 读取与 DMA 完成发布顺序修复（2026-10-10）

QEMU a7292672aa 修正 vfio_region_read 的顺序：先读取物理 BAR，
再发布一次设备 DMA 完成快照，最后返回已保存的 BAR 值。
此前先同步 DMA 再读取 BAR，设备若在两者之间完成 AdminQ，
guest 会读到已前进的 ARQ head，却仍看到旧 descriptor/payload。
iavf 按 head 消费并回收这个空回复，GET_STATS 留在 pending，
watchdog 因 REQUEST_STATS 未清除持续以 20 ms 重排。

实际源码的受控交错验证：旧版本返回 head=1，但 guest opcode=0、
DD=0、payload=00；修复后 head=1、opcode=15、DD=1、payload=55。
短 BAR 读、DMA 发布失败、1/2/4/8 字节读取及普通非分布式路径
也通过验证；未增加同步次数。两端构建、推送原 origin、正常关闭并
重新启动自己的 QEMU 后，两张 VF 的硬件统计均恢复更新。

负载前后分别观察 10 秒，两个 adapter 的 watchdog 均各运行 5 次，
GET_STATS 均各解析 5 个成功回复（104 字节）；host PF 同样各收到
5 个请求。未再观察到持续 pending 和 20 ms 重排。有限窗口不能
证明所有完成交错均已覆盖。

双端各 4 MiB RX 长度/内容通过，但 rx_discards 为 173/112，
TCP 重传为 173/113，RX 处理仍有瓶颈。短时双卡四 worker TX
21.66 MiB/s、全部内容正确、四连接重传均为 0；
双盘四 worker O_RDONLY/O_DIRECT 回归 3115 MiB/s。
这些不是正式性能对照，不能声称吞吐提升或线性扩展。
负载后的 guest 无 WARNING/BUG/DMA fault/QI error/TX timeout。
guest 仍为 #26，HPET、host 内核不变；临时探针和收发进程均已清理，
无新增实验文件。两端自己的 QEMU 保留运行。

### DMA 变化区间扫描优化（2026-10-10）

4 MiB/端的实际 RX 内容校验通过。仅在 QEMU sync/BAR 入口加
可回收 uprobe：Garen 活动秒提交/完成分别 267/541 次，累积
73.3/70.0 ms；Ezreal 两活动秒共提交 267 次、完成 534 次，
累积 135.0/129.9 ms。这些是带探针的累计函数壁钟，包含 DSM
等待，不能全部认作本地 CPU 时间或端到端占比；探针已清理。

merge_buffer 对变化/未变化的长区间按 8 字节跳过，遇到混合
word 仍逐字节寻找精确边界。XOR 的零字节检测判断是否存在相同
字节；memcpy 读取 word，兼容未对齐和短尾。仍只发布变化字节，
不覆盖 CPU 对其他字段的更新，不减少 DMA/DSM 同步，不改变
描述符/数据两阶段发布顺序。

普通 RAM、实际源码、4 KiB、每样本 10000 次合并：整段变化旧
约 1.6 us/新约 0.5 us，32 字节交替变化旧约 2.6 us/新约
1.7 us，仅末尾变化旧约 1.9 us/新约 0.35 us。
10000 组长度/变化模式的双方向合并目标及 baseline 一致，并验证
未变化字节保留目标中的独立更新；map/unmap 生命周期、错误重试、
GPA 批次及 24 种完成交错检查通过。不包含 DSM，不构成端到端
性能提升证明；真实设备部署结果另行补充。

415f82aca9 已在两端构建并正常重启自己的 QEMU；guest #26/HPET
不变。两 VF 的队列重配/ping 通过，各 4 MiB RX 内容/长度通过，
rx_discards 增量 69/39，仍有丢包。双卡四 worker TX 内容/长度
通过，短时 25.62 MiB/s，连接重传 0/1/0/1；双盘四 worker
O_RDONLY/O_DIRECT 只读回归 3222 MiB/s。未进行同条件多轮
正式基线，不把这些波动解释为优化带来的吞吐提升。
负载后 10 秒两 adapter 各 5 次 watchdog/5 个成功 GET_STATS，
无 WARNING/BUG/DMA fault/QI error/TX timeout，clocksource=hpet。
收发进程、observer、临时探针已退出/清理。自己的 QEMU 保留运行，
host PF up；Garen 原 libvirt VM、host 内核、Lux 未动。

### RX ring 深度对照与下一步约束（2026-10-10）

当前 QEMU 功能版本 415f82aca9、guest #26 不变，2 个 queue，
TX ring 固定 128，仅 RX ring 在 128/256 间调整。
每轮双端同时接收各 4 MiB、内容/长度验证；每配置 3 轮。
同步/BAR uprobe 在 receiver ready 后开始计数、sender 完成后读取，
不把配置/reset 时间计入。计数窗口仍有少量空闲控制请求，
耗时包含 DSM/调度/探针开销，不是纯 CPU 成本。先测 256、
再恢复 128，流量使用不同 TCP 源端口/RSS 队列，样本不足以
声称统计显著性能变化。更早的宽窗口 128 试测不纳入三轮汇总。

| RX ring | owner0 三轮 rx_discards 增量 | owner1 三轮增量 |
| --- | --- | --- |
| 128 | 141, 0, 0 | 42, 59, 0 |
| 256 | 266, 0, 0 | 0, 67, 0 |

所有内容校验通过。两配置每端每轮 RX tail 写入约 181–184 次，
加深 ring 没有降低该通知数。guest 源码 iavf.h 的
IAVF_RX_BUFFER_WRITE=16，iavf_clean_rx_irq 在累计 16 项时补充
descriptor，iavf_release_rx_desc 经 wmb 后 writel 新 tail。
约 4 MiB / 1448 字节 / 16 与观测通知数量吻合，不能用 ring 深度
推导通知批次增大。

三轮同步累计耗时中位数（提交/完成，毫秒，包含 DSM 等待）：
128：Garen 145.4/112.3，Ezreal 65.6/94.5；
256：Garen 117.3/122.1，Ezreal 147.7/144.2。
丢包、耗时仍有波动，不能宣称 256 稳定更优，现已恢复 RX/TX
128/128；guest/HPET/host 内核不改。

下一步应评估 QEMU 在 RX tail 上按队列提交范围同步，而非
再次增加 ring 或仅根据 DMA permission 跳过 payload。
目前 VFIO 路径没有解析 virtchnl 队列配置，不能直接安全启用：
需从 AdminQ 的 CONFIG_VSI_QUEUES 请求/成功回复建立
queue id、ring IOVA/长度、RX buffer 大小及代际；RX tail
0x2000+4*queue_id 给出新增 descriptor 范围（含回绕）。
提交应覆盖这些 descriptor 及其重新交给设备的 payload，
维持完成发布与 tail 的顺序；配置失败、reset、disable、部分
unmap、未知格式必须失效或回退全量同步。完成侧仍保持现有
快照/发布顺序。此队列路径尚未实现，也不据此声称通用 VFIO 优化。

恢复配置后 10 秒，两 adapter 各 5 次 watchdog/5 个成功统计
回复；无 WARNING/BUG/DMA fault/QI error/TX timeout。
临时 BPF/guest probes、receiver/sender/observer 均已清理。
两端自己的 QEMU 保留运行，host PF up，无新增实验文件。

### 范围提交同步接口及原有子范围修复（2026-10-10）

为队列范围提交增加 loom_vfio_dma_sync_to_device_range：
接受 IOVA/长度，先检查注册 buffer 对整个区间的覆盖，再按原有
注册顺序提交相交部分，保留 IOVA→GPA 转换和每个重叠注册对象
的 baseline。同步仅针对指定字节，借用原有 pool/baseline/scratch，
不改变注册/引用计数/生命周期。空洞返回 ENOENT，非法长度/溢出
返回 EINVAL，DSM 读取失败返回 EIO。覆盖缺失时尚未发布任何
变化；运行中读取失败仍可能已完成前面部分，调用者须按现有
DMA 错误路径停止，不把 EIO 当成安全快速路径回退信号。

同时修正既有 LOOM_DMA_SYNC_CPU/DEVICE 子范围命令：原代码
找到包含范围的 buffer 后，仍把整个 buffer 传给 sync_buffer；
现在传入只覆盖请求范围的临时视图。实际源码普通 RAM 检查中，
原版本请求 7 字节却把范围外的 0 更新成 0x55，修复版范围外
保持原值。CPU 完成方向同样只发布选定的 9 字节。

实际源码 memfd 检查覆盖：非恒等 IOVA/GPA、跨 4 KiB 边界、
指定范围外的 pool/guest 字节保留、后续全量同步仍发现剩余变化、
读失败、零长度/溢出/空上下文、解除中间映射后的覆盖空洞、
已有 direction=2 的提交过滤。原有生命周期/失败重试、GPA 批次
及 24 种完成交错检查也通过。

该接口尚未接入真实 VF 的 RX tail，原生 guest 的稳态路径仍
采用原有全量同步。因此只证明范围接口/命令语义，不宣称本轮
减少真实网络扫描或提高带宽。后续仍须建立并验证 AdminQ 队列
配置、tail 回绕、payload 复用以及 reset/unmap 失效机制后接入。
guest #26、HPET、host 内核不改，无新增实验文件。

### 原生 X710 AdminQ 队列配置观察器（2026-10-10）

新增 loom-iavf.c：仅在原生 X710 VF 的 AdminQ BAR 控制写、
ARQ head 读以及 vector0 完成中观察，不在 RX/TX tail 数据路径
额外读取 guest RAM。解析 virtchnl v1 的 CONFIG_VSI_QUEUES，
记录候选队列的 RX/TX IOVA、长度及 RX buffer 大小；收到对应
成功回复后才确认配置。此阶段不启用范围快速路径，原有全量
同步仍执行，valid_mask 只表示观察到了配置 ACK，尚不表示队列
已 enable 或可以省略全量同步。

读取使用 QEMU 已完成发布的 baseline，避免再次读取 live DMA
pool 将新 descriptor 与旧 payload 混合。DMA 映射/注册失效
代际变化、AQ 重配、VF reset、disable、未知队列配置形式、
重复待确认配置、失败回复均使缓存失效。代际失效目前较保守，
包括与 ring 无关的映射变化，后续须验证有效覆盖再细化。
PF reset impending/driver close 事件亦失效；设备销毁释放状态。

实际源码普通 RAM 协议检查验证：请求未 ACK 不确认；
hardware pool 已写 ACK、但尚未发布时不确认；发布后的成功
ACK 才确认两队列；失败 ACK、重复 queue id、多个待确认请求
的歧义、映射代际变化、非法 tail、reset/free 均正确处理。
原有范围语义、生命周期/错误重试及 24 种完成交错检查通过。
这不是带宽提升证明；真实设备观察结果另行补充。

真实 VF 重配的只读探针发现，当前 Linux virtchnl_struct_size
采用 LEGACY_SIZEOF 保留一个额外 queue-pair-sized 槽位：
两队列消息长度为 200 字节，而非紧凑格式的 136。
观察器补充同时接受精确紧凑长度及精确 legacy 长度，并按声明
的 num_queue_pairs 解析有效条目；16 队列的读取缓冲也包含
legacy 槽位。两种格式的 ACK/失败/歧义/代际检查均通过。
最初部署时未确认队列配置，仍执行全量同步，不能声称已接通
快速路径或改变网络性能。真实重配后再验证兼容版本。

### AdminQ 间接地址的符号扩展修复（2026-10-10）

真实 VF 的 CONFIG_VSI_QUEUES 只读探针观察到，描述符中的地址
高半部为 0、低半部为 0xffdff000，但 baseline 查询实际收到
0xffffffffffdff000。QEMU ldl_le_p 返回 int，拼接低半部时发生
符号扩展，并非该地址的 DMA buffer 真正丢失。
配置请求与 PF event 两处地址拼接均显式转换为 uint32_t 后
组成 uint64_t。普通 RAM 检查使用与 QEMU 相同的有符号读取
接口和非零高半部/bit31 置位低半部；旧版复现 pending 断言
失败，修复版通过 ACK、失败、歧义、映射失效和 reset 检查。
原测试 mock 的无符号返回值掩盖了该错误。

控制写观察移到现有完整 DMA 同步之后、物理 BAR pwrite 之前，
先记录提交再通知硬件；写失败仍清空状态。仍未启用队列范围
快速路径，也不把此修复称为此前吞吐损失的原因。

94483dd19e 两端构建并部署后的真实 VF 检查：Garen/Ezreal
均先观察到默认四队列配置 mask=0xf，改为两队列及 RX/TX128 后
各观察到成功回复 mask=0x3。双网卡 ping 均 3/3，通过原生
iavf 分别接收 4 MiB，逐字节内容校验通过；这一轮 RX 丢包及
host TCP 重传分别为 owner0 271、owner1 185，瓶颈仍存在。

10 秒 AdminQ 检查两卡各 5 次成功 GET_STATS，未发现
WARNING/BUG/DMA fault/QI error/TX timeout；HPET 仍为 hpet。
guest 启动仍出现既有 hrtimer interrupt took 提示，以及
sda15 FAT 非正常卸载提示，未在此阶段调整时钟或文件系统。

短回归双网卡四 worker 发送约 21.80 MiB/s，四条流内容校验
均通过，TCP total_retrans 均 0；双 raw 数据盘四 worker、
每 worker QD8 的 O_RDONLY/O_DIRECT 读取约 3274.90 MiB/s。
这些为一次功能回归，不能与此前不同轮次值直接推导性能变化，
也没有证明线性扩展。两端仍执行完整 DMA 同步，后续需确认
enable 状态、精确 buffer 覆盖/生命周期、tail 回绕及 payload
复用后，才能把范围提交接口接入原生 VF 的稳态路径。
临时探针、接收/发送进程均已退出，无新增实验文件；
guest #26、host 内核、HPET 不改。

### 队列配置的范围失效与 enable 回复（2026-10-10）

用控制路径的范围通知替代整个 DMA epoch 改变就清除配置：
unmap 成功发布后、最终 unregister 或新注册对象插入前，对
该 container 中设备观察器通知变化的 IOVA 范围。涉及 ASQ/ARQ
ring 时清空观察状态；涉及已确认或待确认的 RX/TX ring 时
清除队列配置和 enable 状态；未涉及这些 ring 的数据缓冲区
变化保留配置。未确认请求被 ring 失效中断时保留歧义标记，
不把旧 ACK 关联到新配置；container 销毁亦清空状态。

增加无 DMA 复制的完整注册覆盖检查，CONFIG 请求及成功 ACK
时必须具备完整 TX/RX ring 覆盖。仍执行完整 DMA 同步，没有
直接访问未同步的硬件 pool，也没有启用稳态范围快速路径。

分别记录配置成功与 ENABLE_QUEUES 成功：解析精确 12 字节
queue_select，校验 VSI 及 RX/TX 掩码是已配置队列的子集，
请求只进入 pending；发布后的 PF 成功 ACK 才确认 enable。
失败、重复请求歧义、disable、VF reset 等仍采用保守失效。

实际源码 memfd 检查通过：有符号 accessor/高地址、配置 ACK
发布顺序、完整覆盖及空洞、无关映射注册/释放保持配置、
失败发布保留原登记、引用递减及最终 unregister、ring unmap、
相同 IOVA 重新映射不恢复旧配置、AdminQ ring 失效；
enable ACK 发布顺序、RX/TX 不同掩码、失败回复、未知队列、
重复请求及 disable。原有范围提交、生命周期/错误重试、
64 KiB GPA 批次和 24 种完成交错检查也通过。
真实设备的状态保留验证与吞吐验证另行记录，不提前宣称提升。

c7d35b7afe 两端构建并部署后，真实 VF 两队列配置确认成功。
20 秒只读 BPF 窗口内，Garen/Ezreal 分别收到 478/2919 次
范围变化通知，24/27 次完成观察采样全部保持 config=3、
enabled_rx=3、enabled_tx=3，pending/ambiguous 均为 0。
证明当前收发中的无关映射变化没有误清除队列状态；不能将
采样解释为所有时间点的完备证明，也不等同于快路径启用。

双网卡各接收 4 MiB、内容校验通过，本轮 owner0/owner1
RX discards 为 14/0、host TCP total_retrans 为 14/2。
10 秒 AdminQ 检查两卡各 5 次成功 GET_STATS，未发现定义的
WARNING/BUG/DMA fault/QI error/TX timeout，HPET=hpet。
短回归双网卡四 worker 发送 20.53 MiB/s，四流校验通过、
total_retrans 均为 0；双 raw 数据盘四 worker、每 worker
QD8 的 O_RDONLY/O_DIRECT 读取 3249.95 MiB/s。
仅为一次功能回归，不宣称相比前轮提升或退化。guest 启动仍有
既有 printk 时间偏移/underflow 和 sda15 FAT 非正常卸载提示，
此次没有修改 HPET 或文件系统。
另补普通 RAM 多页 RX ring 部分 unmap 检查：剩余映射仍在，
但完整覆盖及配置立即失效；检查通过。

探针、发送/接收进程及临时 TCP sink 均已退出。guest #26、
host 6.6.32-krdma+ 保持，未生成实验文件。
下一步仍是 RX/TX tail 的精确 descriptor/payload 范围，
需正确处理首次 tail、回绕、RX buffer 复用和完成发布顺序；
目前 BAR/IRQ 路径仍执行完整 DMA 同步。

### 原生 RX tail 范围提交（2026-10-10）

BAR 写路径仍先完整发布 device→CPU 完成数据；对于已确认配置、
已成功 enable 且已观察前一次 tail 的 X710 RX 队列，CPU→device
提交改为原生 iavf 的 [old next_to_use,new next_to_use) 描述符区间。
该驱动还清除下一条描述符的 status qword，因此同时提交新 tail
指向条目的 offset8、8字节。回绕分成两个区间；首次/相同/越界
tail、未知队列、未启用、失效/歧义状态、非原生布局和覆盖缺失
均回退到原有完整提交。TX/AdminQ/其他设备路径不变。

在已有 IOVA→GPA 注册上读取 CPU 描述符，预检全部 RX payload
及 ring 覆盖后才发布。先逐范围合并 payload，再合并描述符和
下一状态字，最后写物理 BAR。不能省略 RX payload：同一地址
复用时 CPU 可能改写内容，权限标志不足以证明没有 CPU 修改。
中途 DSM 读取失败返回错误并停止 VM，不尝试全量回退或 doorbell。
描述符临时空间缓存在观察器内，稳态不做 malloc 或大块栈分配。
LOOM_IAVF_RX_SCOPED=0 可在 QEMU 启动时强制原有完整提交，用于对照。

实际源码 memfd 检查通过：首次回退、普通前进/回绕、下一状态字、
未涉及描述符/缓冲区字节保留、原地址 payload 复用的 CPU canary、
先完成发布的硬件字节保留、非恒等 IOVA/GPA、跨登记 CPU 读取、
未知/越界/相同 tail、header/映射空洞预检回退、读取失败及部分
发布后的错误返回、ring unmap 失效。实际 helpers.c BAR 检查验证
完成→范围/全量提交→控制观察→物理写顺序，以及同步错误阻止物理写。
既有队列 enable/失效、范围提交、生命周期、64 KiB GPA 批次和
24种完成交错检查均通过。真实设备命中率、健康及性能另行记录，
不能将普通 RAM 检查当作真实吞吐提升证明。

8c86a83026 两端构建并部署后的真实 VF 检查：两卡各接收 4 MiB，
内容校验均通过。20 秒只读 BPF 窗口内，Garen 的 181 次 RX tail
提交中 180 次范围同步、1 次首次回退；Ezreal 的 183 次中 182 次
范围同步、1 次首次回退，rx_error 均 0。因此该窗口明确避免
180/182 次原本的全量 CPU→device 提交扫描，不仅是观察器就绪。

范围 API 调用及请求字节分别为 Garen 3262/4517280、
Ezreal 3298/4567472；CPU 描述符读取请求分别为 202/92160、
204/93184。字节数是请求范围累计，不等于实际改变/复制的字节数。
其他控制/TX 通知仍有 100/114 次完整提交，完成方向仍有
629/697 次同步调用，下一阶段必须继续削减这两部分扫描。

本轮 owner0/owner1 RX discards 为 78/14、host TCP 重传为 78/14，
没有证明丢包问题已解决。10 秒 AdminQ 检查两卡各 4 次成功
GET_STATS、watchdog 各5次，未发现定义的 WARNING/BUG/DMA fault/
QI error/TX timeout，HPET=hpet。未挂探针的短回归双网卡
四 worker 发送 29.34 MiB/s，四流内容校验通过、total_retrans
均为0；双 raw 盘四 worker、每 worker QD8 的 O_RDONLY/O_DIRECT
读取 3168.04 MiB/s。单轮结果没有交替关闭快路径的同条件对照，
不能据此断言吞吐提升幅度或线性扩展。

guest #26、HPET、host 内核不改；全部临时 probes、sender、
receiver、TCP sink、serial observer 已退出，无新增实验文件。

### 原生 TX tail 范围提交（2026-10-10）

在相同的完成→提交→物理 BAR 顺序下，已配置、已成功 enable、
已有上一 tail 的 X710 TX 队列按 [old next_to_use,new next_to_use)
解析原生 16 字节描述符。dtype0 数据描述符同步其 64 位 IOVA、
14 位长度对应的精确 payload；dtype1 原生 TSO/VLAN/tunnel context
不把 qword0 当 DMA 地址，校验 reserved 字段后只提交描述符。
未知 dtype、done、零长度、未知 context 位、覆盖空洞、首次/
相同/越界 tail、未 enable 或失效状态均回退完整提交。
回绕分两段；先预检全部 payload/ring，再提交全部 payload、
当前 descriptor 区间，最后物理 tail。TX 不复制 RX 的下一
status qword，因为原生 TX 驱动没有对应的写操作。
LOOM_IAVF_TX_SCOPED=0 可在 QEMU 启动时关闭，RX 开关独立。

复用观察器内的提交临时空间，新增每队列 TX tail/known 和
每 fragment 的长度缓存；配置失效、ring unmap、disable、
reset 等同时清除 RX/TX tail 状态。完成方向仍使用已有全量
同步，不改变完成发布顺序，也不改 guest。

实际源码 memfd 检查通过：context+数据、跨映射 fragment、
TSO/context 不查伪地址、非恒等 IOVA/GPA、普通/回绕 tail、
范围外 payload/descriptor 保留、先发布的硬件字节保留、
未知/done/零长度/reserved/未知 context 位回退、全部范围预检、
CPU 读取错误及部分 payload 已提交后的错误、ring unmap 失效。
RX 范围/复用、原队列 enable/失效及实际 BAR 顺序/错误检查通过。
真实设备命中率与内容校验结果另行记录，不提前宣称吞吐提升。

4417bfac06 两端构建并部署后的真实 TCP 检查：20 秒只读 BPF
窗口（包含双网卡四 worker 的6秒发送及后续双盘只读回归）内，
Garen/Ezreal 的 TX tail 分别 2473/2486 次，全部范围同步，
没有 TX fallback 或 error；warmup 已建立初次 tail。
同窗口 RX 为 298/359 次，范围同步 297/358 次，首次回退各1、
error均0。完整提交只剩31/30次，完成同步仍11679/12061次，
因此下一主要候选是完成方向扫描，而非继续全量提交。

范围调用/请求字节为 Garen 19658/99089950、
Ezreal 24691/138421016；CPU 描述符读取 2897/381360、
3002/474016。累计请求字节不等于改变/复制的字节数。
带探针的短回归发送约34.34 MiB/s，四流内容校验通过，
TCP total_retrans 按 cpu0..3 为4/0/0/0；双 raw 数据盘四
worker、每 worker QD8 的 O_RDONLY/O_DIRECT 读取3224.25 MiB/s。
不是关闭快路径的同条件交替对照，不能据此宣称吞吐提升幅度。

之后无探针双网卡各接收4 MiB、内容校验通过，owner0/owner1
RX discards为3/0，host TCP total_retrans为3/0。10秒AdminQ
两卡各4次成功GET_STATS、watchdog各4次，未发现定义的
WARNING/BUG/DMA fault/QI error/TX timeout；HPET=hpet。
临时probes、TCP sinks、sender/receiver/serial observer已退出，
guest #26、host内核和HPET未改，无新增实验文件。
后续仍需削减完成方向扫描和范围查找成本，并进行受控性能对照；
提交快路径命中不能证明总体目标完成或线性扩展。


### Native 4 KiB DMA indexing (host-side, guest unchanged)

A host-userspace-only CPU profile of a 12-second dual-NIC TX workload
(cpu-clock:uH, excluding guest execution, host kernel time and blocked DSM
waits) found substantial costs outside DSM. On Ezreal, self samples included
completion memcmp 32.52%, DMA unmap 14.14%, coverage lookup 7.54%, map 4.52%,
and registration 3.30%. These are sampled CPU proportions, not proportions of
workload elapsed time. Inclusive call-tree percentages overlap and must not be
added. The profile is diagnostic evidence, not a controlled throughput A/B.

DMA containers now maintain page-to-map and page-to-buffer hash indexes while
buffer registrations use aligned, disjoint 4 KiB IOVA pages on
a 64-bit host with 4 KiB pages. Other buffer layouts use the general lists;
indexes can be reconstructed after incompatible buffers are invalidated.
Large maps use a separate lookup list without disabling native page lookups. Mapping conflicts, references and
final unregisters retain their existing semantics. Cached list links allow
native single-page unmaps without whole-list searches or tail recomputation.
Device writes are published before removal; publication failures preserve the
mapping and index for retry. General partial unmaps retain slice positions.

Coverage, CPU/baseline reads and small submission-range lookups use the index.
Submission ranges touching at most 16 pages use a bounded stack array sorted
by registration sequence, preserving newest-registration-first publication
and failure order even when IOVA order differs or GPAs alias. Larger ranges
retain the general ordered traversal. Completion snapshot and publication
order remain unchanged; completion still scans registered buffers.

Build and actual-source in-memory checks cover IOVA zero, duplicate/conflicting
registrations, references, final unregister, failed unmap and retry, multi-page
unmap, IOVA reuse with another GPA, GPA aliases, range publication order on
failure, invalid sub-page invalidation, and fallback with partial-map splits.
Existing RX/TX reuse, wrap, preflight/error, late-DMA completion, batching and
BAR doorbell-order regressions pass. Runtime rollout and real-device validation
of this revision are tracked separately; no scaling improvement is claimed
from these source checks. Guest and HPET are unchanged.

Runtime inspection of the first index revision found zero hits despite native
4 KiB traffic mappings: boot-time identity mappings (up to 2 GiB) and recurring
16 MiB device-memory mappings had disabled the whole-container index. The mixed
map revision keeps those maps in a separate list, indexes native maps and
buffers, and reconstructs indexes after non-page buffer invalidations. This is
why source regression success alone is insufficient evidence of optimization.


#### Mixed-map runtime validation

Revision 6a4917adf4 was built on both owners and started with the unchanged
guest kernel build #26, 4 vCPUs, 4 GiB, two NUMA nodes, two queues per VF and
128-entry RX/TX rings. A 35-second diagnostic window containing dual-NIC TX
and dual-disk read-only activity observed these index-enabled operations:

| Owner | DMA map | Submission range | DMA unmap | General-state observations |
| --- | ---: | ---: | ---: | ---: |
| Garen | 67247 | 40632 | 67131 | 0 |
| Ezreal | 42160 | 28333 | 42032 | 0 |

The probes inspect index state rather than assert every internal instruction
path; sampled map requests were 4 KiB. Native and mixed-map source checks also
cover a page buffer inside a larger pool, page invalidation splitting that pool,
retained pointer/index consistency, and remapping the invalidated page.

All four TX streams passed receiver byte-content checks. A six-second diagnostic
run reported 58.15 MiB/s and dual raw O_DIRECT/O_RDONLY reads reported
3205.26 MiB/s. A separate 12-second TX run during host CPU profiling reported
42.50 MiB/s with all four receiver checks passing and retransmits 0/1/1/3.
These runs are not a controlled performance A/B or proof of linear scaling.

The subsequent host-userspace-only profile reported Ezreal memcmp self samples
50.36%, snapshot_from_device 8.97%, and loom_vfio_dma_sync 3.59%. DMA map, unmap
and range-coverage symbols no longer appeared above the 0.5% report threshold.
Garen also no longer listed those three symbols above that threshold, but many
libc samples remained unresolved, so no matching memcmp percentage is assigned.
Changing sampled CPU proportions does not measure total elapsed-time savings
or DSM wait fractions.

Both RX streams received and validated 4 MiB. Owner-0 RX had 75 discarded
packets and the corresponding sender had 75 retransmits; owner-1 had neither.
Thus payload correctness passed, while burst receive service remains a
performance concern. Afterwards each VF completed five GET_STATS control
requests over ten seconds, with no tested DMA/queue/error signatures and HPET
still selected. The existing boot FAT warning is not covered by that statement.
All diagnostic probes, receivers, senders and TCP sinks exited; the guest
remains running. Full completion scans and their publication ordering are the
next optimization target. No guest or host-kernel source was modified.


### Scoped native data-queue completion (guest unchanged)

The data-vector interrupt path and native RX/TX tail or data-vector interrupt
control writes can now synchronize acknowledged native data queues rather than
scan every registered DMA buffer. AdminQ vector zero, control reads/writes,
unknown layouts and containers shared by multiple devices retain full DMA
synchronization. LOOM_IAVF_COMPLETION_SCOPED=0 selects that full fallback for
comparison. Errors stop the VM before the physical doorbell or guest interrupt.

RX addresses are cached from published CPU baselines before ENABLE_QUEUES lets
hardware erase them. Rearm updates the cache after CPU submission and before
the physical tail. Each cached buffer stores its first/last page registration
lifetimes; currently the scoped layout supports receive buffers up to 4 KiB,
covering at most two native pages. IOVA reuse with another registration cannot
reuse a cached address. Unknown or unsupported state falls back. Reset and
queue invalidation free the caches. Scratch storage is allocated at queue
configuration, with no allocation in steady completion handling.

The data path reads DD before capturing RX metadata, captures all selected
descriptor snapshots before any payload publication, and publishes payloads
before those immutable snapshots. RX metadata and length are published before
the byte containing DD. It never re-reads a live descriptor after publishing
payload, which would expose a late completion without matching data. Full-path
completion reconciliation advances the cached cursor before CPU rearm can
clear already-published DD bits. TX snapshots cover only configured rings;
TX data payloads are hardware input and are not completion-scan candidates.

New DMA APIs capture device staging bytes and publish exact immutable snapshot
ranges through the existing changed-byte merge. Coverage is checked before
guest writes; unrelated CPU bytes and out-of-range baselines are preserved.
Native page lifetime identities survive index reconstruction and change after
unregister/remap. General full completion snapshot ordering is unchanged.

Build and actual-source in-memory regressions pass for late DMA, DD-last
metadata/payload visibility, RX reuse, ring wrap, first/last-page lifetime ABA,
failure/retry without premature DD or cursor advancement, unknown/header-split
fallback, shared containers, cache reset/reconfiguration, and published
completion reconciliation before rearm. Actual BAR and MSI-X handlers are
checked for publication-before-doorbell/notify and error suppression. These
source checks do not yet establish runtime fast-path hits or throughput gains;
real-device rollout is recorded separately.

### Native RX first-tail completion cache

The first RX tail excludes its next-to-use sentinel. Native iavf can clear
the sentinel status while retaining RSS writeback in qword 0; it is not a
posted DMA address. Runtime probes on both owners found first-tail slot 15
with tail 15 and an invalid RSS-shaped address, disabling scoped completion.
Skip that sentinel only for the first-tail harvest; subsequent
[old_tail, new_tail) ranges already exclude it. An actual-source regression
retains RSS in the sentinel with cleared status: the old implementation
fails, the corrected cache remains seeded. Scoped completion, DD-last,
MSI-X publication and BAR error-order regressions pass. Runtime performance
after this correction remains to be verified. Guest and HPET are unchanged.

## 2026-10-10：VFIO owner 暂存页复用

原生 VT-d 单卡发送观察到大量 4 KiB DMA 映射生命周期。owner 现在最多保留
64 个空闲 4 KiB 暂存页（每 container 256 KiB），复用前清零。
硬件 IOMMU 映射/解除映射、buffer publication、queue invalidation 和 registration
生成号仍按原生命周期执行；较大映射与部分解除映射继续使用原 mmap 路径。
设置环境变量 `LOOM_VFIO_POOL_RECYCLE=0` 可以关闭复用进行同版本对照。
在正常解除映射中，仅在 backend 成功解除 IOMMU 映射、publication 成功且
buffer/queue 状态失效后回收页。失败 publication 不回收页；容器退出释放空闲页。

实际源码的内存中 ASan/UBSan 验证通过：零初始化、generation ABA、失败同步重试、
缓存容量与释放、部分解除映射、immutable completion、RX descriptor cache。
1024 次串行 4 KiB staging map/unmap 只调用一次 mmap，容器释放时一次 munmap；
这只是生命周期系统调用验证，不能视为硬件吞吐提升。真实 VF 性能对照另行记录。

普通 VM 对照用 Ezreal upstream KVM 6.6.32 和系统 QEMU 8.2.2，guest 内核保持
6.12.0-vanilla-vfio #26，4 vCPU/4 GiB、两 NUMA 节点；巨虚使用 QEMU 9.0.90。
同一张 VF 向 Jinx 发送，单卡双 worker、6 秒三轮中位数：普通 VFIO 463.4 MiB/s，
关闭 BAR mmap 与 KVM MSI/MSI-X 直送 384.5 MiB/s，巨虚先前同规格 98.8 MiB/s。
QEMU 版本和系统镜像实例存在差异，不能把全部差距归因于单一代码路径。
磁盘同一 Ezreal 1 GiB raw、单盘双 worker、每 worker QD4、1 MiB 顺序 I/O、
8 秒三轮：普通 VM 读 2048.0、写含 fsync 1001.6 MiB/s；当前巨虚读 1768.6、
写含 fsync 986.5 MiB/s，写后直接读回检查通过。小范围循环 I/O 不代表大数据集稳态性能。
用户已要求磁盘保留当前结果、不继续重测；TCP 重传排查暂时搁置，HPET 与 guest 代码不变。

真实 VF 初步验证（代码版本 `0548ec98d5`）：同版本开关对照，Ezreal 单卡向 Jinx，
2 worker、6 秒三轮，无性能探针。关闭复用 76.34/90.37/97.42 MiB/s，
开启复用 95.98/107.04/104.63 MiB/s；中位数 90.37 → 104.63 MiB/s。
这是单次先关后开的对照，存在启动阶段和轮间波动，不能宣称稳定提升 16%，
也不能据此解释普通 VM 与巨虚的全部差距。发送接收字节数和内容校验通过。
另一次 12 秒系统调用观察窗口，192547 次 VFIO MAP ioctl 与 192611 次 UNMAP ioctl
仍然执行，而 4 KiB mmap/munmap 分别只有 9833/9897 次；说明保留硬件映射生命周期，
多数暂存页复用成功。该窗口带 syscall 探针，不作为正式吞吐对照。
Jinx 同时向两张 guest VF 各发送 4 MiB，内容校验通过，driver RX discard 增量为 0；
这只是短收包回归，不代表长期 TCP 重传/丢包问题已解决。两端 watchdog/GET_STATS
各 5 次完成，未发现所检查的 BUG、DMA fault、TX timeout 类错误，HPET 仍为 hpet。
既有 FAT 未正常卸载和偶发 printk 时钟下溢未在本次工作中修复。

## 2026-10-10：单 VF 发送的小内存访问来源

继续保留 guest #26、HPET 和当前磁盘配置，Ezreal 单 VF、2 worker 向第三方
Jinx 发送，发送/接收字节和内容检查通过。以下是诊断计数，不是吞吐基准：

- 12 秒探针窗口内包含一次约 6 秒发送。8 字节 DSM memcpy 读请求 482324 次；
  `vtd_get_slpte` 入口 481158 次。该函数每次恰好读取一个 8 字节页表项，
  两者数量对应约 99.76%，说明这一窗口的 8 字节读主要来自 VT-d 页表遍历。
- `vtd_dev_to_context_entry` 和 `vtd_fetch_inv_desc` 各 71026 次；
  16 字节 DSM memcpy 读请求 284419 次。前者在当前 legacy 模式读 root/context，
  后者处理 queued invalidation；调用栈确认二者产生小读，但入口次数不能直接
  换算每个函数内部的完整读请求数量。
- 暂存 DMA map/unmap 入口各 155961 次；TX submission 9065 次，
  data completion 25797 次。暂存页复用减少 mmap，不消除硬件 IOMMU 生命周期。
- 更早的独立 24 秒 syscall enter/exit 配对窗口共 4529485 次 memcpy ioctl，
  3346871 次长度不超过 32 字节；只有 347 次 ioctl 内部观察到远端 fetch 请求。
  “没有 fetch”不表示没有 DSM 成本：内核复制路径仍检查页状态、获取全局与页面锁。

DWARF 调用栈记录明确出现 IOTLB invalidate → shadow page walk → slpte read，
以及 VFIO TX payload 同步、初始 DMA 注册和 completion changed-byte publication。
记录使用约 64 MiB 的上限，数据只放 memfd，结束即释放；该短窗口丢样 26.60%，
因此不使用其调用栈百分比估计总开销或性能收益。更早的无上限采样过大，已停止并释放，
同样不用于性能归因。函数入口计数与 syscall 计数另行完成，无返回探针配对依赖。

当前证据把排查重点移到频繁 DMA 生命周期引发的 VT-d 元数据访问，以及本地复制
ioctl 的固定成本。它尚未证明这些成本足以解释普通 VM 与巨虚的全部吞吐差距。
后续需分别检查缓存的失效语义、可合并的访问、锁等待和 owner 本地执行路径；
不能绕过 DSM 直接读取 guest RAM，也不能删除实际硬件 DMA 解除映射。
诊断接收端和所有探针已退出，Jinx 临时地址已删除。guest/host 内核代码未修改。

## owner 页级通知复用 context 缓存（guest 不变）

`vtd_iotlb_page_invalidate_notify` 在 owner-local legacy VT-d 模式下复用
现有 DMA translation 的正 context 缓存。缓存仍使用同一个 generation 和
IOMMU 锁；全局/设备 context invalidation、root 替换、翻译启停与 reset
沿用已有失效路径。读取失败不缓存，scalable/PASID 路径保留原读取方式。
设置 QEMU 环境变量 `LOOM_VTD_NOTIFY_CONTEXT_CACHE=0` 关闭本优化。
普通 VM 不启用 owner 模式时不受影响；不缓存页表项、不改变 DMA 映射生命周期。
依据 [Intel VT-d 规范](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf)
第 6.5 节，context-cache 与 IOTLB 使用各自的失效命令；修改 root/context
所需的 context 失效仍必须执行，不能用这一缓存绕过失效要求。

实际源码提取后在 memfd 中编译运行 ASan/UBSan 验证：重复命中、正缓存与已有
translation 缓存共享、全局及设备失效、function mask 0/1/2/3、跨 bus 隔离、
root 替换、翻译启停、reset、generation 回绕、失败/非 present 重试，
以及关闭开关和 scalable fallback。测试通过且两端源码构建另行核验。
这些测试不是硬件性能证据；真实 VF 的开关对照与健康检查在部署后记录。

同一次 guest 启动中可通过 owner 的 QMP `qom-set` 修改 `intel-iommu` 对象的
`loom-notify-context-cache` 布尔属性；`qom-get` 读取实际状态。该属性只在设备
完成 realize 后注册，使用已有 IOMMU 锁；切换不清除 context 缓存或改变硬件
DMA 生命周期，既有失效机制持续生效。开启要求 owner IOMMU 模式。

### context 缓存真实 VF 验证

代码 `f06ebe6810` 两端构建、运行。同规格 Ezreal 单 VF→Jinx、2 worker、
6 秒三轮、无性能探针，首次分开启动对照：关闭 163.67/104.78/170.70 MiB/s，
开启 101.02/138.43/122.62 MiB/s。启动状态与轮间波动明显，这一对照
不能证明优化收益或回退；没有用它替代普通 VM 对照。

独立 12 秒诊断窗口各包含约 6 秒发送，关闭：QI fetch 76808 次、
fresh context 76808 次、16 字节 memcpy 读 307618 次；开启：
QI fetch 69946 次、fresh context 0 次、16 字节读 140257 次。
按 QI fetch 归一化，16 字节读约 4.005→2.005 次，重复 root/context 读取消除，
同时 DMA map/unmap 仍各执行 167726 / 152311 次（对应关闭 / 开启）。
诊断窗口负载与吞吐不同，计数不作为性能样本。

随后代码 `93412ed86f` 加入运行时 QMP 开关，两端正常 guest poweroff 后
重新启动同一个 guest #26。保持 4 vCPU/4 GiB、两 NUMA 节点、owner-local
CPU 2/3 两个发送 worker、两队列 RX512/TX128、XPS/RPS=0、HPET，
同一 VF 和 Jinx 接收端，单次启动内交替 OFF/ON、ON/OFF、OFF/ON：
关闭 130.18/86.81/104.15，开启 93.99/115.04/116.12 MiB/s，
中位数 104.15→115.04 MiB/s（观察约 +10.45%）。
每轮无性能探针，两端 QMP 读回实际开关，接收字节数与内容校验通过。
结果仍重叠且只有三对短测试，不能宣称稳定提升；与普通 VM 的主要差距仍未解释。

开启状态下，两张 VF 从第三方 Jinx 各接收 4 MiB，内容校验通过，
RX discard 增量均为 0。队列从默认 4 调为 2、ring 重配置后 ping 正常；
随后 10 秒两端 watchdog/GET_STATS 各 5 次完成，所检查的 BUG、DMA fault、
TX timeout 类错误未出现，HPET 为 hpet。短测试不能证明长期无重传/丢包。
当前保留缓存开启；各探针、发送/接收辅助进程和 Jinx 临时地址已清理。
host 内核及 guest 代码不变，磁盘不重测，Garen 既有 libvirt VM 未停止。
下一步继续区分剩余 DMA 生命周期、队列处理与锁等待成本。

## 远端 guest CPU offline 隔离对照（2026-10-10）

按用户建议，在同一次巨虚启动内比较全部 CPU 在线、远端 CPU offline、
恢复在线三种状态。未改 guest/host/QEMU 源码，未重启 host；QEMU 运行代码
`93412ed86f`，context 通知缓存开启，guest #26、HPET、4 GiB、两 NUMA 节点
保持不变。测试 Garen owner0 的单 VF 和 1 GiB raw 单盘，两个 worker 始终
固定 CPU 0/1；offline 的是 Ezreal owner1 的 guest CPU 2/3。

| 项目（MiB/s，中位数） | 全部在线 A | CPU 2/3 offline B | 恢复在线 A2 |
| --- | ---: | ---: | ---: |
| 单 VF → 第三方 Jinx | 144.74 | 178.52 | 155.87 |
| 单盘读 | 1750.68 | 1925.77 | 1898.16 |
| 单盘写（含 fsync） | 974.17 | 971.43 | 956.73 |

NIC 每状态三轮、每轮 6 秒，两个发送 worker，接收端字节与内容校验通过。
三轮分别为 A 144.74/143.62/155.88，B 200.13/169.55/178.52，
A2 155.87/158.14/144.58。offline 中位数比 A 高 23.34%、比 A2 高 14.53%，
是减少远端 CPU 活动值得继续调查的信号，三轮短窗口尚不能证明稳定因果关系。
当前 Garen NIC 数值不能当作与之前 Ezreal 普通 VM 的严格同 host 对照，
单卡与普通 VM 的主要差距仍未解决。

磁盘使用原生 Linux AIO、O_DIRECT、1 MiB、每 worker QD4，三轮各 8 秒，
另有 2 秒预热；写包含 fsync，完成后 direct readback 首/中/尾位置校验通过。
读三轮 A 1804.59/1705.75/1750.68，B 1930.55/1764.56/1925.77，
A2 1898.16/1699.30/1899.72；写三轮 A 928.16/976.63/974.17，
B 994.00/971.43/949.91，A2 942.01/956.73/957.04。
恢复后读带宽也接近 offline 状态，B 比 A2 仅高 1.45%，不能将读提升
直接归因于 offline；写没有显著改善。循环读取小 raw 后端的结果不能代表
大数据集持续介质吞吐。

CPU offline 会将远端 NIC 的有效 IRQ 目标移到 CPU 0/1。恢复在线后，
请求亲和性仍为 2–3，但有效目标曾仍在 0/1；Linux IRQ 代码允许延迟迁移，
重新写 mask 不能保证立即完成，实际中断到达后才迁移。因此先在 CPU 2/3
各发送三次远端 NIC ping，确认远端 NIC IRQ 40/41/42 有效目标分别为 2/3/2，
本地 NIC IRQ 37/38/39 为 0/1/0，再进行表中的 A2 回测。
恢复确认前另一次 NIC 回测未用于表中的 A2。磁盘使用 managed MSI-X，
未强改其 mask；CPU 0/1 对应 worker 的本地队列 IRQ 保持本地。

最终 guest CPU 0–3 全部在线，owner-local NIC 请求与有效亲和性已核实。
10 秒健康窗口两端 watchdog 和 GET_STATS 各完成 5 次，所检查 BUG、
DMA fault、TX timeout 类错误为空，HPET 为 hpet。所有临时探针、
Jinx 接收进程及 198.18.10.3 地址已清理；Garen 原有 libvirt VM 保持运行。
本次没有计数 DSM page fault，不能由吞吐变化直接认定 DSM 为原因。
下一步优先区分远端 CPU 后台活动引起的页面迁移/共享竞争，与剩余本地
VT-d、VFIO DMA map/unmap 和队列处理固定成本。

## owner 本地 ioctl 与用户态成本拆分（2026-10-10）

同一次四 CPU 在线的 Garen owner0 单 VF→Jinx 运行中，guest、HPET、
QEMU 开关与 CPU 0/1 两 worker 配置保持不变，分别执行独立诊断窗口。
代码仍为 `93412ed86f`，本轮未改运行代码或 guest。

24 秒 BPF 窗口配对 raw ioctl enter/exit，同时按 active 调用记录
sched_switch 与调用内 kvm_dsm_fetch；停止 enter 后等待在途调用退出，
最终 active_count=0，所有观察到的调用返回错误数为 0。
按 ioctl 类别累计（跨线程，不能当作墙钟占比）：

| 类别 | 调用数 | 累计墙钟 ms | 其中 off-CPU ms |
| --- | ---: | ---: | ---: |
| DSM memcpy 读 | 4190973 | 2388.152 | 0.562 |
| DSM memcpy 写 | 1119551 | 558.626 | 0.240 |
| VFIO DMA map | 674591 | 1178.786 | 0.051 |
| VFIO DMA unmap | 674591 | 1934.292 | 0.205 |

硬件 map/unmap 都为 4 KiB，平均约 1.75 / 2.87 微秒，主要在本地
qemu-vcpu0/1 线程内执行。复制调用内仅观察到 72 次 fetch；这不表示 DSM
没有开销，复制 ioctl 即使不 fetch 仍执行 DSM 锁、页面检查与复制路径。
没有把该数字解释为所有 DSM fault 或全系统 RDMA 请求数。
带探针发送三轮 134.14/127.07/123.60 MiB/s，内容与字节数校验通过；
探针对逐 syscall 路径有扰动，不使用该吞吐证明优化收益或回退。

独立低频 cpu-clock:uH、199 Hz、24 秒、DWARF4096 用户态采样，
记录 17.366 MB 于 memfd，处理后释放，报告 Lost Samples=0。
自耗时样本主要包括 ioctl 8.34%、hash lookup 5.16%、malloc 3.49%、
iova_tree_compare 3.26%、pthread_mutex_lock 2.62%、动态类型检查 2.14%、
completion 1.91%、calloc 1.75%；其余热点较分散。
这些比例仅覆盖采样到的 host 用户态 CPU，不包含内核执行、guest 或等待，
不能与上一窗口的累计 ioctl 时间直接相加，也不能据此宣称锁竞争严重。
该窗口发送 140.53/131.79/147.76 MiB/s，字节及内容校验通过。

本轮证据确认需要减少的是每次 DMA 生命周期及小块元数据访问的固定成本，
不只是跨节点 fetch；剩余性能差距尚未完全归因。后续优先检查 4 KiB
map/buffer 元数据的分配与索引成本，以及已发布失效描述符的读取批量化。
任何优化都必须保留实际硬件 unmap、翻译失效、completion 发布顺序与
DSM 访问语义，不能以继续保留失效映射换取吞吐。
临时探针/采样与接收进程均已结束、memfd 已释放、Jinx 临时地址已删除；
最终 CPU 0–3 在线、hpet，所检查 BUG/DMA fault/TX timeout 类错误为空。

## 4 KiB DMA buffer 分配块复用

owner 本地原生 4 KiB buffer 在完成写回、队列失效并从 live index/list
移除后，最多缓存 64 个分配块（每块 metadata + 8 KiB baseline/scratch）。
只复用存储：下一次注册重置完整 metadata，重新通过 DSM 读取 baseline，
重新设置 pool、注册顺序和 generation；实际硬件 map/unmap 保持原路径。
部分/通用 unmap、切片、错误清理保留原释放方式，容器销毁释放所有缓存块。
设置 `LOOM_VFIO_BUFFER_RECYCLE=0` 关闭，独立于 staging page 的复用开关。

实际源码提取的 ASan/UBSan 测试通过：10000 次循环只分配一次、元数据清零、
64 块上限与溢出释放、关闭回退、容器隔离、错误释放后的再次分配、切片
baseline/pointer 生命周期、最终分配/释放平衡。另行核查缓存只进入已完成
失效且移除索引的原生页级成功 unmap 路径。构建和真实 VF 部署结果另行记录，
此处不宣称吞吐收益。

### buffer 复用部署验证

代码 `3f18950b0c` 两端构建通过。原 guest 正常 poweroff，确认两端 QEMU
退出 0 后重新启动；host 未重启，Garen 既有 libvirt VM 未停止。
guest 仍为同一个 #26，CPU 0–3 在线、HPET、4 GiB/双 NUMA；
两张真实 VF 识别，恢复两队列 RX512/TX128、XPS/RPS=0、owner-local IRQ，
两端 ping 均为 3/3。ring 重配置期间 ethtool 曾读到 transient link-down，
随后 link-up 且实际 ping 成功。两块数据盘在两端 PCI 目录中注册；
本轮没有扩大磁盘性能测试范围。

两端实际进程环境确认 buffer 复用开启。Garen owner0 24 秒单卡发送
诊断窗口，依据当前二进制 DWARF 得到 recycle_buffers/free_buffer_count
偏移 0x258/0x25c，在实际 buffer_new 入口读取参数与缓存状态：
复用 586224 次、原生新分配 115861 次、非页及切片均为 0。
即约 83.50% 原生 buffer_new 走复用分支；不是所有 malloc 的减少比例。
此探针窗口三轮 131.09/130.75/130.96 MiB/s，不作性能收益证据。

探针退出后同配置无探针三轮单 VF→Jinx，2 worker、6 秒，
133.93/127.22/168.58 MiB/s，中位数 133.93；全部字节及内容校验通过。
与前一启动的在线测试不同，本轮没有同一次启动开关交替对照，
数值仍明显波动，不能宣称吞吐提升；单卡主要差距尚未解决。
本优化确认减少了已观察到的本地分配成本，还剩大量新分配、索引、
VT-d 小块元数据访问以及必须保留的硬件映射操作。

最终 10 秒两端 watchdog/GET_STATS 各完成 5 次，所检查 BUG、DMA fault、
TX timeout 类错误为空，HPET 为 hpet。短窗口不能证明长期无丢包/重传。
所有探针/辅助接收端与 Jinx 临时地址已清理，源码已同步两端 origin 分支。

## owner QI 成对读取（默认关闭，待真实 VF 开关对照）

当前 Garen 运行代码 `3f18950b0c` 的 24 秒入口计数窗口观察到 125450 次
QI fetch，head/tail 距离均为 2。首个 BPF 程序因有符号取模编译失败，
修正后才附加；该窗口只覆盖部分发送过程，不用于估计完整流量或吞吐收益。
发送三轮 147.93/150.77/139.54 MiB/s，字节及内容校验通过。
没有将带探针数字作为优化性能结果。

依据 [Intel VT-d Rev.5.0](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf)
6.5.2（PDF 第 112 页），硬件允许一起获取多个已提交描述符，但必须逐条
验证后推进 head；6.5.2.11/12（第 128/129 页）规定错误 head 与完成顺序。
实现只将两个已发布描述符合并为一次内存读取，原失效 handler、WAIT、
逐条 IQH 写回与错误停止位置不变。快照是 fetch 调用的栈局部值，
不跨 doorbell、reset 或迁移保留，不缓存 PTE 或 hardware DMA mappings。

仅 owner legacy 模式可启用，且不得跨环回绕或 4 KiB 页；单条、scalable、
非 RAM/不足连续长度、地址溢出及批量读取失败均回退原单条读取。
设置 `LOOM_VTD_QI_PAIR_READ=1` 启用；默认关闭。
设备 realize 后可用 QMP `loom-qi-pair-read` bool 属性做同启动切换，
普通非 owner 模式拒绝开启。QI MMIO 和 QMP 配置都由 BQL 串行执行，
不会在同一次 fetch 中途切换。

实际源码提取的 ASan/UBSan 测试通过：128/256-bit 解码、顺序 handler、
每条 head 更新、环回绕、跨页回退、第一/第二条非法描述符停止位置、
批量读失败回退、后续单条读失败、关闭/scalable/MMIO 回退和非法 tail。
这些测试使用隔离的内存/handler stub，不能替代真实 WAIT/DMA 硬件验证。
Ezreal 源码构建通过；两端当前运行仍是旧版本，未启用这项优化。
下一步部署并核验两端 QMP 状态，做同启动开关交替的无探针性能对照，
另测元数据读取计数及队列重配置/管理队列健康。

### QI 成对读取同启动真实 VF 对照

代码 `6af87e7c27` 两端编译部署。旧 guest 正常 poweroff，两端 QEMU
退出 0 后重新启动相同 guest #26；host 内核、guest 代码及 HPET 不变，
Garen 既有 libvirt VM 保持运行。四 CPU 在线、4 GiB/双 NUMA，
两张 VF 配置两队列 RX512/TX128、XPS/RPS=0 与 owner-local IRQ。
两端 QMP 读回成对读取初始关闭，再分别设置并读回每个测试状态。

同一次启动 Garen 单 VF→第三方 Jinx，两个 worker 固定 CPU 0/1，
每样本 6 秒，无性能探针；按 OFF/ON、ON/OFF、OFF/ON 交替：
关闭 136.13/162.35/152.26 MiB/s，开启 133.15/145.88/152.24 MiB/s，
中位数 152.26/145.88 MiB/s。观察开启约低 4.19%，仅三对短窗口，
尚不足以断言稳定回退，但没有证明吞吐收益，不能宣称单卡问题已解决。
所有接收字节及内容校验通过。

另行独立 12 秒计数窗口各包含约 6 秒发送：
关闭 QI fetch 69778 次、16 字节读 139900 次、32 字节读 585 次；
开启 QI fetch 66396 次、16 字节读 289 次、32 字节读 67052 次。
按 QI fetch 归一化的 16/32 字节读约 2.013→1.014 次，
确认描述符读取调用约减半。context fresh read 两状态均为 0。
8 字节读仍为 480874/442865 次，其中 slpte 479635/441801 次；
硬件 map/unmap 两状态仍各 155091/144891 次。
计数窗口负载和调用数不同，不能用绝对计数下降证明性能收益；
没有省略实际硬件失效或映射生命周期。

开启状态下，两 VF 队列分别 2→1→2，RX ring 512→256→512，
配置恢复后两张网卡 ping 各 3/3。随后 10 秒 watchdog/GET_STATS
各 5 次完成，所检查 BUG、DMA fault、TX timeout、QI 错误为空，
HPET 为 hpet；队列重配会重置设备统计，最终零 discard 不能证明
整个发送过程没有丢包。TCP 重传不在本轮修复范围。
最后两端 QMP 将成对读取恢复关闭并读回确认；保留 context 缓存和
buffer/staging 分配复用。临时探针、接收进程和 Jinx 地址均已清理。
下一步继续查剩余 VT-d 页表元数据、队列同步和锁路径的关键成本，
不因读取次数减少而将该试验默认开启。

## host 执行位置与 RDMA 接收线程成本（2026-10-10）

运行代码仍为 `6af87e7c27`，QI 成对读取关闭。24 秒 host-only CPU
采样与 /proc 线程监测期间，Garen 单 VF→Jinx 三轮为
141.01/140.39/136.13 MiB/s，内容/字节校验通过；采样窗口不作优化性能对照。

线程 stat 扣除 guest_time 后，所监测线程累计 host 用户态 7.39 秒、
内核态 19.78 秒，guest 执行另计约 8.01 秒，跨线程不能视为墙钟占比。
本地 vCPU0/1 的 host CPU 分别 11.91/7.79 秒；主线程 1.46 秒，
RDMA 接收辅助线程 4.96 秒。主要活跃线程 runqueue 等待约 0.75/11.50/
3.69/0.66/7.03 ms，未发现秒级调度队列等待。wchan 是 20 Hz 近似快照，
只能显示被采到的睡眠位置，不能据此计算完整等待时间。
host-only cpu-clock:H 199 Hz/24 秒记录 35.382 MB memfd，Lost Samples=0；
内核锁、syscall、DSM memcpy 等热点分散，未把锁函数采样等同于严重锁竞争。

Garen/Ezreal 均为 192 个 host CPU、两物理 NUMA 节点：
node0 为 0–47,96–143，node1 为 48–95,144–191。
X710 0c:00.0 位于 node0，Garen DSM RDMA HCA ac:00.0 位于 node1。
初始 QEMU 各线程 allowed mask 为 0–191，本地 vCPU 曾运行在 node1，
因此 guest 的逻辑 owner-local 亲和性不保证 host 的物理设备局部性。

同一次启动、无性能探针、每状态三个 6 秒样本，Garen 单卡两个 worker，
guest CPU 0/1、guest IRQ、HPET、所有 QMP 开关不变：
A 原 host mask 142.26/120.75/144.36，中位数 142.26；
B vCPU0/1 固定 host CPU20/21、辅助线程22–27（node0），
167.19/165.03/153.04，中位数 165.03；
A2 恢复原 mask 157.58/144.23/156.47，中位数 156.47；
C vCPU0/1 固定68/69、辅助70–75（node1），
148.41/158.33/161.78，中位数 158.33。
所有接收字节及内容校验通过。B 比 A/A2 观察约高 16.01%/5.47%，
样本重叠、顺序与时间波动仍存在，不能宣称稳定 NUMA 因果收益。
没有迁移已有内存或改 memory policy；结束时 numa_maps resident page
统计 node1 1071560、node0 9700（4 KiB），设备与内存局部性存在权衡。
全部 host 线程亲和性最终恢复 0–191，Garen libvirt VM 未改。

进一步对 TID3415662 独立 12 秒、199 Hz host CPU/DWARF 采样，249 个样本、
Lost Samples=0、memfd 1.160 MB，调用栈明确为 recv_thread_func。
该线程运行样本约 75.10% 在 usleep 后的 nanosleep/定时器/调度调用链，
并非说它睡眠占墙钟 75%。源码 migration/rdma-gvm.c 在空 CQ 时 usleep(10)，
目前 ibv_create_cq 没有 completion channel。先前 24 秒该线程发生
1848245 次 voluntary context switch。证据指向频繁空轮询的本地固定成本；
尚未证明它造成主要单卡差距，也未用缺失 DSM fetch 认定无 DSM 成本。

下一步检查事件驱动 CQ：arm 后再 drain 以免丢唤醒，区分发送/接收完成，
保证 stop 可唤醒并先 join 后释放 CQ，提供旧轮询回退。先验证消息/停止语义，
再比较 idle/active CPU 与同启动吞吐；不改 guest 或 HPET。
最终 CPU 0–3 在线、hpet，两端 QMP pair=false，所检查 BUG/DMA/QI/TX timeout
错误为空；采样进程、memfd、Jinx 接收端/临时地址已清理，host 未重启。

## RDMA 请求接收 CQ 事件等待（默认关闭）

`LOOM_RDMA_RECV_CQ_EVENTS=1` 仅为专用请求接收连接创建非阻塞 completion
channel。同步响应 CQ 与发送完成 CQ 保留原来的单消费者轮询语义，
消息协议、request FIFO、control handler、guest 内核和 HPET 均不变。
默认未设置或为 0 时保持旧接收轮询路径；启动日志输出实际 events/poll 模式，
创建 channel/带 channel 的 CQ 失败时回退无 channel 的 CQ。

事件接收 helper 先 arm 通知，再 poll CQ；有完成项立即返回原处理路径，
没有完成项才 poll channel fd 和共享 stop eventfd（100 ms 上限）。
收到事件先 ack 后再次 arm/drain，允许没有 CQE 的额外事件；
EINTR/EAGAIN 重试，通知或事件等待/读取异常回退 usleep(10) 的旧轮询。
依据 [rdma-core 的 ibv_get_cq_event 文档](https://github.com/linux-rdma/rdma-core/blob/master/libibverbs/man/ibv_get_cq_event.3)，
成功取得的 CQ event 都须 ack，重 arm 后可能出现无对应 CQE 的额外通知。

running 标志改为原子访问；cleanup 先清除 running 并写 stop eventfd，
该 fd 不由单个 receiver 消费，保证所有接收线程可见。
join 所有接收及 worker 线程后，停止 event-mode receive QP、取得并 ack
剩余事件，再销毁该 CQ/channel，最后关闭 stop fd。其他 legacy 连接
仍沿用已有资源生命周期，本修改不宣称完成原有全部连接资源回收。

实际 helper 源码提取到 memfd，ASan/UBSan 隔离测试通过：
arm-before-drain、完成在 arm 后到达、额外空事件、EAGAIN/EINTR、
stop/超时、notify/poll/get 错误回退、错误 CQ 的 ack 与旧轮询分支。
另用实际 poll/pipe/eventfd/pthread 和三个模拟接收者测试停止：
一次 stop 写入唤醒全部三个线程，join 后 eventfd 仍保留该计数，
再关闭资源，ASan/UBSan 通过。verbs/CQE 使用 stub，不是 RDMA 硬件测试。
另核查源码清理次序为 wake→join→QP→event ack→CQ/channel→stop fd。
Ezreal 构建通过；两端运行中的 QEMU 仍为旧代码 `6af87e7c27`，
没有启用新的事件路径。下一步完成两端构建与部署，核实实际模式、
真实消息和 shutdown、idle/active CPU，再测单卡吞吐，不能提前宣称收益。

## RDMA CQ 事件模式真实验证（2026-10-10）

两端源码和运行版本均为 `e423ada678`，通过 sudo env 显式设置
`LOOM_RDMA_RECV_CQ_EVENTS=1`。两端启动日志确认 receive CQ 为 events；
Garen 接收线程的实际 poll 参数为两个 fd、100 ms 超时，fd 分别指向
infinibandevent 与 eventfd，确认走到事件等待。guest #26、HPET、规格、
context 缓存、buffer reuse 保持不变，QI 成对读取关闭。

24 秒活动窗口 /proc 监测：接收线程 host CPU 0.42 秒、
33767 次 voluntary context switch；旧轮询诊断窗口分别为 4.96 秒、
1848245 次。新窗口有发送测试但没有 perf/BPF；旧窗口还有 perf 采样，
两次启动及负载不同。这些数值说明观测到的接收线程固定成本下降，
不作为严格同启动性能收益百分比。24 秒无测试流量窗口还有 guest
后台消息，接收线程 CPU 0.18 秒、12033 次 voluntary context switch，
poll wchan 占 468/473 个快照。

无监测单 VF→第三方 Jinx、两个 CPU0/1 worker、每轮6秒、三轮：
155.19/152.30/131.00 MiB/s，中位数152.30，接收字节和内容校验通过。
单卡主要差距仍在；此版本没有证明稳定吞吐提升。TCP 重传修复仍暂停。
10 秒健康窗口两端 watchdog/GET_STATS 分别5/4次，
检查的 BUG、DMA/QI 错误及 TX timeout 为空，HPET 为 hpet。

随后实际正常 guest poweroff：两端 QEMU 均 EXIT0；针对真实进程的
rdma_router_cleanup 入口/返回探针在每端各计数一次，证实 cleanup 返回，
没有卡在 join 或 CQ 清理。该证据不代表旧连接所有资源回收均已完善。
测试接收进程、探针、memfd 和 Jinx 临时地址已清理，之后重新启动事件模式。
公平的同启动 poll/events 吞吐对照尚未实现；当前开关只在初始化读取，
不能把两次启动的数值当成严格 A/B。

## 事件模式下远端 CPU offline 单卡/单盘对照（2026-10-10）

按用户要求，在上述 CQ events 模式同一次启动内，A 全部在线→B guest CPU2/3
offline→A2 恢复全部在线。运行代码 `e423ada678`，未改 guest/host 内核、
QEMU 代码、HPET、4 GiB 两 NUMA 节点及 host 线程 mask。仅改变 CPU online
配置；两个发送/读取 worker 固定 guest CPU0/1，设备均为 Garen owner0。

| 项目（MiB/s，中位数） | A CPU0–3 | B CPU0–1 | A2 CPU0–3 |
| --- | ---: | ---: | ---: |
| 单 VF→第三方 Jinx | 135.17 | 188.33 | 153.56 |
| 1 GiB raw 单盘读 | 1807.97 | 1877.53 | 1788.16 |

网卡每状态三轮各6秒，所有接收字节/内容校验通过：
A 134.48/140.53/135.17，B 173.70/188.33/205.86，
A2 152.28/153.56/170.93。B 比 A 高39.33%，比A2高22.64%。
这是远端 CPU 活动有额外成本的信号；短窗口与顺序波动仍存在，
尚不能区分 DSM、远端中断与其他共享路径，也没有消除主要单卡差距。
CPU offline 不移除远端内存，不能据此声称 DSM 已被排除。

单盘只读，使用仓库脚本的 Linux AIO ABI/磁盘 serial、容量校验定义，
在内存中构造两个 worker、O_RDONLY|O_DIRECT、1 MiB、每 worker QD4，
分别循环读取各512 MiB范围；每状态2秒预热、三轮各8秒，不写盘。
缓冲区由对应本地 CPU first-touch。所有 AIO completion 为完整1 MiB、无错误：
A 1816.51/1722.79/1807.97，B 1890.89/1746.32/1877.53，
A2 1788.16/1687.61/1819.47。B 比 A/A2 高3.85%/5.00%，样本重叠，
不能认定稳定读带宽提升；未进行本轮写测试或大数据集持续介质测试。

CPU2/3 已恢复。恢复后重新设 NIC owner-local IRQ，CPU2和3各经远端 NIC
ping3次，均成功；实际有效 IRQ 本地37/38/39为0/0/1，
远端40/41/42为2/3/2，再测A2。测试进程全部结束，Jinx临时198.18.10.3
地址已删除，没有创建实验文件。巨虚保持CPU0–3在线和events模式运行，
Garen已有libvirt VM未改。

最终10秒健康窗口两端 watchdog 和成功 GET_STATS 各5次；所检查错误为空，
HPET为hpet，两张NIC的discard/tx_errors均为0（非整个历史无丢包证明）。

## CQ 接收模式运行时控制（同启动对照）

以 `LOOM_RDMA_RECV_CQ_EVENTS=1` 启动以预创建 channels，之后可通过 QMP：
`qom-set path=/machine property=loom-rdma-recv-cq-events value=false/true`。
false 恢复 usleep(10) 接收轮询，true 恢复事件等待。getter 同时检查请求策略
与所有接收连接的健康状态。没有预创建 channel、连接缺失或 channel 已回退
时，启用操作报错；运行中不会分配/销毁连接，也不会复活失败 channel。

策略及 receiver fallback 标志使用原子访问。切换不更改停止 eventfd；
阻塞 receiver 最多在100 ms现有 poll超时后观察新策略。暂停事件等待时保留
receiver 独占的 armed 状态，待恢复或 cleanup 取得并 ack 已排队通知。
接口只在多 QEMU RDMA router 初始化成功后注册到 machine。
ASan/UBSan helper/控制 API 测试覆盖 armed 状态下关闭、恢复待处理通知、
等待期间切换、缺少/失败 channel 拒绝启用，以及已有错误/停止分支。
真实 poll/eventfd 三线程停止测试继续通过。硬件同启动 A/B 尚待部署验证。

## CQ 运行时开关的真实同启动验证（2026-10-10）

两端运行 `821986ba72`，启动时创建 events channel，随后两端 QMP
`/machine/loom-rdma-recv-cq-events` 反复 false/true 并读回确认。
同一个 Garen接收TID3466728在false时实际syscall为clock_nanosleep，
true时为poll两个fd、100ms；通过/proc/mem读取pollfd，确认channel
infinibandevent与stop eventfd。guest#26、CPU0–3在线、HPET、4GiB、
IRQ/队列/host mask保持一致，QI pair关闭。

无测试流量但有guest后台消息，各12秒/proc监测：
poll接收线程host CPU2.02秒、967311次voluntary switch；
events为0.15秒、10346次，run_ns分别2022473856/147030295。
事件模式减少了观测到的接收线程固定CPU/调度成本；它不是DSM成本归零证明。
该监测与吞吐窗口分开，没有perf/BPF采样干扰吞吐比较。

无监测、Garen单VF→第三方Jinx，2个CPU0/1 worker，每样本6秒，
交替顺序poll/events、events/poll、poll/events：
poll 132.69/135.77/129.77，中位数132.69 MiB/s；
events 129.84/138.48/126.96，中位数129.84 MiB/s。
所有字节/内容校验通过；样本重叠，events观察值低2.15%，
没有证据说明它提高单卡吞吐，不能将CPU下降外推为带宽收益。
另一个发送窗口中，两端events→poll→events切换成功，
接收字节/内容校验通过，约138.47 MiB/s仅作为功能测试、未纳入性能对照。

最终两端events=true，10秒watchdog/成功GET_STATS各5次，
所检查BUG/DMA/QI/TX timeout错误为空，HPET=hpet，
discard/tx_errors=0（非整个历史无丢包证明）。
接收进程、/proc监测、临时trace探针和Jinx地址已清理，未创建实验文件。
guest仍运行，Garen既有libvirt VM未改，host未重启，guest代码未改。
本版本真实开关/消息已经验证；新版本正常shutdown尚未另测，
此前event版本正常cleanup返回与当前实际poll/eventfd三线程stop测试
提供相关证据，不把它们当作新版本完整shutdown硬件证明。

## VFIO 中断、完成与提交路径成本定位（2026-10-10）

运行代码仍为 `821986ba72`，CPU0–3在线、guest#26、HPET、两队列/
RX512 TX128不变，CQ events开启。Garen单VF→第三方Jinx两个CPU0/1 worker。
两个独立24秒窗口针对实际Garen进程3466448做PID过滤BCC uprobe/uretprobe；
每个函数记录入口到返回的墙钟时间及调用数。窗口含三轮6秒发送与后台控制消息。
这些是受探针影响的诊断数据，累计时间有嵌套，不是CPU用时或墙钟占比，
不能相加，两个窗口也不能当成配置性能对照。

第一窗口：
| 函数 | 调用数 | 累计秒 | 平均µs |
| --- | ---: | ---: | ---: |
| vfio_msi_interrupt | 36992 | 0.8170 | 22.08 |
| loom_iavf_sync_data_completion | 122762 | 1.5604 | 12.71 |
| loom_vfio_dma_sync | 235 | 0.0814 | 346.57 |
| loom_iavf_reconcile_completion | 122960 | 0.2800 | 2.28 |
| msix_notify | 36992 | 0.1244 | 3.36 |

第二窗口进一步覆盖BAR和提交路径：
| 函数 | 调用数 | 累计秒 | 平均µs |
| --- | ---: | ---: | ---: |
| vfio_msi_interrupt | 75028 | 1.1134 | 14.84 |
| vfio_region_write | 125922 | 2.3798 | 18.90 |
| loom_iavf_submit_tx | 46123 | 0.8100 | 17.56 |
| loom_iavf_submit_rx | 50893 | 0.1892 | 3.72 |
| loom_iavf_sync_data_completion | 200888 | 1.3803 | 6.87 |
| loom_vfio_dma_sync | 235 | 0.0833 | 354.68 |
| msix_notify | 75028 | 0.2140 | 2.85 |

调用入口计数含不适用/空操作分支，不能把submit_rx计数等同有效RX提交。
第一窗口发送155.85/139.34/146.23，第二窗口130.43/156.03/157.39 MiB/s，
接收字节与内容全部通过；不把这些数值作为无探针性能基线。

源码证实 scoped完成同步同时被helpers.c的BAR写前路径与pci.c中断回调调用；
第一窗口次数约为中断回调3.3倍。它在每次扫描所有已启用TX ring、检查RX
completion并保留不可识别状态full同步回退，不能简单删除doorbell前同步：
提交必须合并晚到DMA、保留CPU页重用与payload/descriptor发布顺序。
本地目标MSI已经在loom_pci_forward_msi中绕过远端RPC；
x-no-kvm-msix=on保留用户态完成同步，再调用msix_notify。
直接使用普通VFIO irqfd会绕过该DMA发布步骤，不能用配置强开取代正确性。

证据把下一步重点放在BAR/队列提交中的非DSM固定工作及物理MMIO成本。
vfio_region_write当前完成同步与控制观察后，通过pwrite写物理BAR；
可进一步测量此syscall份额，再判断owner私有的BAR映射是否值得实现。
若采用映射，必须保留guest MMIO陷入与同步，不能直接映射给guest绕过模型。
本轮没有更改运行代码、guest/host内核或HPET，也没有证明全路径开销已消除。

所有探针进程退出并cleanup，接收端和Jinx临时地址已清理。最终10秒两端
watchdog/成功GET_STATS各5次，检查错误为空，HPET=hpet，
discard/tx_errors快照为0；两端巨虚与Garen原libvirt VM保持运行。

## BAR 系统调用与当前 host CPU 调用栈（2026-10-10）

两端仍运行 `821986ba72`，未改guest/host/QEMU运行代码、CPU配置或HPET。
Garen实际VFIO device fd32指向anon_inode:[vfio-device]；
针对PID3466448/fd32的pwrite64系统调用入口/退出做24秒tracepoint监测。
按真实文件offset和4字节写分类（不是推定所有VFIO文件写都是BAR0）：

| offset类别 | 次数 | 累计秒 | 平均µs | 超过10µs | 短写/错误 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0x0–0x3c TX tail | 48322 | 0.043496 | 0.900 | 1 | 0 |
| 0x2000–0x203c RX tail | 5435 | 0.004855 | 0.893 | 0 | 0 |
| 0x3800–0x38fc IRQ control | 67678 | 0.054312 | 0.803 | 2 | 0 |
| 其他低offset | 50 | 0.000118 | 2.370 | 0 | 0 |

实际主要offset为0x3800/0x3804、0/4、0x2000/0x2004，
其他有0x5c00、0x5000、0x7000、0x8400。合计121485次、
约0.102782秒；这是带探针的syscall墙钟累计时间，不是纯CPU时间。
物理BAR pwrite份额较小，暂不为其实现额外私有mmap。
此窗口发送173.45/156.01/168.89 MiB/s，校验通过，不作正式性能基线。

系统perf包装器不能自动找到自编译krdma内核工具，使用现有
/usr/lib/linux-tools/6.8.0-146-generic/perf，无安装或内核修改。
CPU-clock:H排除guest执行，199 Hz、24秒、DWARF stack8192，
记录61,983,992字节memfd，约5K样本、Lost Samples=0。
Self热点分散：_raw_spin_unlock_irqrestore6.60%，kvm_dsm_memcpy2.36%，
perf_iterate_ctx2.12%，syscall_enter_from_user_mode2.06%，ioctl1.79%，
g_hash_table_lookup1.57%。这些是采样CPU分布，不能把锁Self%认定锁竞争，
采样自身有开销，不能据此直接推出路径吞吐上限。

为定位调用来源，再做49 Hz/24秒的独立host-only栈采样：
15,125,228字节memfd，perf script解析994样本，其中115个leaf unknown。
78个_raw_spin_unlock_irqrestore leaf中，37个栈经qi_submit_sync、
qi_flush_iotlb、iommu_flush_iotlb_psi、intel_iommu_tlb_sync到ioctl；
另16个经krdma_poll/send、kvm_dsm_fetch和guest page fault，
5个经DSM invalidate。这里是所列具体栈的采样计数，未知帧和采样不足
限制归因，不表示其完整inclusive成本或所有锁竞争来自这两条链。

ioctl leaf有7个来自vtd_get_slpte/page_walk的DSM metadata读、
5个来自sync_buffer→TX提交，3个来自VFIO DMA unmap、
3个来自DMA register。哈希查找有4个来自loom_vfio_dma_unmap，
4个来自map_at→unmap；mutex_lock样本多处明确进入kvm_dsm_memcpy、
ivy_kvm_dsm_page_fault或dsm_lock。因此没有证据支持盲目改通用QEMU锁。
这些读/复制的系统调用与DSM路径耦合，不能宣称已被从DSM开销中分离。

源码检查：vtd_sync_shadow_page_hook立即逐event通知，
vfio_iommu_map_notify将UNMAP交给vfio_container_dma_unmap，
legacy backend做真实VFIO_IOMMU_UNMAP_DMA ioctl。
下一步先统计每次失效产生的通知数、范围和可合并性，再考虑同一失效内
合并安全的相邻host unmap。必须保留真实硬件unmap/flush、DMA lifetime、
dirty tracking、通知顺序、错误处理和QI完成语义，不能缓存失效映射或跳过flush。

所有发送窗口内容/字节校验通过；没有本轮无探针对照收益结论。
memfd及采样/tracepoint进程全部结束，Jinx接收端和临时地址已清理；
最终10秒两端watchdog/成功GET_STATS各5次，检查错误为空，HPET=hpet，
discard/tx_errors快照为0，Garen已有libvirt VM未改，两端巨虚保持运行。

## Owner IOMMU 同步内 unmap 合并（默认关闭）

新增 `LOOM_VTD_UNMAP_COALESCE=1` 和intel-iommu QMP属性
`loom-unmap-coalesce`。仅owner IOMMU可启用，每次shadow page walk独立
缓冲通知，返回前（含错误返回）全部flush。migration非idle时保持原通知路径。
仅合并type=UNMAP、perm=NONE、translated_addr=0、同一target AS、
连续且等大小buddy范围，保持canonical power-of-two mask与对齐，
每条最大64KiB。MAP、替换映射的非零translation、间隙和未知状态均为屏障。
通知缓存有界32条，满时按顺序flush；不跨walk/QI完成边界，不省略硬件unmap。

为保留现有分配复用，DMA清理扩展native page-index路径到1–16个完整4KiB页：
先检查全部map无大型映射覆盖，按最新注册优先发布全部buffer baseline，
成功后才失效队列缓存、删索引/链表并回收buffer/staging page，
最后推进epoch。失败保持live链表；部分、未对齐、大型映射及超范围
继续通用路径。实际VFIO硬件unmap仍由container-base先执行。

ASan/UBSan memfd测试提取实际通知helper：2000个随机通知流逐4KiB展开，
与原序列比较覆盖、字段及顺序；另覆盖MAP屏障、target/translation/
间隙、越界、默认关闭、64KiB上限和缓冲flush。
实际native清理helper通过1–16页、空洞、范围外相邻映射保留、注册顺序、
发布失败不改链表、large-map/未对齐/超范围回退、尾指针/生命周期测试。
这些使用硬件/同步stub，不取代真实VFIO与性能验证。

实现前PID过滤24秒探针，以当前二进制DWARF确认event/entry offset，
记录259350次shadow walk，577950 MAP和577950 UNMAP，全部UNMAP为4KiB，
实际legacy host unmap也为577950次；87206次walk包含多条unmap，
448275个连续相邻边、256734对相邻等大小且合并后对齐。
walk无错误，探针外hook/physical unmap为0。该统计未额外检查zero translation，
只证明几何合并机会，不是已实现收益。发送113.84/109.26/107.62 MiB/s
且内容/字节通过，强探针开销明显，不作无探针性能基线。
当前尚需两端部署、实测开关与host unmap减少、健康/退出及同启动吞吐。

## Unmap 合并真实验证及硬件 flush 边界（2026-10-10）

两端构建/运行 `2d95e06d8c`，guest#26、4CPU/4GiB、HPET和IRQ配置不变。
同一次启动两端QMP反复关闭/开启并读回确认；CQ events开启、QI pair关闭。

独立12秒强探针窗口，Garen单VF CPU0/1→Jinx，含一次6秒发送：
OFF 200486条guest 4KiB UNMAP→200486次legacy unmap，
实际byte覆盖821190656，恰为200486*4096。
ON 173721条guest 4KiB UNMAP→52991次legacy unmap，
实际byte覆盖711561216，恰为173721*4096；合并后的实际请求size为
4KiB 23363次、8KiB 9299次、16KiB 7718次、32KiB 12611次。
按每条guest unmap归一化，legacy调用由1降到0.305（约减少69.5%）。
两窗口流量不同，不直接比较调用总数；walk无错误、无scope外调用，
发送内容/字节通过。探针影响很大，不用窗口吞吐证明收益。

无探针同启动，交替OFF/ON、ON/OFF、OFF/ON，每样本6秒、两CPU0/1 worker：
OFF 133.00/115.16/121.59，中位数121.59 MiB/s；
ON 155.67/120.00/123.59，中位数123.59 MiB/s。
样本重叠，观察约+1.64%，未证明稳定吞吐收益；所有内容/字节校验通过。

为区分ioctl与硬件失效，另在两个12秒窗口，PID过滤同时计数实际legacy
unmap和内核intel_iommu_tlb_sync：
| 模式 | legacy调用 | unmap覆盖字节/4096 | TLB sync |
| --- | ---: | ---: | ---: |
| OFF | 219952 | 219952 | 219952 |
| ON | 69146 | 235143 | 235143 |

TLB sync entry/return计数一致；受探针影响的累计时长分别0.456853/
0.481387秒，不能作为无探针性能对照。实证：每4KiB仍有一次硬件sync，
虽ioctl约减少70.6%，硬件失效并未合并。
host源码vfio_dma_do_unmap逐vfio_dma对象执行vfio_unmap_unpin；
后者每对象独立iotlb_gather，vfio_sync_unpin先sync再释放该对象的页。
因此QEMU通知合并只是前提，不能宣称已经实现跨DMA对象的flush批处理。

开启状态10秒控制健康通过；随后正常guest poweroff，两端QEMU EXIT0，
没有unmap、CQ join或退出卡住。关机后Garen vfio_iommu_type1 refcnt=0，
既有libvirt VM PID6122仍运行；仅观察模块，未卸载/替换host模块。
两端随后重新启动本版本，补做Ezreal owner1网卡功能验证：
开启合并，worker CPU2/3、enp0s8→第三方Jinx198.18.11.3，
一次6秒约183.20 MiB/s，接收端源IP198.18.11.2，字节/内容校验通过。
此功能脚本沿用了case名Garen，实际interface/CPU/IP确定设备owner1；
单样本不作为Ezreal性能基线。

最终合并恢复OFF（源码默认OFF），CQ events保持ON。CPU0–3在线，
HPET未改，实验接收端、所有probe/memfd和Jinx临时地址均清理。
下一步评估host VFIO同一次请求内共享gather、flush后才unpin的方案；
先完成源代码审查/测试和模块构建，再考虑部署，保留DMA隔离与失败回退。

重启后最终10秒两端watchdog/成功GET_STATS各5次，检查错误为空，
HPET=hpet，discard/tx_errors=0（不代表整个历史无丢包）。两端运行正常。

### Staging page 缓存容量

`LOOM_VFIO_POOL_CACHE_PAGES` 指定每个 VFIO 容器最多保留的空闲原生 4 KiB
staging 页数，默认仍为 64，范围 0..4096。0 不保留空闲页，最大保留 16 MiB。
超出范围或无法解析的值报告错误并使用默认 64；
`LOOM_VFIO_POOL_RECYCLE=0` 仍完全关闭页面复用。
容量只影响解除硬件映射、发布完成并退休后的空闲页面，不缓存硬件映射、
IOVA 注册或注册 generation；页面再次使用前仍清零。
大页、非原生页及一般部分解除映射继续使用原路径。

2026-10-11 对当前巨虚 Garen owner 的计数显示：18 秒内 629698 次单页映射，
247412 次因空闲页耗尽触发 mmap，247295 次单页 munmap，cache miss 约 39%。
未出现映射错误，复用开关开启。perf 采样中的 mmap 开销含 perf 自身工作，
上述调用计数才是容量不足的直接证据，不能由采样简单推算吞吐收益。

实际分配/退休 helper 的 ASan/UBSan 测试覆盖 0、1、64、1024、4096 页容量，
满缓存释放、缓存耗尽、复用清零、关闭复用、非对齐和大页回退，通过。
1024 页的实机结果见下文，默认策略仍为 64 页。

实机验证补充：两端已启用 `LOOM_VFIO_POOL_CACHE_PAGES=1024`，每个容器最多保留 4 MiB 空闲页。
Garen 的 18 秒计数窗口：511190 次单页映射、443 次 mmap、0 次单页 munmap、0 次映射错误；
缓存耗尽率约 0.087%，原 64 页窗口约 39.3%。Ezreal：618175 次映射、0 次 mmap/munmap、
0 次映射错误。并发窗口逐项读取计数可能有一条边界差异；两个 owner 的实际 limit 都读到 1024。

两张 VF 各由 owner 本地 CPU worker 发往 Jinx，接收字节数与内容校验通过，
rx/tx discard、tx error 均为 0，guest dmesg 无 error 级别信息。
1024 页配置下两端正常关机退出 0，VFIO IOMMU 模块引用回到 0。
一次计数运行第三轮因接收器原定 600 秒期限到达而被断开，已确认接收器正常退出；
该轮未纳入性能结果，换新接收器后重新完成三轮校验。

无探针、Garen 单卡两个 CPU0/1 worker、每轮 6 秒三次，64 页为
`[151.912, 171.126, 168.361]` MiB/s（中位数 168.361）；
1024 页为 `[153.273, 147.112, 166.386]` MiB/s（中位数 153.273）。
这是跨启动的对照，样本重叠，未证明稳定的吞吐改善；不把减少 mmap/munmap 的结果等同于带宽提升。
目前保持代码默认 64 页，实验启动显式使用 1024 页，继续检查其余开销；guest 与 HPET 未改。

### Legacy shadow walk 的叶页表批读（实验开关）

`LOOM_VTD_LEAF_BATCH_READ=1` 或 Intel IOMMU 的 QMP 属性
`loom-leaf-batch-read=true` 启用默认关闭的批读路径。
仅在 owner IOMMU、caching-mode、legacy 页表、migration idle 下的
shadow 通知遍历使用，每个叶层 frame 最多缓存 16 个相邻条目（128 字节），
只覆盖本次遍历的 IOVA 范围且不跨页表页；普通 DMA 翻译与 replay 不使用。
只预读普通 RAM，设备内存、短的 RAM section、非对齐或溢出地址回退。
批读失败后当前 frame 逐条读，保持原跳过读错误的行为；通知顺序与完成处理不变。
快照不跨 frame/遍历或 invalidation 保留，下一条 invalidation 重新读取。
该模式依赖 caching-mode guest 在失效命令之前发布页表更新的既有顺序，
不修改 guest、QI 执行次序或 HPET。

原实现的 18 秒实机计数：276585 个叶层 frame，1233467 个叶条目，
加上父层，共 1786637 次 8 字节页表读取；无遍历错误。
按范围统计，批读预计可把该窗口叶层读取降到约 276585 次，
这是几何估算，并非已经测得的 syscall 或吞吐收益。

提取实际 helper 的 ASan/UBSan 6006 个用例通过，覆盖区间、尾部、
512 个条目边界、读取失败、RAM/device/section 回退和新 frame 读取更新。
实际新旧叶层 walk 的 4000 个差分用例通过，覆盖权限、跳过项、通知顺序、
hook 失败与默认关闭路径。QEMU 编译和 diff 检查通过；实机结果见下文。

实机验证：同次启动，Garen 18 秒 OFF 窗口有 1311076 次页表读调用，
读取约 1311087 个条目；ON 窗口有 743000 次调用（含 148304 次批读），
读取约 1632882 个条目。按读取量折算调用数减少约 54.5%。
Ezreal ON 窗口有 728676 次调用，读取约 1487347 个条目，减少约 51%。
两端的读取量与叶层范围条目数加父层读取相符，无 walk 错误。
计数是并发窗口逐项读取，存在少量边界差异；这些不是 CPU 时间或吞吐测量。
初次探针挂在已内联的 read 包装入口未命中，已改挂实际 `address_space_rw`，
OFF 的作用域读计数与逐条 getter 计数吻合后才进行 ON 对照。

移除所有探针后，同一 boot、4 CPU 在线、Garen CPU0/1 两个 TCP worker
向 Jinx 发包，每轮 6 秒，OFF/ON 交替六组：
OFF `[181.746,182.177,205.384,183.835,199.953,197.671]` MiB/s；
ON `[202.856,207.823,195.468,209.683,222.202,215.510]` MiB/s。
中位数 190.753 → 208.753 MiB/s（+9.4%），六组中五组 ON 更快，
仍有一组反向波动；结果仅代表当前 workload，不代表已达到普通 VM 或线性聚合水平。
其余配置固定：stage cache 1024、host batch unmap 与 QEMU coalesce 开启、
CQ events 开启、QI pair 关闭；guest 与 HPET 不变。

Ezreal CPU2/3 的独立 VF 发送校验通过；两端收发字节数和内容检查通过，
guest dmesg 无 error 级别信息，两卡 rx/tx discard、tx error 为 0。
两端在开启状态正常关机退出 0，VFIO IOMMU 模块引用均回到 0。
代码默认仍关闭；仅本实验显式开启，继续定位剩余开销。


### Owner 本地 BAR 扫描合并 BQL 获取（默认关闭的实验）

`LOOM_PCI_LOCAL_BAR_LOCK_BATCH=1` 在启动时开启，也可在完成设备目录初始化后
通过 QMP 设置 `/machine` 的布尔属性 `loom-local-bar-lock-batch`。
默认关闭。开启时，同一次 owner 本地 BAR 扫描和匹配后的本地分发共用一次 BQL，
避免每个描述符快照和分发各自重新获取锁。已经持有 BQL 的调用不额外获取或释放。
仍执行原有描述符读取、reset sequence 检查和分发 generation/BAR 验证，没有增加路由缓存。

远端 owner 扫描和 RPC 保留原来的锁范围；不能把新的外层 BQL 延伸到远端 RPC，
否则响应链中的本地回调可能需要同一把锁。guest、HPET、DMA 发布和中断处理未修改。

诊断先用 sched_switch/wakeup 区分阻塞与唤醒后等待，明确排除 TASK_REPORT_MAX
抢占标志；读取锁参数并对照 ELF 的 bql 地址，确认热等待属于 BQL。
高频 uprobe 会显著降低吞吐，其吞吐不能用于性能对照。随后用 QEMU 自带
`sync-profile on/off` 复核，扫描位置的获取次数与累计获取耗时也居前；
接收线程等待请求、磁盘线程池的空闲 condvar 等待不作为性能损失。

实际扫描函数的新旧差分 ASan/UBSan 测试 5000 组通过：
PIO/MMIO、读写、本地/远端、已持有/未持有 BQL、空目录、未映射和范围匹配；
检查相同请求及返回数据、锁平衡，并检查新外层锁不会覆盖远端 RPC。
两端 QEMU 编译和 diff 检查通过，代码提交 `b80cf7f7e7`。

同次启动，Garen 单卡两个 CPU0/1 TCP worker、三轮六秒，QEMU sync-profile
窗口中扫描加 BAR 分发获取 BQL 的次数：
OFF `1796953 + 135751 = 1932704`；
ON `798215 + 409546 + 117 = 1207878`（包含新增的本地外层获取），
原始窗口计数少约 37.5%。窗口内 IOMMU 分发数分别为 473170、469469，工作量相近；
这些是调用计数，不是消除了同等比例的 CPU 时间、DSM 等待或带宽损耗。

关闭所有 profiling 后，同次启动、四 CPU 在线，Garen CPU0/1 单 VF 发往 Jinx，
OFF/ON 交替六组、每轮六秒，接收字节数和内容检查全部通过：
OFF `[216.904,188.921,201.502,222.938,220.999,193.726]` MiB/s；
ON `[207.064,189.812,198.135,210.879,206.645,205.017]` MiB/s。
中位数 209.203 → 205.831 MiB/s（-1.6%），仅两组 ON 更快。
没有证明吞吐收益，所以不默认开启，也不把这项改动算作已获得的带宽优化。
下一步需要测量 BQL 持有期间的工作，区分 DSM 阻塞与设备处理本身的成本。

开启后的两个 owner VF 分别用其本地 CPU 向 Jinx 发送，接收长度和内容校验通过。
两块既有 1 GiB raw 盘的 O_DIRECT/native AIO 读取回归（每 worker QD4、
1 MiB 请求、预热 2 秒加统计 8 秒）完成长度和错误检查通过；
这次读取仅作功能回归，不用于新的磁盘吞吐对照。
四 CPU 在线、HPET 保持，guest dmesg 无 error 级别信息，
两卡 rx/tx discard 与 tx error 均为 0。
两端开关开启状态下正常关机，QEMU 退出 0，VFIO IOMMU 模块引用回到 0。


### BQL 持有期间的等待与 CPU 采样（2026-10-11）

为区分持锁线程和等待锁线程，诊断使用当前 ELF 的 `bql` 地址，
由编译类型确认 `QemuMutex.lock.__data.__owner` 位于偏移 8、长度 4，
读取实际 pthread mutex owner 并与当前 TID 比较。
没有仅依赖 `bql_locked` TLS 标志：条件变量等待可释放实际锁而仍保留该标志。
sched_switch 按 `prev_state & 255` 区分休眠和 runnable，排除抢占标志；
只对目标 QEMU 进程读取 owner。收集程序、事件和数据都在内存中，完成后清理。

Garen 30 秒窗口（四 CPU 在线、本地 CPU0/1 单 VF 向 Jinx 两个 TCP worker、
四轮六秒）：1374324 次 owner 读取，仅一次失败。
记录到持有 BQL 的 off-CPU 时间合计 22.724 ms，
其中 DSM/RDMA 函数族累计 4.517 ms，其余主要是用户态 nanosleep。
这是睡眠/被调度走的区间，不是完整 BQL 持有时间，也不包含在 CPU 上忙等的时间。
未持锁线程的 poll、KVM halt、接收 worker 条件变量等待不能归入持锁阻塞。
这个窗口不支持“长时间持 BQL 睡眠等待 DSM 是单卡主要瓶颈”的判断，
也不能据此断言 DSM 总成本很小。

另外按现有线程逐个打开 perf CPU-clock 事件，设置 exclude_guest，
以 99 Hz 和 199 Hz 采样，在采样点读取实际 owner；没有对每次锁操作插入 uprobe。
捕获 kernel stack 和用户 IP。199 Hz 的 26 秒窗口共有 7622 个样本，
owner 读取无失败，其中 3010 个采样点属于当前 BQL 持有线程：
用户态 1410（46.8%）、真实 VFIO/IOMMU 函数族 593（19.7%）、
DSM/RDMA 函数族 722（24.0%）、其他内核代码 285（9.5%）。
99 Hz 的较短重叠窗口也看到用户态和真实 IOMMU 工作占据较大部分。
事件会带来采样开销，这些窗口的吞吐不作为无探针对照。

函数族只是栈分类：DSM memcpy 栈包含本地拷贝、地址转换和锁，
不是全部在做网络传输；这些样本比例也不是可直接扣除的带宽损耗比例。
用户帧回溯受当前构建影响，部分 glib/libc 的 IP 只能解析为 unknown，
因此不把其全部计为 malloc、memcpy 或任何指定函数。
可识别的用户态热点包括 hash lookup、iova_tree_compare、队列完成扫描及分配。

下一项候选来自源码：VT-d shadow walk 已查找到待删除的映射，
`iova_tree_remove` 又执行范围查找、GTree 删除和结束查找。
对于与现有映射范围完全相同的删除，可以研究复用已找到的节点，
而部分范围/多节点范围保留原路径。
还未修改该路径；需要先确认精确匹配的占比并验证通知、范围删除与节点寿命语义。
本轮未修改 guest、HPET、host 模块或设备处理代码；本地 BAR 合并加锁保持关闭。


### 精确 shadow UNMAP 的重复范围查找（默认关闭实验）

`LOOM_VTD_EXACT_UNMAP_REMOVE=1` 或 Intel IOMMU 的 QMP 布尔属性
`loom-exact-unmap-remove=true` 开启；默认关闭，setter 要求 owner IOMMU。
仅 owner shadow 通知遍历、migration idle 使用，replay/普通 DMA 翻译不使用。
在 UNMAP 中，若先前找到的唯一映射与本次 IOVA/范围完全相同，
直接使用栈上的 target key 调用 GTree 删除，省去范围 helper 的重复查找及结束查找。
部分范围、多节点范围、缺失映射保持原行为。MAP 替换会先调用 notifier hook，
可能改变树中节点的寿命，所以该分支继续使用原范围删除路径。
不缓存映射或节点指针，不改变通知与完成顺序、DMA publication 或 hardware unmap。

原实现的 22 秒计数窗口中，491899 次范围删除全部是单节点精确 4 KiB 删除，
内部有 983798 次额外查找，读取无错误。
初始探针挂 iova_tree_find 包装函数却未命中，出现不可信的“全部为空”；
反汇编确认包装调用已内联，该次结果丢弃。
改挂实际执行的 libglib g_tree_lookup 返回入口后才得到上述有效计数。

实际 util/iova-tree 与新旧 vtd_page_walk_one 的 ASan/UBSan 差分验证
50000 个操作通过，含 ON/OFF、精确/部分/多节点范围、缺失映射、
MAP 替换、hook 失败、hook 删除旧节点。
用例实际进入新路径 625 次，其中 588 次为大于 4 KiB 的精确范围，
比较树的完整内容以及通知、返回错误和事件恢复状态。
两端 QEMU 编译与 diff 检查通过，代码提交 `718970dcb8`。

开启后的实机作用域计数：Garen 493802 次精确删除，全部 4 KiB，
GTree 删除成功 493802 次、删除内部 lookup 0 次；
Ezreal 428114 次、成功 428114 次、lookup 0 次。
两个窗口均无读取或删除错误，range fallback 次数为 0。
这里减少的是每次精确删除原有的两次额外 lookup，
caller 的初次查找和 GTree 删除自身的树搜索仍然存在。
计数窗口带探针，吞吐不作为性能对照。

全部探针移除后，同次启动、四 CPU 在线，Garen CPU0/1 两个 TCP worker
通过单 VF 发往 Jinx，OFF/ON 交替六组、每轮六秒：
OFF `[190.812,194.984,210.108,169.335,160.196,155.166]` MiB/s；
ON `[190.653,202.211,195.793,159.964,146.438,165.665]` MiB/s。
中位数 180.074 → 178.159 MiB/s（-1.1%），只有两组 ON 更快，
两种状态在后半段均下降。没有证明吞吐改善，所以保持默认关闭，
不把查找减少等同于带宽提升。接收字节数和内容检查全部通过；
其他配置保持，包括 HPET、guest #26、leaf batch、host batch unmap、
QEMU coalesce、CQ events、stage cache 1024；本地 BAR 合并加锁关闭。

开启时，两张 VF 各由 owner 本地 CPU 发送，接收字节数和内容校验通过；
两块既有 raw 盘的 O_DIRECT 读取完成长度/错误检查通过，仅用于功能回归，
没有重做磁盘读写聚合性能比较。
四 CPU 在线、HPET 保持，guest dmesg error 级别为空，两卡 discard/tx error 为 0。
两端开启状态正常关机退出 0，VFIO IOMMU 模块引用均回到 0。

### VFIO 成本拆分及 lazy 配置约束（2026-10-11）

在单页指针存储已开启的状态，以 199 Hz、host-only per-thread CPU 采样
读取真实 BQL mutex owner（不是 condvar 等待时仍可能保留的 TLS 标记）。
6098 次读取、0 次读取失败；持锁样本 2371：
用户态 1129、DSM/RDMA 函数族 559、VFIO/IOMMU 431、其他内核 252。
VFIO 子分类：GUP/pin 51、锁页记账 26、IOTLB flush 127、
unpin 9、map 10、unmap 12、其他 196。
约 2.2% 的持锁样本落在 GUP/pin、1.1% 在锁页记账，
当前证据不支持优先引入复杂的跨请求 page pin 缓存。
这些是 CPU 样本，不是函数累计耗时或可以直接扣除的带宽损失；
DSM 函数族也包含本地复制和锁，不能全部解释为跨网传页。
带采样的吞吐不用于性能对照。

20 秒 VFIO ioctl 几何计数：
严格配置 MAP 961482 次，全部单页；
UNMAP 的 1/2/4/8 页次数分别为 110471/49517/45961/71024，
没有 16 页请求，读取错误 0。扩大目前 16 页批量上限不能减少此窗口的 flush。

只追加启动参数 `iommu.strict=0`，不修改 guest 代码和 HPET，
同版本 guest #26、4 CPU 的启动、两卡 ping 通过。
全局早期日志确实打印 lazy mode，但这不代表设备最终使用 DMA-FQ：
随后日志打印 `DMAR: IOMMU batching disallowed due to virtualization`，
全部 sysfs IOMMU group type 为 DMA。
实际 guest 源码 drivers/iommu/intel/iommu.c 的 intel_iommu_init 中，
caching-mode 且非 first-level 默认 domain 的条件会调用 iommu_set_dma_strict。
因此这次配置实际被强制回严格域，不能将结果解读成有效 lazy 的收益测试，
也不能只用早期全局日志证明 lazy 生效。

配置尝试下的无探针 Garen 单卡六轮为
[215.917,232.868,215.821,211.790,194.045,210.295] MiB/s，
中位数 213.806，与先前严格模式 214.231 接近；跨启动不作严格配对比较。
另一 20 秒计数 MAP 1016643 次，仍全为单页；
UNMAP 1/2/4/8 页次数 115704/50939/43632/78083，读取错误 0。
原始计数依赖负载和窗口，不能直接比较两次绝对次数作为加速比例。
Jinx 向两张 VF 各发送 256 MiB，guest owner 本地 CPU 接收，
四条 128 MiB 连接的字节数与内容检查通过。
已恢复原 iommu.strict=1 的启动配置，没有把该参数保留为优化，
没有为了启用 lazy 关闭虚拟 IOMMU 的 caching-mode 或修改 guest。

### 同一源码单机路径与巨虚的网卡对照（2026-10-11）

当前内建 KVM 也保留不启用 DSM 的单机路径，不需要卸载模块或重启 host。
QEMU system/vl.c 在没有 -local-cpu 时将 local_cpus 设为全部 smp.cpus；
hw/i386/pc.c 仅在 local_cpus != smp.cpus 且无 shm_path 时启用 DSM。
内存访问同样按该条件选择 DSM ioctl 或本地 memmove。
单机实测可以正常启动并登录。这里是同一份修改版 QEMU/KVM 的单机对照，
不是未经修改的 upstream KVM 基线；CONFIG_KVM=y 当前仍不能卸载。

测试都使用 Garen 的同一个 X710 VF 0000:0c:02.0，QEMU 9.0.90、
host 6.6.32-krdma+、同一个 guest #26、同一个已有根镜像、
4 vCPU / 4 GiB / 两个 2 GiB NUMA node、HPET。
保留 VFIO x-no-mmap、x-no-kvm-msi、x-no-kvm-msix、2 queues、
RX512/TX128、RPS/XPS=0 和 IRQ0-1。
两个 worker 固定 guest CPU0/1，通过 enp0s7 向 Jinx 198.18.10.3 发送；
接收端固定 host CPU4-7，验证每条 TCP 连接的字节数和内容。
每轮约 6 秒，六轮，吞吐是应用数据 MiB/s，不是线速字节计数。
性能窗口无探针。host QEMU CPU/memory placement 没有额外 NUMA 绑定。

| 模式 | 六轮中位数 MiB/s |
| --- | ---: |
| 巨虚：严格 owner vIOMMU，测试前 | 196.309 |
| 同源码单机：严格普通 vIOMMU | 364.575 |
| 同源码单机：不配置 vIOMMU | 1121.584 |
| 恢复巨虚：严格 owner vIOMMU | 198.727 |

原始六轮：
GVM 前 [177.795,188.640,193.348,202.652,214.352,199.270]；
单机 vIOMMU [367.952,370.440,366.287,362.654,362.863,315.376]；
单机无 vIOMMU [1121.811,1121.526,1121.641,1121.368,1119.284,1122.100]；
GVM 恢复 [207.373,192.249,205.204,221.839,185.862,187.963]。

严格单机配置保留 intel-iommu,caching-mode=on,intremap=off 和
intel_iommu=on iommu.strict=1。
无 vIOMMU 参考只移除 intel-iommu 设备并用 intel_iommu=off；
guest sysfs IOMMU groups 为空，HPET 和所有 4 CPU 保持。
单机只有一张 VF/一块数据盘，巨虚仍呈现两端设备，但本轮只给 owner0 发流量。
因此对照不能把全部差距归因于某一个模块或纯 DSM。

独立的 20 秒计数窗口按单机 QEMU PID 过滤：
kvm_vm_ioctl_dsm 0 次，VFIO MAP/UNMAP 各 1659539 次，证明收发走单机路径。
计数窗口吞吐不纳入上表。所有无探针测试的接收字节和内容校验通过。
两次单机 VM 均正常关机退出 0，VF 恢复到 iavf，模块引用归零；
随后恢复两端巨虚，双卡 ping 3/3 通过，CPU0-3 在线、HPET、
iommu.strict=1 保持，guest error 级 dmesg 为空。
初始直接启动因关机 wrapper 已恢复 VF 驱动而缺少 /dev/vfio group；
确认进程已退出后才按原 wrapper 的绑定/恢复流程启动，没有动 PF。
Garen 的 libvirt worker-g4 PID6122 保持；Lux 未使用；
临时 Jinx sink 和地址已清理，未产生实验脚本、日志、磁盘或 pcap 文件。

结论：严格虚拟 IOMMU 的动态映射/失效在本机也有显著成本，
但不是巨虚剩余差距的唯一来源；还需区分暂存缓冲区、同步、元数据与 DSM。
不能直接给当前巨虚去掉 vIOMMU：当前暂存区注册依赖 IOMMU 通知。
后续应检查是否能将 owner 本地 DMA 暂存和数据队列同步与虚拟 IOMMU 解耦，
或建立不启用 DSM 的本地暂存对照，以定位暂存层的独立成本。
尚未实现或验证这些替代路径，不关闭现有失效机制或把 DMA 直接指向 DSM 页。

### 单机暂存对照（2026-10-11，代码 f988c1673a）

新增默认关闭的启动选项 LOOM_VFIO_LOCAL_STAGE=1，必须同时设置
LOOM_VFIO_DSM_DMA=1；仅用于 KVM 单机、无 shm 的严格 guest vIOMMU 对照。
保留 intel-iommu,caching-mode=on,intremap=off，以及 guest
intel_iommu=on iommu.strict=1。单机 -smp 4，不设置 -local-cpu，
GVM_OWNER_IOMMU=0。不能运行时切换，不能据此直接移除巨虚的 vIOMMU。
原分布式启用条件保持，guest 内核与 HPET 未改。

对照双方均显式使用 x-no-mmap=on,x-no-kvm-msi=on,x-no-kvm-msix=on,
x-no-kvm-ioeventfd=on,x-no-vfio-ioeventfd=on，避免暂存自动禁用 fast path
造成额外配置差异。开启方设置 LOOM_VFIO_POOL_CACHE_PAGES=1024、
LOOM_VFIO_BUFFER_RECYCLE=1。沿用既有 VF 绑定/正常退出恢复流程，
不要改绑 PF。实际启用函数旧/新差分与边界测试 864 项通过（744 项原行为比较），
ASan/UBSan、两端编译及 git diff --check 通过。

同一 Garen VF、guest 4CPU/4GiB/2NUMA、HPET、两个 CPU0/1 TCP worker，
每轮 6 秒，向 Jinx 198.18.10.3:49161 发流量并校验接收内容。
三次启动各六轮无探针总吞吐 MiB/s：
- 暂存 ON：[268.754,255.498,275.109,251.934,285.845,271.803]，中位数 270.279。
- 暂存 OFF：[447.492,407.003,430.644,408.613,428.384,432.034]，中位数 429.514。
- 恢复 ON：[287.065,310.987,314.715,288.916,289.493,296.550]，中位数 293.022。

有跨启动波动；该对照下开启暂存吞吐下降约 32–37%，不能当作稳定的精确比例，
也不能将损失全算成 memcpy 或从巨虚吞吐直接减出纯 DSM。
单机使用普通 vIOMMU，巨虚有 owner shadow/coalesce，二者不能作严格加性分解。
开启暂存的独立 20 秒计数：DSM ioctl 0、启用 true 1799596、
注册成功 899798/错误 0、scoped TX 51740/fallback 33。
单机确实没有启用 DSM；额外 Jinx→guest 接收 256MiB 内容校验通过。
所有探针窗口吞吐均不纳入性能对照。

恢复 ON 的独立 199Hz/26 秒 CPU 采样：7974 次读取、错误 0；
实际 BQL 持锁样本 3512，其中用户态 2086（59.4%）、
VFIO/IOMMU 1258（35.8%）、其他内核 168。
用户态包含未解析 libc/GLib、哈希、分配、队列完成处理等，
不能把全部用户态样本归因于复制。IOMMU 栈涉及失效提交、同步和 xarray 遍历，
采样比例不是吞吐损失比例。7 个非持锁 DSM/RDMA 栈分类样本包含普通路径守卫，
不推翻实际 DSM ioctl 为 0 的计数。

这次新增的是定位开关，没有声称提高性能。下一步拆分注册基线初始化、
暂存复制、队列同步、元数据与映射生命周期的成本，保留失效及发布顺序。
两端巨虚已恢复原分布式启动配置，单机对照正常退出并恢复 VF；
不安装覆盖 host 模块，不改 guest，不使用 Lux，不生成实验脚本、日志或 pcap。

### 暂存页重复清零：默认关闭的候选优化（2026-10-11）

巨虚 owner0 单卡发送的独立 22 秒探针窗口，注册入口仅见 4096 字节：
direction=1（设备只读）715540 次 / 2930851840 字节，
direction=0（双向）18882 次 / 77340672 字节，设备只读占约 97.4%。
返回成功 734434 次、错误 0；入口/返回计数有窗口边界差异。
带探针吞吐不作为性能对照；这不是应用有效吞吐或 DSM 网络传输量。
说明不能笼统假设所有映射都允许设备写，后续可研究只读映射的复制成本，
但不能直接将 baseline 与 pool 合并：重叠注册、切片和生命周期必须验证。

新增 LOOM_VFIO_SKIP_REDUNDANT_ZERO=1，默认关闭、启动时读取。
仅复用的 4KiB 暂存页可省去 memset；调用方必须声明 will_initialize，
并在硬件 dma_map 前完整注册全部区间。失败仍回滚且不安装硬件映射。
没有 GPA 的旧调用传 false，继续清零；新 mmap 的内核零初始化保持。
baseline/实际 guest 读取/复制、硬件 IOMMU 及失效和 DMA 发布顺序不变，
不会让真实设备直接 DMA 到 DSM 页。

使用实际 pool_alloc 与实际 vfio_container_dma_map_full 的 ASan/UBSan
测试共 31 项通过；外部 register/backend 在测试中使用受控桩，
覆盖开关、旧/翻译路径、缓存/新页、map/register/hardware 失败、
GPA 溢出、失败页随后走旧路径的清零、超过 64MiB 的分段初始化及第二段失败。
两端编译通过。当前运行进程仍是此前二进制，未在硬件启用该选项，
没有性能收益结论。下一步正常关机后验证真实 DMA 收发及回滚，
再用无探针 ON/OFF 对照决定是否保留运行时启用。

后续硬件验证（代码 7e03f0d475）：旧巨虚正常关机，两端 QEMU EXIT 0，
VF 恢复 iavf、模块引用均 0；未重启 host。两端用开关=1 重新启动，
实际 QEMU 环境已核实，guest #26、CPU0-3、HPET、严格 vIOMMU 保持。
双卡 owner ping 均 3/3；Garen 卡三轮发送 194.676/207.344/208.396 MiB/s，
Ezreal 卡 169.034/178.091/160.488，每轮两个 owner 本地 worker、6 秒，
均向 Jinx 发流量且接收端字节/内容校验通过。
这是开启状态的功能记录，缺少同源码 OFF 对照，不构成性能增益证据。
Jinx→guest 两卡各两次 128MiB（每卡 256MiB）接收内容校验通过。
候选开关在当前实验运行中为 ON，源代码默认仍 OFF；下一步无探针对照。
host 保留了旧实验正常退出时的 krdma -32 日志，时间早于这次新进程启动，
新收发期间未新增同类 error 级日志；guest error 级 dmesg 为空。
临时 Jinx TCP sink 与两地址清理，worker-g4 保持，Lux 未使用。

### 重复清零候选的无 DSM 对照结果（2026-10-11）

代码 7e03f0d475，同源码单机本地暂存，不设 -local-cpu；
LOOM_VFIO_DSM_DMA=1、LOOM_VFIO_LOCAL_STAGE=1，两方都保留严格 vIOMMU
和此前一致的 VFIO fast-path 开关。guest 4CPU/4GiB/2NUMA、HPET、
2 队列、RX512/TX128、RPS/XPS0、IRQ0-1，CPU0/1 两个发送 worker，
每轮6秒向 Jinx198.18.10.3:49161 发送并校验内容。
原环境 host CPU mask 0-191、内存未绑定，本轮未改变这一条件。

独立 12 秒窗口，在卸载探针后读取冻结计数：
- ON：map 755123、缓存4KiB 755123、可跳过清零755123、必须清零0。
- OFF：map735694、缓存4KiB734776、可跳过0、必须清零734776。
- ON恢复：map750812、缓存4KiB750812、可跳过750812、必须清零0。
三个窗口 read error、map返回错误、DSM ioctl 均0。
缓存条件由实际 DMA map 入口读取，分配函数行为已由实际函数测试覆盖。
使用当前 DWARF 验证 ctx.skip_redundant_zero 偏移33384、
ctx.free_page_count84、VFIOContainerBase.loom_dma40。
初始未冻结的计数读取出现单条边界差异，未采用，改为卸载后快照再测。
所有探针窗口吞吐不进入性能结果。

每次卸载探针并完成带探针流量后，独立进行六轮无探针测试，MiB/s：
ON：[268.036,301.998,295.274,242.814,245.712,250.512]，中位数259.274；
OFF：[267.498,296.461,293.429,285.856,249.199,251.318]，中位数276.677；
ON恢复：[326.080,296.364,292.141,314.680,294.770,316.984]，中位数305.522。
全部接收字节/内容校验通过。两次ON差约17.84%，且三组区间明显重叠，
不能证明候选优化的稳定性能增益。因此源代码默认OFF，
并将两端巨虚运行配置恢复OFF，未将清零减少等同于吞吐提升。

期间观察到不同启动的 host 放置明显不同：
OFF 进程 numastat 总驻留N0=95.55MiB/N1=621.74MiB；
ON恢复测试前N0=645.00MiB/N1=31.16MiB；VF numa_node=0。
这是进程总驻留，不能据此断言全部 DMA 暂存页的位置。
线程允许0-191，OFF main/vCPU0/1样本在54/188/49（N1），
ON恢复启动样本main/vCPU0/1在124/129/37（N0），不代表全窗口线程固定位置。
首个ON没有 NUMA快照。故该实验不能隔离清零与跨启动放置影响；
下一步固定 host CPU、内存NUMA策略，核实页分布再测试。
这些均无需修改 guest OS，不能将差距都归因于 DSM。

三个单机VM均正常关机EXIT0、VF恢复iavf、模块引用0，再恢复两端巨虚。
此前开启候选的巨虚也正常退出，两端EXIT0/refs0。
HPET、guest#26、host内核及模块参数保持；没有使用Lux或新增实验文件。

### host NUMA 放置的无 DSM 对照（2026-10-11）

代码仍为 7e03f0d475；本轮没有修改 guest 或实现代码。
采用同源码单机本地暂存模式：LOOM_VFIO_DSM_DMA=1、
LOOM_VFIO_LOCAL_STAGE=1、pool cache1024、buffer recycle1、
GVM_OWNER_IOMMU=0，不设置 -local-cpu。
重复清零候选保持OFF，严格 vIOMMU、guest#26、HPET、
4CPU/4GiB/两个2GiB guest NUMA backend、VFIO fast-path 开关不变。
guest CPU0/1 两个6秒发送worker，2队列、RX512/TX128、RPS/XPS0、
IRQ0-1，同一VF向Jinx198.18.10.3:49161发送，核对接收字节及内容。

只改变QEMU的host启动包装：
- near：env [原变量] numactl --cpunodebind=0 --membind=0 [原QEMU参数]；
- far：env [原变量] numactl --cpunodebind=1 --membind=1 [原QEMU参数]；
- near恢复：再次0/0。
VF 0000:0c:02.0 numa_node=0。host节点CPU为
N0=0-47,96-143，N1=48-95,144-191。
这是CPU与内存一起改变的联合放置对照，不能分别归因到其中一项。

三次启动各六轮无探针MiB/s：
near：[291.726,311.448,311.075,299.663,326.845,295.977]，中位数305.369；
far：[271.461,282.811,281.449,273.404,285.261,261.937]，中位数277.427；
near恢复：[287.196,289.058,312.188,288.170,323.058,308.756]，中位数298.907。
全部内容校验通过。两次near比far中位数高10.07%/7.74%，
near前后中位数差-2.12%；本轮所有near样本均高于所有far样本。
结果支持在此单机暂存配置下靠近NIC放置，而非证明巨虚已经获得同样提升，
也不能证明10%适用于所有负载。

独立12秒探针窗口先卸载再读冻结计数：
near map785757、far748455、near恢复851992；
read错误、map返回错误、DSM ioctl均0，候选跳过清零次数均0。
带探针的两轮预热吞吐不计入性能表。
逐线程检查实际Cpus_allowed_list，读取numa_maps的策略及各节点驻留页；
两块2GiB主要guest RAM VMA均采用目标bind策略且驻留页在目标node。
near初始测试后匿名VMA统计169358页在N0、0在N1
（首版统计含少量file/COW映射，后版将file VMA排除）；
near恢复测试后非file匿名VMA169325页在N0、0在N1。

far的核验发现200个非file匿名驻留页在N0、170386在N1。
host KSM run=1、merge_across_nodes=1。
两处64KiB/2MiB VMA的smaps KSM分别60KiB/740KiB。
用numa_move_pages的只读查询得到实际node，再读pagemap/kpageflags：
528个被检查页中200个node0页全部带KPF_KSM，328个其他页在node1；
没有迁移页或关闭host KSM。故不能声称far的所有匿名页都在N1，
但已核实off-node部分是KSM共享页，而非将绑定失败静默忽略。
主要guest RAM VMA仍全部在N1，策略为bind:1。
这也提示后续严格放置对照应考虑VM自身的内存合并配置，
不动其他VM使用的host KSM。

三个单机VM均正常关机EXIT0、VF恢复iavf、模块引用0。
恢复巨虚原host未绑定配置和候选清零OFF，避免将未测的巨虚放置收益写成结论。
下一步在巨虚做原配置/near/原配置对照；
NIC在N0、RDMA HCA在N1，应同时核实RDMA线程与缓冲位置的取舍。
HPET、guest、host内核和模块参数保持，worker-g4保留，Lux未使用。

### 巨虚全进程近端放置对照与线程角色（2026-10-11）

本轮性能代码仍是7e03f0d475；guest#26、HPET、4CPU、
严格owner vIOMMU、两个owner真实VF、两队列RX512/TX128、
RPS/XPS0及guest IRQ0-1/2-3保持。
两卡分别发送到Jinx10.3/11.3，每卡两个本地guest CPU worker、
每轮6秒、各6轮；Garen卡先测、Ezreal卡后测，并非双卡并发。
接收端按字节数及内容核对。本轮无BCC/perf探针，清零候选仍OFF。

原配置A：host线程允许0-191、内存policy default。
近端B：仅在原启动env变量之后、python wrapper之前插入
numactl --cpunodebind=0 --membind=0，两端相同；NIC NUMA0，HCA NUMA1。
恢复C：去掉numactl，恢复原配置。
逐线程mask与numa_maps核验，B所有线程都在0-47,96-143，
G/E非file匿名驻留页分别1072634/1070756全部在N0，policy bind:0。
A两端主要驻留页已经在N0，但线程可跨socket。
C的Garen驻留2个2GiB大VMA转到N1，Ezreal仍在N0；
原配置的first-touch不固定，不能把整个差距归因到某个线程或RDMA。

六轮原始总吞吐MiB/s：
A G=[215.107,186.982,209.871,195.276,201.384,163.282]；
A E=[181.131,167.431,181.472,176.950,159.209,174.348]；
B G=[208.596,212.975,235.813,213.826,226.939,200.854]；
B E=[173.574,163.618,148.092,159.765,176.840,181.078]；
C G=[215.552,223.292,194.854,208.839,191.776,209.295]；
C E=[187.947,194.151,188.198,162.530,182.861,190.491]。

中位数：
| 单卡 | 原A | 全进程近端B | 恢复C |
| --- | ---: | ---: | ---: |
| Garen | 198.330 | 213.401 | 209.067 |
| Ezreal | 175.649 | 168.596 | 188.073 |

Garen的B比A/C高7.60%/2.07%，Ezreal低4.02%/10.36%。
样本明显重叠、原配置放置也随启动变化，没有证据证明全进程近端同时改善两卡。
保持原启动配置；不能将两卡分开测的结果相加声称并发聚合性能。
单机无DSM下的近端收益仍不能直接迁移为巨虚收益。
本轮没有按流量归一化DSM故障/远端页计数，所以不能声称已经证明RDMA导致下降。

代码检查确认 migration/rdma-gvm.c：
recv_thread_func负责CQ接收与消息分发，request_worker_func每peer
FIFO执行模型回调/BQL/可能嵌套RPC，接收不能被请求处理阻塞。
它们是QEMU设备RPC路由线程，区别于host内核DSM/RDMA线程。

为下一步按角色亲和性实验，代码3d1fcd14c7仅在这两个线程入口设置Linux名字：
gvm-rdma-rx、gvm-rdma-req。启动各调用一次pthread_setname_np，
没有改CPU/memory affinity、队列、协议、DMA、锁或失效机制。
两端编译通过；新启动各核实一个rx/一个req名字。
这是定位改动，不是性能优化，性能表采集在此改动之前。
下一步先核实这些线程的CPU/等待/BQL成本，
再在同次启动中比较NIC处理留N0、RDMA接收靠近HCA的角色分配。
不通过全进程绑定推断某一角色的因果。

A/B/C均正常关机两端EXIT0、refs0，VF恢复iavf后才再次启动。
新版本恢复原unbound配置，root登录、双卡owner ping均3/3通过，
新代码双卡发送内容校验通过，guest error级dmesg为空。
原Garen libvirt worker-g4保持，host和guest内核、HPET、模块参数保持，
Lux未使用，没有产生实验脚本、日志、pcap或新磁盘。

### 同次启动的RDMA接收线程亲和性与RPC等待（2026-10-11）

代码3d1fcd14c7，guest#26/HPET/4CPU/严格owner vIOMMU保持，
不重启VM、不迁移内存、不改变其他线程的亲和性。
只对两端gvm-rdma-rx切换CPU mask：
N0=0-47,96-143；N1=48-95,144-191（HCA在N1）。
6对次序交替0→1、1→0，每个条件分别测Garen和Ezreal卡，
两本地guest CPU worker、6秒、Jinx10.3/11.3接收字节/内容校验。
两卡顺序测，非并发聚合。性能窗口无BCC/perf。

原始每对N0/N1 MiB/s：
| 对 | Garen N0 | Garen N1 | Ezreal N0 | Ezreal N1 |
| --- | ---: | ---: | ---: | ---: |
| 0 | 236.900 | 191.786 | 191.294 | 160.506 |
| 1 | 206.144 | 202.178 | 178.211 | 185.679 |
| 2 | 201.077 | 212.459 | 186.730 | 169.301 |
| 3 | 196.645 | 204.829 | 171.310 | 185.547 |
| 4 | 211.912 | 198.458 | 150.198 | 150.958 |
| 5 | 197.956 | 205.915 | 194.008 | 175.033 |

Garen中位数203.610/203.503（N1 -0.05%），
Ezreal182.471/172.167（N1 -5.65%）。
两卡均只有3/6对N1更快，区间重叠，没有一致改善证据。
24个发送测试全部内容校验通过。
终于恢复两端原mask0-191，并通过实际sched_getaffinity再次验证，
不是只恢复启动命令。没有采用新的接收线程默认绑定。

只读/proc统计取约81.6/81.4秒区间，覆盖部分交替负载而非每个条件：
Garen活跃vCPU0/1 CPU时间24.086/21.766秒，
request线程4.637秒、receiver3.420秒、主线程3.918秒；
Ezreal活跃vCPU2/3为31.286/18.638秒，
request0.569、receiver0.512、主线程1.704秒。
主要vCPU运行队列等待约4–10毫秒，但调度次数很多：
Ezreal vCPU2/3增量3782455/1841285。
schedstat CPU时间包含guest执行，不能当作纯QEMU/VFIO或DSM开销；
低runqueue等待也不等于没有阻塞/同步等待。
瞬间wchan接收线程为do_sys_poll、请求线程为futex_wait_queue，
与CQ事件接收和FIFO worker的代码一致。

随后另开22秒探针窗口，只观测Ezreal真实PID687902，
用rdma_router_call、rdma_send_msg及libc usleep的入口/返回
按TID、消息类型、调用嵌套范围累计墙钟时间，结束卸载后读取冻结表。
没有嵌套同线程call冲突；该窗口的带探针吞吐不计入上表。
在MMIO（type2，来源interrupt-router.c枚举）：
RPC完成205458次，累计调用墙钟5022.746毫秒；
嵌套send_msg累计1604.929毫秒；
send范围usleep205429次/1108.909毫秒；
send之外、RPC内部usleep436746次/2401.373毫秒。
睡眠两个范围互斥，合计3510.282毫秒，约占本窗口RPC累计墙钟69.89%。
平均单次usleep约5.4–5.6微秒，不能按默认50微秒推算；
读到进程主线程timerslack_ns为1，并非实测所有线程的timer slack。
外层RPC时间包含锁、发送及回复等待，跨线程累计也不等于独占实际墙钟；
探针本身会增加时延，不能将比例解释为带宽损失或未观测的正常延迟。

源码rdma_send_msg同步消费SEND CQ后才复用发送缓冲；
rdma_wait_completion和rdma_router_call在CQ为空时usleep(1)。
回复CQ每peer只有一个consumer，由response_lock保护。
普通mmio_forwarding还持有io_forwarding_mutex。
尚未确认这些MMIO对应哪些设备地址，也未将它们全归因于某个设备或HPET。
它们是QEMU设备RPC等待，与kernel DSM页传输需要分别核实。

下一步针对具体等待路径评估有上限的CQ短轮询：
保持发送完成验证、缓冲生命周期、序号/类型/长度验证、
每个回复CQ的单consumer约束、原超时和错误行为、IO顺序。
不能直接去掉usleep变成无界忙等，或跳过完成就复用缓冲。
HPET设备、guest时钟和guest OS保持。
本轮没有实施轮询优化，没有宣称提升性能。
原运行配置保留，host/guest内核及模块参数未变；
Garen worker-g4保持、Lux未用，没有产生实验脚本/日志/pcap/磁盘。

### 2026-10-11：MMIO 地址归因与有界 CQ 短轮询

在旧运行版本 PID687902 上，另开22秒 BCC 窗口观测实际 mmio_forwarding
入口地址和返回耗时，Garen 单卡3轮6秒到Jinx，数据全部校验通过。
探针在统计前卸载，无同线程嵌套冲突。
本窗口只见0xfed000f0：vCPU2完成15811次/累计1854.942毫秒，
vCPU3完成10149次/777.377毫秒。源码HPET_BASE=0xfed00000、
HPET_COUNTER=0xf0，确认这些是远端HPET主计数器读取。
该窗口不能证明所有负载的所有MMIO都属于HPET，也不能与前窗口次数直接相除。
它解释了一个待验证的机制：远端CPU即使不跑本地网卡worker，
时钟读取仍会给master带来同步MMIO请求；不是直接观测的DSM页请求。

代码提交8b5ccca0d2新增运行时属性：
`qom-set path=/machine property=loom-rdma-rpc-spin-ns value=5000`，
两端分别设置，单位ns，范围0..20000，默认0。恢复用value=0。
仅改变rdma_wait_completion的SEND CQ和rdma_router_call的回复CQ等待策略；
第一次空poll开始计时，每次wait只有一个总预算，耗尽后一直usleep(1)；
不是每次sleep后重置预算，不是无限忙等。活动wait保留自己的快照。
使用host CLOCK_MONOTONIC，不改变HPET设备、guest clocksource或guest OS。
发送完成、缓冲复用、原5秒/调用超时、WC/回复序号/类型/长度检查、
回复CQ单consumer和FIFO/IO顺序保持。默认0不额外读取ns时钟。
属性越界拒绝且保留原值，0/20000/20001/UINT64_MAX两端QMP验证通过。

从实际源码提取等待helper和旧/新SEND完成循环，外部CQ、时钟、sleep stub，
memfd编译ASan/UBSan，48案例通过：默认0逐步等同旧循环、
即时/延迟完成、CQ错误、WC失败、超时、活动预算不可延长、
预算耗尽不继续读ns时钟、无符号时钟绕回、非法配置不变。
这是有界策略/完成语义测试，不是实际网络延迟或完整RDMA容错验证。
两端ninja与git diff --check通过，经正常guest poweroff两端QEMU_EXIT0、
VF恢复iavf且模块refcnt0后，新版本两端编译启动，guest原#26/4CPU/HPET。

同一次新启动六组交替0/5000ns，两个owner各2个本地CPU worker，
每卡6秒向Jinx，先Garen后Ezreal，每条件两卡分开测，不是并行聚合。
24次卡测试/48条TCP连接内容和字节数全部PASS，性能窗口无BCC/perf。

| 指标 | 0ns中位数 | 5000ns中位数 | 变化 | 5000ns胜出组数 |
|---|---:|---:|---:|---:|
| Garen卡 MiB/s | 205.206 | 197.623 | -3.70% | 1/6 |
| Ezreal卡 MiB/s | 186.923 | 183.752 | -1.70% | 3/6 |

配对原始值（pair/budget_ns/G/E）：
```
0 0 197.194 186.941
0 5000 224.826 189.029
1 5000 198.971 199.495
1 0 217.265 154.912
2 0 208.476 184.210
2 5000 185.672 170.393
3 5000 210.974 172.705
3 0 222.112 186.906
4 0 200.807 196.011
4 5000 196.276 207.213
5 5000 189.777 178.474
5 0 201.936 196.808
```

每条件外部/proc各线程schedstat CPU总和窗口约13.05..13.62秒：
Garen CPU累计中位数0/5000ns=13.370/13.225秒，
Ezreal=11.878/11.670秒。
包含guest执行和短暂控制/间隔，不能当纯host开销或固定工作量CPU效率。
没有证明5us稳定提升，不启用该候选；两端已读回恢复0。
guest错误级dmesg为空，CPU0-3和HPET保留，Jinx临时IP/收发进程清理。
不重跑磁盘性能，不使用Lux，不产生实验脚本/日志/磁盘文件，
Garen worker-g4 PID6122保留，host/guest内核和模块参数保持。

下一步须分别测量master侧HPET请求等待BQL、实际MMIO处理和回复发送耗时。
源码address_space_rw通过prepare_mmio_access持BQL调设备，
完成后释放，之后才发送回复；不能误称其一直持BQL等待SEND CQ。
同一master的本地网卡和远端MMIO可能竞争BQL，需要实际等待证据。
先区分服务端排队/执行与传输/唤醒，再选择可保持HPET的通用路由优化，
不以本次睡眠比例推断带宽损失，也不把架构RPC开销全归因DSM。

### 2026-10-11：master MMIO 分解与稳定 BAR miss 快速排除

原代码运行PID3836434/TID3836731，22秒探针窗口，
仅在handler消息type2范围记录函数累计墙钟（可嵌套，不相加）。
MMIO handler25198次/2953.435ms；loom_pci_handle_mmio25198次/2113.178ms；
HPET地址address_space_rw25193次/494.399ms；
hpet_ram_read25193次/75.157ms；rdma_send_msg25196次/173.654ms；
范围内bql_lock_impl201584次/1994.325ms（约每完成请求8次）。
同线程各阶段无嵌套冲突。边界跨probe attach/detach计数差，
以及uprobes对所有本地热BQL调用的扰动使吞吐降到67..74MiB/s，
本窗口不能当实际延迟/吞吐基线或用其占比估算性能收益。

源码解释：HPET也先走loom_pci_handle_mmio→bar_access，
分别对3个本地设备snapshot_bars、refresh_remote_mappings、
3个远端设备snapshot_bars，均可能获取BQL；
确认不属于PCI后，再经address_space_rw获取BQL读HPET。
回复发送已在address_space_rw释放BQL之后，不持BQL等待发送CQ。

代码9cfe76fc2c新增默认关闭的
`qom-set path=/machine property=loom-bar-negative-filter value=true/false`。
两端分别设置。稳定miss前置检查持一段BQL和directory_lock，
使用本地真实prefix与远端已发布incoming prefix直接检查BAR范围，
无新地址缓存、无配置快照复制、无新远端RPC。
只有确认不可能命中才直接返回false，随后照常访问原MMIO设备。
BAR命中仍走原snapshot/generation/execute路径；不改变HPET设备或时钟。
目录控制未激活、missing prefix、odd reset sequence，或directory_epoch
不等于applied_directory_epoch（代理映射尚待应用）都回到原完整扫描。
后者避免跳过更新映射导致访问仍映射的旧BAR。
BQL→directory_lock顺序与现有snapshot相同；已有BQL保持，不释放调用方锁。

实际bar_access_may_match源码memfd ASan/UBSan测试6046案例通过：
本地/远端BAR和边界、I/O vs MMIO、overflow、未知/odd prefix、
迁移待应用与应用后的旧/新地址、锁获取与恢复、3000随机配置。
外部状态和锁为stub，测试不等同真实设备热拔插/迁移全流程验证。
两端ninja/diffcheck通过，正常guest poweroff两端EXIT0、
VF恢复iavf/refcnt0后新版本启动，原#26/4CPU/HPET，owner pings均3/3。

同一次启动六组交替OFF/ON，两个owner各2本地CPU worker、6秒到Jinx；
Garen先、Ezreal后，不是并行聚合；无BCC/perf性能窗口。
24次卡测试/48条流字节及内容校验全部通过。

| 吞吐 MiB/s | OFF中位数 | ON中位数 | 变化 | ON胜出 |
|---|---:|---:|---:|---:|
| Garen卡 | 218.241 | 212.504 | -2.63% | 3/6 |
| Ezreal卡 | 184.670 | 187.230 | 1.39% | 2/6 |

配对原始值（pair/ON/G/E）：
```
0 0 203.573 170.971
0 1 220.937 187.908
1 1 199.912 186.552
1 0 227.023 191.684
2 0 234.783 191.282
2 1 243.784 181.153
3 1 214.493 174.621
3 0 235.224 178.058
4 0 206.924 156.925
4 1 210.514 207.919
5 1 195.520 195.468
5 0 209.458 206.311
```

独立操作计数探针窗口12秒/模式，用真实handler数据过滤type2和HPET地址，
统计每个完成handler范围的BQL进入次数；
全部探针attach完成后才打开记录开关，先冻结记录再detach：
OFF11111次请求/88888次BQL，每次min=max=8；
ON17364次请求/34728次BQL，每次min=max=2。
即本范围实际75%更少获取，但请求次数不同，不能比较原始总数或将75%
解释为吞吐提升。每窗口Garen另跑一轮6秒TX内容验证，带探针吞吐不作基线。
初次仅detach后读取记录出现边界请求min0，已废弃该初次读数，
改用明确记录开关冻结重测得到上面的完整范围计数。

操作减少成立，稳定吞吐收益未证明，不默认或运行时启用：两端已OFF读回。
CQ短轮询仍0。guest错误级dmesg为空，CPU0-3/HPET保持，
Jinx临时IP和收发进程正常清理，没有实验脚本/日志/新磁盘文件。
当前巨虚正常运行，Garen另一个worker-g4保持，不使用Lux。
下一步仍需区分剩余BQL排队、设备DMA/vIOMMU处理及DSM，
不能由本次减少操作推断全部竞争已消失。

### 2026-10-11：用低频 DWARF 采样定位本地数据路径

本轮不改实现：运行9cfe76fc2c/PID3850850 Garen，guest原#26/CPU0-3/HPET，
BAR负向检查OFF、CQ短轮询0。仅测Garen卡2个本地CPU worker向Jinx，
每窗口3轮6秒，所有字节/内容验证PASS，采样吞吐不作对照基线。
使用已安装/usr/lib/linux-tools/6.8.0-146-generic/perf（6.8.12）；
/usr/bin/perf包装器找不到6.6.32专用路径，不安装工具/改内核。
记录cpu-clock:H/99Hz，evlist实际读回exclude_guest=1，
perf数据只在memfd，--no-buildid --no-buildid-cache，不产生实验文件/缓存。

初始frame-pointer回溯出现0xa8、0xc0、ASCII数据式伪地址和不合理g_free祖先，
废弃fp Children归因，不能称g_free消耗约21%。改用dwarf,8192，
report/script显式--kallsyms /proc/kallsyms，DWARF窗口lost samples=0。
当前vfio_iommu_type1的srcversion与源码ko同为05DDC81760CD1558FACF0C1。
用户态少量未知libc地址保留，不擅自归因。
22秒DWARF report的inclusive（嵌套不能相加）：vtd_process_inv_desc28.00%、
vtd_sync_shadow_page_table_range23.98%、vfio_container_dma_map_full11.12%、
vfio_legacy_dma_map4.57%、vfio_legacy_dma_unmap4.06%、master handler1.74%。
vtd_fetch_inv_desc两处内联路径15.93/12.14%，不能直接相加去重。
这些路径也包含DSM，所以不能把vIOMMU inclusive全部称非DSM。

再开22秒DWARF窗口，用perf script --inline导出内存文本；
按样本period加权、每类每样本只记一次：2010样本/20303030100ns权重，
解析失败块0。分类标识DSM=kvm_dsm/ivy_kvm_dsm/krdma_/dsm_；
VFIO=vfio_/loom_vfio_（含QEMU回调与同步，不仅kernel map）；
vIOMMU=vtd_/iommu_dispatch；RPC=handler/router_call/send_msg。
按DSM→VFIO→vIOMMU→RPC→OTHER优先级互斥分类：
18.756/34.527/12.537/2.637/31.542%，VFIO+vIOMMU互斥部分47.065%。
不去重的inclusive：vIOMMU41.841%、VFIO40.398%、RPC2.985%；
vIOMMU且无已识别DSM帧35.423%，VFIO且无已识别DSM帧34.527%。
“不含已识别DSM帧”不证明完全独立于DSM（缺帧、DSM调用前后、通用syscall等）。
CPU采样不含阻塞/等待时间，18.756%不是DSM总带宽损失，
也不能排除DSM等待仍是关键瓶颈；仅Garen此负载，未观测Ezreal或双卡并行。

下一步检查本地高频DMA/vIOMMU映射重建及QI处理。
container-base.c普通vfio_container_dma_map传gpa=NULL，
translated map才在硬件dma_map前注册/初始化外部pool。
不能直接从巨虚参数删vIOMMU当作方案：普通staging尚无同样的GPA初始化契约。
减少重建必须保留硬件IOMMU隔离、正确GPA/pool初始化、DMA发布与完成同步、
失效/解除映射/释放顺序；新方案需匹配配置对照。
本轮无新优化或吞吐提升声明。guest代码和HPET保持，
CPU0-3/HPET及错误级dmesg为空已确认；巨虚与Garen worker-g4保持，
Jinx正常STOPLOOM并删除临时IP，没有使用Lux或实验文件。

### 2026-10-11：普通 RAM 映射与稀疏物理 DMA 路径的契约核对

本轮为源码/运行布局核对，不启用新数据路径、不改guest/HPET。
普通RAM listener真实入口common.c:vfio_listener_region_add，
IOVA是section.offset_within_address_space加主机页对齐修正；
VFIO container listener注册在bcontainer->space->as。
pci_device_iommu_address_space在没有guest IOMMU或bypass时返回
&address_space_memory，只有此身份下普通RAM的IOVA才可按GPA使用。
不能把其他AddressSpace、RAMDevice或RamDiscardManager都当作同样的RAM。
RAMBlock内部offset/alias不是GPA，实际GPA必须按AddressSpace外部坐标处理。

QMP info mtree -f实测当前system memory：
n0主RAM低地址段至0x7fffffff，低端有VGA/ROM覆盖；
n1在0x100000000..0x17fffffff。两段各约2GiB且中间并不连续，
0xb0000000..PCI/MMIO/HPET等不是普通RAM。不得把0..4GiB全当RAM授权。
VGA VRAM在0xfd000000另有16MiB，必须另辨RAM类型而非混入n0/n1。
实测Garen可用内存约368462MiB，当前QEMU RSS4308332KiB；
内存充足不证明全域同步可接受。QMP现有filter=false、CQ spin=0。

阻止直接开启“全RAM注册”的具体代码：
1. loom_vfio_dma_register每次最多64MiB，buffer_new为每buffer分配
   sizeof(header)+2*size（baseline+scratch），另外map还有pool。
   若全4GiB注册，额外虚拟存储约12GiB/container（4GiB pool+两份缓冲），
   不含guest RAM与metadata；resident触碰量/保留量随同步变化，不是实测RSS预测。
2. page_index_layout遇非4KiB注册禁用page_indexed，
   page_generation当前因此返回0，data IRQ scoped路径会回退。
3. sync(false/true)全域路径会遍历全部注册buffer，控制vector或unknown状态
   仍用此路径。全RAM注册因此可能在中断/doorbell上扫描、同步GB级无关内存。
4. 非page buffer可合法重叠（相同翻译）；scoped snapshot只选择一个buffer
   时不能擅自忽略其他baseline。不能简单把page_generation非page检查删掉。
5. partial unmap先完成所有device publication，再队列invalidate、
   保留slice/改变映射/munmap；buffer_slice保留原generation和baseline，
   新布局必须同时验证覆盖/生命周期/重叠语义，不能凭一个旧cache pointer复用。

更合适的后续原型：仅支持确认的X710 VF物理RAM模式，启动opt-in，
保持host真实IOMMU隔离；guest内核与HPET不改，只评估QEMU平台配置。
普通RAM listener先描述合法GPA权限/区间，而不是把全RAM作为活跃同步buffer。
已观察AdminQ/TX/RX队列首次使用的页，才分配owner本地4KiB pool，
初始化注册并完成硬件VFIO map；随后复用同一物理页映射。
普通物理DMA下guest没有vIOMMU MAP/UNMAP通知，必须由RAM区间失效、
queue reset/停止DMA和container退出定义寿命，不能照搬当前短期IOVA寿命。
单页有效性以GPA、RAM权限、注册generation和当前授权区间为依据，
触及无效/ROM写入/MMIO/RAM hole时不得创建硬件映射。
RAM只读权限与queue方向不同：普通RAM R/W页重复用于TX/RX时不能因上一用途
保留错误权限；ROM等只读区间始终拒绝设备写权限。
硬件map成功前所有描述符/payload页完整初始化；新map失败必须全回滚；
MSI先capture完整device snapshot，payload先发布，completion最后发布，沿用排序。
未知AdminQ/数据队列状态不能用“全4GiB同步”掩盖未实现协议；
该物理模式应明确拒绝未支持状态/设备，保留原vIOMMU路径作为可靠回退配置。
宿主RAM区间remove必须先停硬件DMA/完成原有发布，再解除真实IOMMU映射、
失效队列状态和释放pool，测试要覆盖失败与部分范围。
保留已识别页可减少运行时map churn，但会增加长期页/内存保留；
页数不能超过授权RAM页数，需测稳定运行映射数量、RSS、启动与回收成本。

当前AdminQ ASQ/ARQ base/count、data queue/ring/payload观察器已经存在，
因此有候选同步范围来源，不等于上述物理路径已经实现。
先验证授权范围、第一次map/回滚、queue依赖与失效的模拟外部状态测试，
再做匹配配置单机无DSM控制与真实Garen/Ezreal功能/吞吐对照。
禁止只删intel-iommu参数便宣称支持；现有gpa=NULL map没有该契约。
本轮保持巨虚、所有CPU、guest与host内核及模块参数；不使用Lux/实验文件。


### 2026-10-11：稀疏物理 DMA 核心 API（尚未接入启动路径）

hw/vfio/loom-dma.c 新增 loom_vfio_ram_add/remove/read_cpu/materialize，
由 caller 持 BQL，且仅在 staging 开启、container AddressSpace 严格为
address_space_memory 时适用。add 只记录对齐的普通 RAM GPA 范围及 RAM 权限；
caller 必须先判别普通 RAM、RAMDevice、RamDiscardManager 与 MMIO，
不能把 RAMBlock offset 或其他 AddressSpace 当作 GPA。
read_cpu 先确认请求完整覆盖合法区间，然后读取 CPU GPA 视图；
没有建立 active buffer 或硬件映射，用于第一次 queue 发现。

materialize 先检查全部覆盖页及 RAM 写权限、已有页的 GPA/权限/pool 身份，
再逐个建立并初始化 4KiB owner pool。沿用实际
vfio_container_dma_map_iommu/full 在 hardware dma_map 前注册/初始化的顺序；
重复使用相同页不重复注册或 map，普通 R/W RAM 允许 TX/RX 角色复用。
失败只回滚本次新建的页，保留此前有效页。
回滚或 revoke 的 backend unmap/publication 失败时保留仍可能被硬件访问的 pool，
标记该模式失败，拒绝继续使用；不得将 metadata 存在当作 hardware map 仍有效。
remove 先检查请求全部属于授权范围，再解除 backend map、执行原有 publication/
队列失效/metadata 回收，然后拆分剩余权限区间。
caller 必须先停止对应 DMA，失败后不得恢复该路径。

测试从当前源文件抽取实际新函数和实际 map_full/map_iommu 编译，
CPU/GPA 读取、pool 分配与 VFIO hardware backend 为模拟边界，
不是运行真实硬件的测试。memfd 中 GCC ASan/UBSan：
238 个定向检查，加 4000 步确定性状态操作，共 348834 次断言 PASS。
覆盖无映射授权、RAM hole/高 GPA/readonly/overflow、
多页初始化及 hardware map 每个失败位置的回滚、旧页保留、
unmap/publication 失败后保留与禁止复用、stale GPA 拒绝，
以及 authorize/remove 分裂与重新授权、页复用和权限变化的参考模型核对。
legacy teardown 源码确认 container fd close 在 object_unref/finalize/free pool 前；
新路径仍需在接入 MemoryListener 时验证设备停 DMA 和动态 RAM 删除的顺序。
QEMU 编译通过、git diff --check 通过；未生成实验文件。

这些 API 目前没有 MemoryListener 或 AdminQ/TX/RX 调用者，
没有物理 DMA 启动开关，也未移除当前 guest vIOMMU；
正常运行路径仅增加最终释放空授权对象的检查。
不能据此声称物理模式已能启动或性能已改善。
后续须在真实硬件 enable/tail 之前发现并初始化完整 ring/payload 依赖，
限定已验证 X710 VF，拒绝未知状态并保留原 vIOMMU 启动配置。
guest 内核、HPET 和运行中巨虚配置保持不变。

### 2026-10-11：再次核对 guest 远端 CPU 下线影响

同一次巨虚启动、Garen 本地单卡发送至第三台 Jinx、
本地 /dev/vda raw 单盘 O_DIRECT 读；两 worker 固定 guest CPU0/1。
每状态各三轮，NIC 每轮6秒，磁盘1MiB/QD4每worker、预热2秒/计量8秒。
依次 CPU0-3在线、CPU2/3下线、恢复CPU0-3；HPET与源码/配置保持。
中位数（MiB/s）：NIC 226.875 → 256.661 → 210.236；
单盘读 1989.125 → 2037.375 → 2015.625。
下线时 NIC 比首组高13.13%，单盘读高2.43%，磁盘仍在本次波动范围。
远端 CPU 在线影响 NIC 的证据可重复，但该对照不区分 DSM、
跨节点同步、时钟访问或 CPU hotplug 导致的中断/队列变化，
也不是普通 VM 匹配配置对照，不能声称全部瓶颈已解决。
NIC 所有字节/内容验证通过；读 completion 长度正确。
最后 CPU0-3 恢复、中断请求亲和性恢复错误列表为空、guest 错误级 dmesg 为空；
Jinx 临时接收端/地址已清理，不使用 Lux、不改 guest、不生成实验文件。


### 2026-10-11：保留稀疏页首次注册前后的队列配置

接入前源码核对发现两处 bootstrap 不兼容：
queues_get 原来要求已经存在 loom_dma，因而第一块 pool 创建前的
AdminQ base/count 写入不会被记录；register 完成初始化时原有
loom_iavf_dma_invalidate_container 又会在页与 ring 相交时 reset，
清空之前已观察的 base/count。稀疏模式若照搬此顺序无法可靠启动。

现在允许已存在合法 RAM 授权对象（loom_ram）但尚无 active pool 的
已识别 X710 VF 创建观察状态；普通非 staging/无授权路径保持原检查。
materialize 完整预检且只映射未拥有的新页时设置 BQL 内部 materializing 标志，
registration_invalidate 仅在这个新页初始化阶段保留观察状态。
不省略 ordinary translated registration、unmap/revoke 和失败回滚的失效；
失败内层 map_full 自身清理也仍失效，新页失败不能保留可运行假象。
成功及失败均退出 materializing，嵌套 materialize 返回 EBUSY。
已覆盖且身份/权限一致的暖页请求在完整预检后直接返回，
不分配 rollback array；有缺页时仅为缺页数分配。

memfd GCC ASan/UBSan 检查当前实际 core/map_full/map_iommu/
registration_invalidate，并校验生产 register 只使用该 hook：
4000 步状态操作及定向检查共 348856 次断言 PASS。
新增检查覆盖新页初始化保留观察状态、scope 外失效、失败/撤销继续失效、
scope 清理/重入拒绝、暖页与失败预检不发生 g_new 分配。
外部 CPU/pool/hardware map/unmap 与失效回调仍是模拟边界，
不把此测试称为真实 VFIO 或完整 register 的硬件测试。
另抽取当前实际 iavf 类型、queues_get/observe_write/reset/
queues_invalidate 函数，外部 DMA baseline/request 回调模拟：
14 项 bootstrap 检查 PASS，确认第一块 pool 前可记录 ASQ/ARQ base/count，
pool 出现后保留状态，原非授权路径仍需要 loom_dma，reset 清除配置。
QEMU 编译与 git diff --check 通过。

读取当前未改动 guest drivers/net/ethernet/intel/iavf/iavf_adminq.c
iavf_config_asq_regs/config_arq_regs 的实际顺序：
head/tail 清0 → 写 LEN enable → 写 BAL low → 写 BAH high；
ARQ 最后再写 tail=count-1，ASQ 日后提交时写 tail。
所以“只在 tail 时 materialize，但之前完全原样转发 enable”
尚不足以证明首次硬件访问的安全顺序。
后续要完整处理 guest-visible queue 配置、实际硬件启用的先后关系，
以及重配置/reset/拒绝未知状态，不能在 base/count 尚不完整时猜 GPA。
这仍是启动接入前的修正，没有 MemoryListener 授权入口、
没有物理 DMA 启动开关，运行巨虚仍是原 vIOMMU 路径；
不宣称当前带宽改善或原型已能启动，不改 guest 或 HPET。


### 2026-10-11：AdminQ 配置与硬件 enable 的独立门控组件

新增 hw/vfio/loom-iavf-admin.c 和 include/hw/vfio/loom-iavf-admin.h，
编入 VFIO。组件维护 ASQ/ARQ 各自的 LEN/BAL/BAH、完整配置位、
下一提交位置、实际 hardware-enabled 状态及不可恢复的 failed 状态。
当前仍没有 production caller、真实 prepare callback 或物理模式启动开关，
因此不是已接通的 VFIO 网卡路径，也不能据此宣称无 vIOMMU 模式能启动。

与未改动 iavf 驱动的 enable-before-base 顺序兼容：
LEN 写入先向真实硬件清除 enable；记录 guest 请求值；
只有 LEN/BAL/BAH 都到齐才调用 prepare(enabling=true)，
要求完成整个 ring、ARQ 全部接收缓冲的发现/授权/映射/同步，
随后写真实 enable，并读回核对 count/enable。
LEN/BAL/BAH 的六种顺序均可；活跃期间再次写任一配置寄存器，
即使该半地址值没有改变，也先 disable/读回并开始一轮完整配置，
避免把前一轮的高地址或 count 混入新地址。
此原型要求重配置提供完整 LEN/BAL/BAH，未实现单寄存器局部重配置。

head 目前只支持 native driver 的 clear0，会先关闭活跃队列再清 head；
初始/关闭时 tail0 不会重新 enable 未准备好的队列。
活跃 tail 先调用 prepare(enabling=false)：
ASQ [next,tail)；ARQ [next,tail]，均按 count 回绕；
所有依赖准备成功后才能写 tail，再更新 next。
prepare callback 的后续真实实现必须识别 ASQ 内嵌的 data queue 配置/
ENABLE_QUEUES 等进一步 ring/payload 地址，并拒绝未知协议，
不能把此 gate 当作已实现授权、CPU/pool 同步或 data queue 操作。

pending 配置的 LEN 读合并 guest 请求 count/enable 与硬件状态位；
运行中和 failed 时不合成 enable/count，保留实际硬件关闭/reset 的可见性，
避免 iavf_check_asq_alive 被虚构的 enable 欺骗。
write/read/prepare 任一步失败会标记 failed，尽力 disable 两个 AdminQ，
后续受管寄存器写入一律 EIO，不转发 tail 或新地址。
即使 enable write 已部分到达硬件但返回错误，也执行关闭。
callback 必须返回0或负errno，非零正返回也转换成错误，不能误当 handled 成功。
如果停止/读回失败，不能声称硬件已静止，caller 必须保留 DMA pool，
直至实际硬件访问停止或 backend close。
enable-bit readback 是 posted-write/config 校验，不证明所有在途 DMA 已 drain；
真实接入还需遵守旧请求完成/快照发布/重配置/回收的完整顺序。
state 只允许在确认 hardware reset/quiescence 后清零初始化。

memfd 编译当前实际 header 与完整 gate 源文件，外部 write/read/prepare 为模型；
GCC ASan/UBSan 41571 次断言 PASS，覆盖：
- ASQ/ARQ 六种 LEN/BAL/BAH 顺序，enable 前 prepare、读回、状态位；
- 4GiB以上地址、低/高地址半值重复、shutdown head/tail clear、回绕；
- 4096 次双队列配置事务和16384次 tail 提交的 hardware/prepare 模型核对；
- config各 write/prepare/read 失败、tail准备/写入失败、非法范围/协议；
- enable 部分写入错误、读回 count/enable 不符、无法关闭、两队列关闭；
- failed 不可恢复、运行中 hardware-reset 可见、callback错误正返回拒绝。
不是实际 DMA、真实 VFIO、完整 guest boot 或性能测试。
QEMU 编译与 git diff --check 通过；未生成实验文件，不改 guest/HPET，
运行巨虚仍保持原 vIOMMU 路径，Garen worker-g4 未动，不使用 Lux。
下一步实现真实 queue dependency prepare 和 BAR adapter，再接 RAM listener，
先做匹配配置的单机无 DSM 功能对照，再验证 Garen/Ezreal 巨虚。


### 2026-10-11：实现物理 AdminQ 的 DMA 依赖准备函数

新增 loom_iavf_prepare_physical_admin：需要已识别 X710 VF、合法稀疏 RAM
授权和已记录的相同 AdminQ base/count；否则 ENOTSUP/ESTALE，不猜地址。
此函数不写任何 BAR；未来 gate 的 prepare callback 可在返回0后启用/提交，
caller 必须先完成旧 device completion 的 publication/reconcile。
当前尚无 production caller/BAR adapter/RAM listener/启动开关，
运行巨虚仍使用原 vIOMMU 路径，不能据此声称物理模式已可启动或提升带宽。

ASQ enable 只初始化/同步整个 ring，不从未提交 descriptor 猜 payload。
ARQ enable 从授权 CPU GPA 读取全部 descriptor，处理 BUF/datalen/high-low 地址，
初始化/同步 payload 后再同步 ring。
ASQ tail 处理 [next,tail)，ARQ tail 处理 [next,tail]，均支持回绕与拆成两个范围。
使用有界 CPU read，ring 最多1023×32B；没有“全4GiB注册/扫描”替代依赖发现。
AdminQ payload 用普通 RAM 双向权限，即使该命令带 AQ_FLAG_RD，
也不能因上一次 TX 用途给设备保留不正确的只读 domain。

支持 VF 常用 direct AQ 版本/驱动版本/shutdown，及已明确无额外 DMA 地址的
virtchnl 基础 inline 控制消息；未知/高级协议、CONFIG_TX/RX 单队列格式等拒绝。
CONFIG_VSI_QUEUES 共用原 queue-layout parser，检查数量/对齐/溢出、
禁用 head writeback/header-split 等不支持格式，初始化/同步 TX/RX ring；
另拒绝 child ring 与 AdminQ、同一配置中任一其他 ring 的字节重叠。
此 prepare 不伪造 PF ACK 或修改 valid/pending；仍由实际 ACK 观察器确认。
ENABLE_QUEUES 要求已 ACK 的 valid mask、正确 VSI、无未完成状态，
拒绝重复 enable 已活跃的方向；先处理所有被启用 RX queue 的 payload，
再同步 RX/TX ring，最后同步 enable-message 和引用它的 ASQ descriptor。
RX buffer目前限定非 header-split、每 buffer≤4096B的原 native/scoped布局；
最多4096 RX slot 的读取分成≤64KiB片段，不超出 authorized CPU read 契约。
payload 不能覆盖已知 AdminQ/data descriptor；RAM hole/readonly/map/sync 错误
返回 gate，不能继续写 tail。
queue-transition op2/6/8/9/29 必须单 descriptor 提交，拒绝与其他命令混批，
保持原 ACK 观察器“一次一个状态转移”的关联约束。

单个 materialize 范围沿用已验证的失败回滚。
多依赖准备若后面的范围失败，前面已准备的授权页可能保留至失败模式退出；
不能宣称整个 callback 已全回滚，也不能释放可能被硬件访问的旧页。
gate 会停止后续提交、尽力关闭 AdminQ；真实 adapter/退出回收仍需验证
硬件停止与 snapshot/payload-before-completion 的完整顺序。
准备时 payload-before-descriptor 的同步不等于完整 device completion 实现。

memfd GCC ASan/UBSan 抽取当前实际 prepare/helpers、queue parser、
queues_get/observe_write/queues_invalidate、完整 gate 源码：
11923 次断言 PASS。RAM/授权/materialize/sync、BAR IO及部分 observer 外部回调模拟。
覆盖所有 count1..16 的 ASQ/ARQ next/tail组合（独立逐 slot walk oracle）、
空区间/回绕、4GiB以上 GPA、读取/map/readonly/sync失败、未知命令、
legacy额外 queue-pair slot、child ring重叠/非法布局、ACK/enable条件、
RX header-split/payload alias、4096 RX slot两次64KiB读取、
错误转换及真实 gate+observer+prepare 的 native enable-before-base组合。
组合 adapter 记录 original guest register 一次、内部 LEN gate 写入不覆盖请求，
确认第一个 pool 出现前记录配置、准备后才启用、tail推进双方 next，
以及 map失败不会硬件enable/后续提交。
另原 bootstrap 14项检查仍 PASS；QEMU 编译与 git diff --check 通过。
不是实际硬件 DMA、完整 guest boot 或性能测试；没有生成实验文件。
guest、HPET、现有巨虚和 Garen worker-g4保持，不使用 Lux。

下一步绑定真实 BAR adapter，并处理数据 doorbell 的新增 TX/RX payload 页。
只接 AdminQ 然后启用物理模式不够：后续 TX 新分配 buffer 仍可能尚无硬件映射；
因此必须在每次 data tail 前补齐授权/映射/同步，未知状态阻止转发。
全部接通后才加入 RAM listener/opt-in 开关并验证匹配配置单机与真实巨虚。


### 2026-10-11：RX 启动的未提交空槽兼容

进一步读取实际未改 guest iavf_main.c:1221 和 iavf_txrx.c:839：
初始化调用 iavf_alloc_rx_buffers(ring, IAVF_DESC_UNUSED(ring))，
分配后清 next_to_use 的状态，再写 RX tail；常规初始布局留一个未提交槽，
不是所有 rx_count descriptor 都有 packet address。
此前物理 ENABLE 准备要求每个槽非零，会误拒绝这种正常初始化。

现在只跳过所有四个 qword 都为0的槽，不为未提交空槽建立 GPA0 的 DMA 映射；
address为0但其他字段非零仍拒绝。有效 packet address 仍必须完全授权、
符合非 header-split 布局、避免覆盖描述符，payload仍先于ring同步。
本次不意味着 hardware tail 可以暴露空槽：真实 BAR adapter 尚未接入，
它必须验证 tail 暴露的每个槽非零且依赖已准备；初始化早期 RX tail
可能先于 PF CONFIG/ENABLE，需暂存并在地址明确/提交范围完整准备后转发，
不能因为这里容许未提交空槽就原样透传未知 data tail。
稀疏物理启动仍未开放，不宣称实际网卡功能已经通过。

实际 gate+observer+prepare 的 memfd ASan/UBSan 检查现11928断言PASS；
新增 N-1 缓冲正常布局及 null-address/dirty字段三个拒绝检查。
QEMU 编译与 diff --check通过；运行巨虚、guest、HPET保持不变，
没有实验文件，没有使用Lux。

### 2026-10-11：稀疏 data doorbell 与真实 BAR 适配器

loom_iavf_prepare_physical_data 只准备新提交的 TX [old,tail)、
RX [old,next_to_use)；按实际队列大小回绕，CPU 读取每段不超过64KiB。
TX data 描述符准备非空 payload，原生 TSO/VLAN context 没有外部
payload；其他类型、保留字段、描述符别名、越界/只读/未授权地址拒绝。
RX 只准备提交区间的非零 buffer，单独验证/同步 exclusive NTU 槽
已清零的 status，不把该槽保留的 RSS/writeback 地址当作新 DMA 地址。
payload 先于 ring 同步；任一步失败都不产生 prepared tail。

原生 iavf 可能在 CONFIG_VSI_QUEUES ACK 前写 RX tail。
这时只暂存最新 hint，绝不写硬件；TX 初始 tail0也不转发。
CONFIG 请求保留 hint，RESET/DISABLE/REQUEST_QUEUES 丢弃旧 hint；
配置失效清除 prepared tail。配置 ACK 后才能使用真实 ring/count，
不能把准备成功当成 PF ACK。已知配置不覆盖 hint queue 时拒绝提交。

新增 record_physical_data：在 doorbell 前记录完成跟踪需要的状态，
保留旧 tail/known；原有 observe_write 用于 legacy 路径，不能直接用于
新路径的提交位置。硬件 pwrite 完整成功后才 commit_physical_data；
错误值、未准备、配置失效、重复 commit 均拒绝。写失败不前移 next。

新增 hw/vfio/loom-iavf-bar.c，并在 helpers 的实际 BAR 读写路径接入：
仅当 bcontainer->loom_ram 存在时拦截，默认路径保持原行为。
确认 X710 VF/BAR0，拒绝关键寄存器的非32位/未对齐/部分重叠写入；
初次门控前读两个 AQ LEN，发现仍 enabled 或读取失败则拒绝。
先发布已有完成，再做 sparse prepare；内部 LEN disable/enable
不覆盖原始 guest 请求。CONFIG ACK 后、ASQ 命令 tail 前刷新 RX hints，
然后观察原始命令并写硬件。初始化 hint 不重复透传。
门控处理成功后跳过 legacy submission 与第二次 pwrite，仍执行 trace/EOI；
读取仅合并 pending LEN，请求/真实硬件 reset 状态按 gate 约定呈现。

每个 VFIORegion 保存独立 gate；finalize 只释放 gate 元数据。
短写、读写/DMA 准备失败使 gate failed，尽力关闭 AQ，停止 guest，
不把 disable readback 当作 DMA 已排空证明，不释放仍可能使用的 pool。
PCI pre_reset 的已有 gate 保持 failed：当前 post_reset 即使 reset
失败也会执行，因此未证明硬件静止前不能直接清空 gate 再恢复。
该原型一旦 PCI reset，需要关闭后台并重新启动 QEMU；fresh 初次启动
尚无 gate 不受影响。后续需要完成有成功证据的 reset/restart 生命周期。
物理模式仍没有 RAM listener 或启动开关，不能仅删除 guest vIOMMU
就启用；没有运行这条新硬件路径，不宣称吞吐提升或实际启动通过。

验证：memfd GCC ASan/UBSan 抽取当前实际 parser/prepare/data/record/commit、
observer request/write、完整 AdminQ gate 与完整生产 BAR adapter，
18547 次断言 PASS。外部 RAM授权/map/sync、VFIO fd 与 PF ACK 模拟；
scoped completion 的外部实现模拟，不覆盖真实 DMA/DSM 或硬件时序。
覆盖所有 RX32/TX8 old/tail 回绕组合、4095 RX slot 分段读取、
early hint/config ACK/ENABLE 顺序、完整六种 AQ 配置顺序、
部分重叠/短读/短写/启用状态/权限与同步失败、
实际 adapter payload→ring→hardware 顺序、失败 tail不前移、
pending LEN影子与真实 reset可见、默认禁用时无IO/无gate分配。
同时源文件 diff --check、QEMU 编译链接通过。
guest代码、HPET、现有巨虚和 Garen worker-g4 保持不变；不使用 Lux，
未生成实验脚本/日志/数据文件。

### 2026-10-11：接通 sparse RAM listener 与默认关闭的物理 DMA 启动入口

新增 hw/vfio/loom-physical.c，LOOM_VFIO_PHYSICAL_DMA 未设置或0走旧路径，
1选择物理 DMA 原型，其他值在 VFIO attach 前拒绝。新模式必须同时满足：
LOOM_VFIO_DSM_DMA=1使 staging 可用（单机控制还需 LOCAL_STAGE=1），
system-memory AddressSpace、4KiB host页、legacy VFIO、非mdev PCI设备；
有 guest vIOMMU 或 iommufd 时拒绝。读取真实 PCI配置后再确认
Intel X710 VF 8086:154c且 reset-capable，guest-visible ID覆盖不能绕过检查。
物理模式关闭 VFIO migration，不支持恢复未知 DMA 生命周期。

legacy container 在 listener replay 前记录 loom_physical。
普通 RAM 区域登记 guest 地址范围及 readonly权限，只授权覆盖完整4KiB页，
不在 replay 时硬件映射、不为整个4GiB域分配 pool。
地址取 offset_within_address_space；CPU读取仍通过 system AddressSpace，
不把 RAMBlock offset或跨区别名误当 guest GPA。
MMIO/RAMDevice区域排除；protected RAM、RamDiscardManager和gIOMMU拒绝。
部分页裁掉，错位alias、溢出/空区间拒绝；没有任何可授权RAM则初始化失败。
descriptor/doorbell的已有 sparse prepare 在提交时才建立必要的私有 pool，
注册/初始化/GPA关联完成后才让硬件映射可见，温热页沿用现有映射。

listener在物理模式跳过原本普通RAM的整域 dma_map/unmap/ref路径，
授权只保存地址/权限、不保存直接RAM指针，因此不额外引用 section。
初始化失败的注销尚无 DMA页面，保留授权元数据到 container销毁：
不能对未成功授权的section猜测remove范围，也不能错误地中断正常清理。
初始化若意外出现DMA页面则拒绝继续清理，避免未静止时释放。

运行时只要设备仍挂载且已有 materialized buffer，RAM region_del 在
修改授权/unmap/释放pool之前返回EBUSY，listener执行fatal error停止进程；
VM pause/BQL并不证明硬件静止，不能仅vm_stop后就回收。
暂不支持运行时RAM热移除。正常legacy detach从容器列表删除设备并关闭
device fd，最后group的listener才注销；此时撤销授权/硬件映射，
最后container fd关闭之后object finalize释放pool。
当前PCI reset仍保持旧gate failed，需关闭QEMU后fresh启动；尚未实现
有硬件成功/排空证据的reset恢复，这个限制没有被startup开关隐藏。

scripts/gvm-device-vfio-node.py在改绑VF之前验证新设置、
拒绝physical+legacy-dma-bridge组合，并检查真实sysfs X710 ID。
物理模式保留四CPU/4GiB/双NUMA和owner设备布局，
设置GVM_IOMMU/GVM_OWNER_IOMMU/LOOM_OWNER_IOMMU=0；
默认模式仍为1。guest内核不改，HPET不改。
新入口是显式物理地址DMA配置，不能在现有映射上动态切换环境变量。

后续待验证的命令构造是在原来sudo env前加入
LOOM_VFIO_PHYSICAL_DMA=1；原脚本的guest内核、IB地址、数据盘等保持。
匹配单机控制需删除 -device intel-iommu并设置
LOOM_VFIO_DSM_DMA=1 LOOM_VFIO_LOCAL_STAGE=1 LOOM_VFIO_PHYSICAL_DMA=1。
本次没有关闭/重启现有巨虚，没有启动该原型的真实硬件：
必须先正常退出旧QEMU/确认VF恢复，再验证单机及巨虚收发/错误/映射增长，
之后才能对比性能，不能以配置/模型测试宣称已经提升带宽。

验证：实际 listener/attach策略、sparse core、map_full/map_iommu及
registration保护函数抽取后，memfd ASan/UBSan共348908断言PASS；
外部MemoryRegion/Int128/硬件map与GPA注册模拟。
覆盖默认关闭/设置错误、地址空间/后端/页大小/设备类型保护，
RAM授权不映射、4GiB以上GPA、完整页裁剪、readonly/alias/溢出/hole，
初始失败清理、设备仍挂载拒绝删除且pool/映射保持、关闭设备后删除、
revoke失败poison，以及原4000步区间状态模型/首次注册/温热复用检查。
实际Python启动模式函数6项检查及全脚本AST语法PASS；
没有执行sysfs改绑或真实launch。QEMU编译链接与diff --check PASS。
guest、HPET、现有巨虚及Garen worker-g4保持，不使用Lux，不生成实验文件。

### 2026-10-11：物理 sparse DMA 首次真实启动与网络验证

代码38937e909a在真实Garen/Ezreal上启用LOOM_VFIO_PHYSICAL_DMA=1，
没有改guest内核：仍为6.12.0-vanilla-vfio #26；4vCPU/4GiB/双NUMA、HPET。
先正常poweroff旧巨虚，两端QEMU_EXIT0、VF恢复iavf；Garen worker-g4未动。
再启动Garen单机控制，正常退出并恢复VF后启动Garen master+Ezreal slave。
没有host重启，没有使用Lux，没有新增实验脚本/日志/磁盘文件。

单机和巨虚均可登录；guest原生iavf识别8086:154c。
巨虚独立enp0s7/enp0s8（Garen/Ezreal owner）都成功link up 10Gbps；
两端数据盘仍可枚举，但本轮没有重跑已接受的磁盘带宽。
两网卡配置combined2、RX512/TX128、RPS/XPS0；
Garen卡IRQ affinity0-1，Ezreal卡2-3。

发送测试沿用此前内存中的LOOMTCP数据校验程序：
每张卡2个worker，绑各自owner的两个CPU，每stream持续6秒，每配置3轮；
接收端Jinx br0对应198.18.10.3/11.3，port49161，校验每字节pattern
和实际接收字节数。单机Garen也用CPU0/1、相同VF/guest内核/HPET配置。
全部发送结果的接收字节数和内容正确；guest与Jinx记录的持续时间接近6秒，
没有依据异常启动printk时间戳计算吞吐。TCP retrans仍存在，本轮没有处理。

| 配置 | 三轮发送MiB/s | 中位数MiB/s |
| --- | --- | --- |
| Garen普通VM，新physical staging路径 | 1106.508 / 1107.471 / 1110.305 | 1107.471 |
| 巨虚Garen单卡，CPU0/1 | 709.497 / 625.299 / 801.246 | 709.497 |
| 巨虚Ezreal单卡，CPU2/3 | 589.658 / 578.309 / 583.004 | 583.004 |
| 巨虚双卡同时，每卡2worker，共4worker | 500.241 / 497.723 / 499.073 | 499.073 |

普通VM新路径约9.29Gbit/s，已经接近本次10GbE链路容量。
巨虚单卡明显高于此前旧启动结果（Garen约210–227MiB/s），但旧结果不是
同一源码新启动的配对对照，不能把历史比值当成严谨的独立优化效果。
需要在当前相同版本重测vIOMMU旧配置，才能排除重启/环境差异。
巨虚双卡同时反而低于单卡，目标尚未实现；合计未接近Jinx10GbE上限，
不能用接收链路容量解释这次下降。也没有证据把全部损失归因DSM，
仍需区分guest共享竞争、DSM、owner处理和接收端影响。

另外真实接收功能：Jinx port49162向两张卡并行发送，每卡256MiB，
guest分别CPU0/CPU2完整接收并逐字节校验；源端收到OK且字节数一致。
耗时Garen6.628秒/Ezreal6.475秒，源端计时6.621/6.467秒相符。
这是功能检查，不是完整RX性能benchmark；这次RX速度也偏低，
还没有匹配普通VM接收对照，不能宣称收发性能都优化完成。

测试后guest在线CPU0-3、clocksource hpet、error级dmesg为空；
两张卡ip -s link均errors/dropped为0，这不等于没有TCP重传或硬件层丢包。
Garen当前QEMU PID3981076，Ezreal733925保持运行，源码相同。
普通VM测试后QEMU_EXIT0且VF恢复iavf，退出/授权清理实际通过。
Jinx两个TCP sink和RX source均正常停止，临时IP aliases已删除；
没有残留监听49161/49162。Garen worker-g4 PID6122保持运行。

短测内存快照：普通VMRSS710072KiB、VmLck15380KiB；
巨虚GarenRSS4326844KiB/VmLck4208864KiB，
EzrealRSS4290932KiB/VmLck4202884KiB（包括巨虚本身锁定的4GiB RAM）。
不是长期映射增长证明，仍需检查长时间运行的页面/映射数量和回收上界。
后续先重测同版本旧配置作单卡对照，再低频采样双卡的剩余处理路径、
定位共享竞争；保持guest与HPET不改，不把新native线速视为巨虚目标完成。

### 2026-10-11：同版本回切对照、双卡CPU采样与IRQ配置审计

当前代码保持38937e909a（其后只更新README），正常关闭physical巨虚后，
同一二进制启用physical=0/vIOMMU=1；再正常关闭，回切physical=1/vIOMMU=0。
两端均QEMU_EXIT0并恢复VF到iavf后才重新启动，不是修改正在运行的环境。
guest仍#26、4CPU/4GiB/双NUMA/HPET，相同两队列/RX512/TX128/RPS/XPS0，
每卡两个owner CPU worker、6秒/轮、3轮，Jinx同一接收服务/IP配置。
本轮不改guest代码，不改HPET，不重测磁盘，不使用Lux。

| 配置（中位数MiB/s） | 首次physical | 同版本旧vIOMMU | 回切physical |
| --- | ---: | ---: | ---: |
| Garen单卡 | 709.497 | 209.199 | 788.340 |
| Ezreal单卡 | 583.004 | 174.367 | 426.904 |
| 双卡各2worker同时 | 499.073 | 175.351 | 277.759 |

旧配置三轮：G208.913/209.199/239.366，E151.514/174.367/181.017，
双卡148.067/184.419/175.351；回切新路径G788.340/809.771/760.020，
E532.045/409.917/426.904，双卡272.790/277.759/292.108。
所有发送数据校验正确，源/接收端计时相符；旧配置测试后error级dmesg为空。
因此单卡改善在同版本回切后重现，但E与双卡跨启动变化很大。
不能只报告第一次499，不能把3次短测当长期稳定收益。
双卡仍低于单卡，目标尚未达到，不能归因于Jinx10GbE容量上限。

在回切physical巨虚双卡26秒流量期间，同时采样两个QEMU进程：
perf6.8.12 cpu-clock:H 99Hz、DWARF8192、24秒、exclude_guest=1确认；
每端先确认/proc/PID/exe与build二进制inode相同，无旧二进制符号混用。
数据保存memfd，不写perf.data或buildid缓存，结束后memfd关闭。
采样时吞吐318.334MiB/s，字节内容正确；不是未采样的性能对照。

互斥分类优先DSM→VFIO→vIOMMU→RPC→OTHER，按sample period加权：
| 采样主机CPU时间占比 | Garen | Ezreal |
| --- | ---: | ---: |
| DSM调用栈 | 48.132% | 58.919% |
| VFIO（不含上述DSM样本） | 14.993% | 10.808% |
| RPC（不含上述两类） | 5.046% | 3.295% |
| OTHER | 31.829% | 26.977% |

Garen2061样本/20.818秒采样CPU权重，Ezreal2276样本/22.990秒，
解析bad block0、perf script stderr为空；未采到vtd软件处理栈。
inclusive VFIO为21.349%/14.763%，包含与DSM重叠的样本，不能与DSM相加。
Garen record报告瞬时线程3997146 open失败后忽略；并未独立核验所有lost events，
所以不宣称完整捕捉所有线程或零丢样。
这只反映被采QEMU线程的主机CPU消耗，不包括阻塞等待、guest CPU执行，
也不包含QEMU外的kernel worker，不能解释成吞吐损失48%/59%。

Garen实际调用栈提供下一步线索：
gvm-rdma-req → loom_pci_dispatch → vfio_region_write →
vfio_loom_sync_write → loom_vfio_dma_sync → address_space_read →
kvm_vm_ioctl_dsm → kvm_dsm_memcpy。
说明部分设备访问仍落在完整DMA同步兜底路径；其具体BAR地址/频率、
是否可以只发布AdminQ或已完成data范围，尚未核实，不盲目取消完成同步。
下一步对实际BAR访问与full-sync调用关联计数，再设计正确的缩小同步范围。

IRQ审计：同样写0-1掩码后，两个Garen data vector本次都实际投递CPU0；
Ezreal分别CPU2/3。因此owner CPU掩码不能保证各队列均匀分配。
临时精确绑定Gq0→0/Gq1→1/Eq0→2/Eq1→3后双卡三轮
313.894/306.924/290.876（中位306.924）；
恢复原请求掩码后240.270/255.739/291.486（中位255.739）。
但恢复后的计数显示Gq1仍实际投递CPU1，与初始CPU0不同，
故这不是有效的恢复对照，不能声称IRQ绑定带来20%收益。
末尾明确固定四个data vector到0/1/2/3，仅为后续控制配置可重复；
mailbox掩码保持原设置，没有声称吞吐收益。
guest两个PCI设备numa_node仍-1；iavf RX资源创建fq.nid为NUMA_NO_NODE。
这只是潜在分配位置线索，本轮未改设备NUMA配置或guest代码。

当前physical巨虚GarenPID3994428/Ezreal736529保持运行；
guest在线0-3、HPET、error级dmesg为空，Garen worker-g4 PID6122保持。
两个Jinx TCP接收服务均STOPLOOM正常退出，IP aliases清理、49161无监听；
两个perf进程及对应Python任务已正常结束，没有新增实验文件。
仍需解决双卡波动、完整DMA同步兜底开销，完成RX匹配对照/长期映射增长检查，
以及physical模式reset恢复；不标记目标完成。

### 2026-10-11：CPU offline 对照与完整 DMA 兜底路径成本核实

代码仍为38937e909a，guest #26、HPET、physical sparse DMA不变；本轮没有改guest代码。
先前“完整DMA同步兜底可能是主要吞吐瓶颈”的说法只是调用栈线索。
本轮关联实际BAR访问、完整sync调用及耗时后，不再将它作为首要优化目标。

CPU offline对照在同一次巨虚运行内完成：Garen owner0、guest CPU0/1固定两个worker，
CPU2/3按在线→下线→恢复顺序；每阶段分别测单卡与单盘，各三轮。
单卡enp0s7→Jinx198.18.10.3，两路TCP每轮6秒，接收端核验模式、字节数与elapsed。
单盘/dev/vda，serial GVM-DATA-OWNER0，既有未挂载1GiB raw，
O_DIRECT/libaio只读，1MiB、每worker QD4、2秒预热+8秒计数。
没有覆盖磁盘数据，没有生成实验文件。

| 三轮中位数 MiB/s | 全CPU在线（前） | 远端CPU2/3下线 | 全CPU恢复（后） |
| --- | ---: | ---: | ---: |
| Garen单卡TCP发送 | 507.447 | 835.005 | 692.896 |
| Garen单盘读取 | 1981.250 | 2005.250 | 1981.375 |

下线后NIC约7.005Gbit/s，高于前后在线组，但在线前后有明显波动，
不能把65%或20%作为稳定因果收益，也不能据此证明原因就是DSM。
磁盘约1.21%的差异小于本轮轮次波动，不视为实质收益。
本地NIC data IRQ36→CPU0/37→CPU1，vda实际使用IRQ27→CPU0/28→CPU1，
三阶段都保持本地，因此并不是只有offline阶段才把本地I/O完成中断移回本地。
offline不删除远端QEMU、设备owner或DSM。
CPU2/3及原请求IRQ掩码已恢复，所有阶段error级guest dmesg为空。

BAR/full-sync成本核实使用Garen当前QEMU PID3994428的BCC entry/return uprobes，
只记录内存内聚合表。DWARF验证VFIORegion.nr偏移48，实时exe与build inode匹配。
关联vfio_region_read/write的region/地址/方向与loom_vfio_dma_sync的方向。
各26秒窗口包含三轮6秒单卡或双卡发送及边缘空闲时间；采样吞吐不当作未采样对照。
4个异常计数（上下文溢出、full-sync嵌套、缺失return上下文、统计表容量失败）均为0，
所有full-sync return错误数0。这里只测Garen，不能外推Ezreal。
uprobes累计inclusive elapsed包含阻塞和探针开销，不能当作exclusive CPU或吞吐损失比例。

| Garen完整sync累计耗时 | 单卡窗口 | 双卡窗口 |
| --- | ---: | ---: |
| 26秒内所有方向/来源总和 | 258.491 ms | 256.474 ms |

高频data BAR写没有触发完整sync：单卡TX tail0/4共202889次，
data IRQ控制3800/3804共185536次；双卡Garen对应48661、70940次。
完整sync主要由管理寄存器读6400/8000/7400/6800等，
写5000/5c00/7000/8400，以及无BAR上下文的中断事件触发。
双卡没有使这些调用频率显著增加，故本次证据不支持“全量扫描爆炸”解释。
正确的控制/完成同步继续保留，尚未缩小AdminQ同步范围。

随后对Garen双卡26秒流量做24秒主机CPU采样：
perf6.8.12，cpu-clock:H 99Hz、DWARF8192，memfd、禁buildid缓存，
evlist确认exclude_guest=1、live inode匹配。
2430样本、24.545秒CPU权重，解析bad block0，script stderr为空。
互斥分类以DSM调用栈优先，DSM51.564%，VFIO13.621%，RPC4.321%，OTHER30.494%。
VFIO不含可见DSM调用栈的子分类（占全部采样主机CPU）：
scoped完成同步6.091%，physical提交准备4.486%，materialize1.193%，
完整sync0.700%，其余VFIO约1.152%。
generic ioctl入口等仍可能没有可见DSM帧，分类并非完美剥离所有DSM成本。
VFIO叶符号unknown约2.469%，整个窗口unknown约3.621%。
不包含guest CPU、阻塞等待、QEMU外kernel worker，也未独立验证零lost events。
该采样流量272.365MiB/s、所有接收内容/计数正确，仅为定位成本。

下一步优先检查physical_read_ring已验证描述符后，physical_stage又通过
sync_to_device_range重新读取同一CPU描述符的重复工作：
考虑复用调用者持有的不可变快照，先完整预检覆盖/方向/注册生命周期，
保留payload先于descriptor、所有准备先于真实tail写及成功后才commit tail。
这是候选方案，还没有实现或宣称性能收益。不能直接跳过完成发布。
仍需完成单卡非DSM成本优化、RX性能对照、长期映射增长及reset恢复验证。
本轮所有探针、perf和接收任务正常退出、Jinx两个临时IP已移除，
4个guest CPU在线、HPET不变；巨虚继续运行，Garen worker-g4未动，Lux未访问。

### 2026-10-11：复用已验证的 physical data 描述符快照

实现commit 5dac65683b（origin/device-aggregation），Ezreal修改并同步Garen，
两台QEMU编译通过；不改guest #26或HPET，不改host kernel。
新增loom_vfio_dma_submit_snapshot：BQL下完整覆盖与方向预检，
device-write-only direction2在修改任何pool/baseline前拒绝；
复用merge_buffer，仅合并CPU改变的字节，保留其他device更新及范围外baseline。
CPU/device-read-only direction1可提交；最后保留device写屏障。
调用者必须持有不可变私有快照，注册生命周期由BQL保护。

physical data preparation此前先physical_read_ring读取、验证描述符，
随后physical_stage又在sync_to_device_range中重新读取相同guest字节。
现在TX/RX的新提交[old,tail)复用submit_desc中的已验证快照，
包含ring回绕两段；RX exclusive NTU的+8状态复用已验证的8字节零快照。
materialize仍先完成完整注册/权限预检；所有payload仍先于descriptor，
真实tail写和成功后commit顺序不变。
AdminQ、未知操作及完成同步的保守路径保持。

验证不写实验文件，C harness与可执行文件使用memfd：
actual submit_snapshot+merge_buffer+skip_matching_bytes（mock外部pool/地址空间），
ASan/UBSan通过101466253断言：空/溢出/范围缺口、跨页、
后续页direction2不能导致先前页变化、direction1、短段及保留无关DMA变化。
actual AdminQ/data准备测试18417断言、actual gate/BAR适配器18552断言通过，
均保留原ring回绕、失败、tail未commit等测试。
新增读取后模拟guest描述符改变为非法类型的场景，
确认实际data准备函数提交的是读取时已验证的原快照，没有再次读取该descriptor。
硬件map/同步故障由harness注入；不能把这些mock断言当作所有硬件行为的证明。

旧巨虚guest正常poweroff，两端QEMU EXIT0且VF恢复iavf后，
用同配置启动新QEMU Garen4019166、Ezreal743223。
guest正常启动/login，仍6.12.0-vanilla-vfio #26、HPET、0-3在线、4GiB两NUMA；
每卡2queue RX512/TX128、RPS/XPS0，四个data vector精确绑定0/1/2/3。
Garen无关worker-g4 PID6122保持，Lux未访问。

新版本三轮6秒两worker/卡→Jinx，pattern/count/elapsed都通过：
| TX MiB/s | round0 | round1 | round2 | median |
| --- | ---: | ---: | ---: | ---: |
| Garen单卡 | 881.250 | 715.157 | 885.486 | 881.250 |
| Ezreal单卡 | 599.949 | 601.179 | 605.469 | 601.179 |
| 双卡四worker总和 | 469.558 | 534.951 | 445.586 | 469.558 |

与前一启动相比观测数字提高，但旧版本不同启动之间已经波动明显，
且本轮重启了QEMU，尚没有相同二进制snapshot OFF/ON/OFF严格对照。
不能把全部提升归因于此修改，也不宣称已线性扩展。
Jinx两个IP仍共用一条物理10G；当前dual吞吐低于链路上限。

并发RX每卡256MiB，CPU0/Garen耗时4.726859秒，
CPU2/Ezreal5.768540秒，内容/计数正确；
Jinx source对应4.719755/5.763526秒相符。
这是功能验证，仍缺相同native RX性能对照。
guest error级dmesg为空，两卡rx_errors/tx_errors均0。
末尾Garen卡rx_dropped/tx_dropped均0，Ezreal卡rx_dropped31/tx_dropped0，
未采每阶段前后差值，不能定位31个drop发生于配置、TX或RX，也不能宣称零丢包；
TCP重传存在，按既定要求暂不以重传作为当前优化任务。

新版本Garen24秒CPU采样（99Hz、DWARF8192、memfd、exclude_guest=1核实，
live inode核实）：2594样本/26.202秒CPU权重，bad block0/script stderr空。
DSM可见调用栈43.909%、VFIO不含上述DSM17.386%、RPC3.778%、OTHER34.927%。
VFIO子类占全部host CPU：完成同步8.712%、提交准备5.359%、
materialize0.925%、完整sync0.540%、其余1.850%。
采样并发26秒双卡438.422MiB/s，与旧采样272.365不同，
不能直接用前后占比变动声称性能收益/损失。
本窗口unknown5.050%（VFIO叶unknown3.200%），仅QEMU线程host CPU，
不包括guest CPU、阻塞等待及外部kernel worker，未独立核验零lost events。
完成函数+0x3c5的热点由当前binary DWARF定位到loom-iavf.c:906，
是每次同步检查所有TX描述符类型的循环。
下一步先验证descriptor快照收益的严格对照，再设计减少重复完成检查，
需要保证新提交验证、DMA写回、ring复用与失效的完整不变量；
不能直接删除该检查或跳过完成发布。
仍需解决单卡剩余成本、双卡非线性、RX性能及长期映射/reset验证。

接收服务、RX source与perf任务已正常结束，memfd关闭，Jinx临时IP清理；
新巨虚保持运行，未生成实验脚本、结果文件或新磁盘。

### 2026-10-11：四字节槽批量验证 TX snapshot 类型

实现commit 88d6c47ce9，origin/device-aggregation，Ezreal修改后同步Garen，
两端编译通过，guest/HPET/host kernel不变。
完成路径仍验证全部TX descriptor，允许0/1/15，非法类型仍回退完整同步。
新增tx_snapshot_types_valid每组四项只取little-endian command的最低类型字节，
打包为4个独立byte lane：types&0x0e检测不属于0/1，types^0x0f检测不是15；
各lane加0x7f后的最高位求交为非法项。
每lane值最多15，加法小于256，不跨lane进位；最后不足4项逐个验证。
没有取消任何检查、减少DMA发布或修改完成顺序。

微测试只反映该循环：Ezreal host CPU10固定，gcc -O2，128项0/1/15混合有效类型，
每轮一百万次，7轮oldcheck72.009–72.273ms、batch48.447–48.469ms，
中位数72.027ms→48.451ms（约32.7%减少）。
调用noinline，循环修改不参与dtype的字节避免不变调用被编译器提升；
不是guest流量/整体CPU或吞吐收益，也不能推断相同比例的端到端提升。
源码实际helper提取后ASan/UBSan通过：
四项全部65536种dtype组合、0–129长度的每位置16种类型、非零上半字节、
未对齐输入、4096项，以及每种短尾精确贴近PROT_NONE保护页，无越界。
测试源码/程序均在内存和memfd中，没有新增实验文件。

旧Garen4019166/Ezreal743223 guest正常poweroff，两端QEMU EXIT0，
VF恢复iavf后启动新Garen4028262/Ezreal745427。
新guest正常启动/login #26，4CPU在线、4GiB两NUMA、HPET不变；
每卡2queue RX512/TX128，RPS/XPS0，data vector按队列名称动态精确绑0/1/2/3，
本启动IRQ号与前启动可能不同（Garen36/38，Ezreal39/40），不硬编码旧IRQ号。

单卡与双卡各一次6秒功能发送（每卡2worker绑定owner CPU）→Jinx：
Garen663.582MiB/s、Ezreal593.068MiB/s、双卡426.355MiB/s，
内容、发送/接收计数及elapsed均通过。
每卡再接收256MiB：Garen5.929150秒、Ezreal8.068116秒，
source5.922353/8.062143秒，内容/计数正确。
只做新实现功能验证，未重复三轮或声称是新性能中位数，
不能与前启动881/601/470三轮中位数作有效因果对比。
末尾guest error级dmesg空，两卡rx_errors/tx_errors/rx_dropped/tx_dropped均0；
TCP重传仍存在，不宣称整个链路零丢包。
接收与source正常STOP退出、aliases清理、无实验文件；
Garen无关worker-g4保持，Lux未访问，没有host reboot。

这只降低已定位纯CPU热点，不能说明整个非DSM路径已最优。
下一步仍需严格验证descriptor snapshot与本批量检查的端到端收益，
并继续检查其他完成处理/复制/ioctl成本、RX原生对照、
长期physical映射增长及reset恢复。目标保持未完成。

### 2026-10-11：接收路径普通 VM 暂存/直接 DMA/恢复暂存对照

本轮代码仍88d6c47ce9，没有改guest、HPET或host kernel，也没有访问Lux。
先测当前巨虚Garen本地单卡，再正常poweroff两端，Garen用同一QEMU的普通
4本地CPU模式启动（无-local-cpu，无跨节点RDMA连接），不切换host KVM模块。
这不是upstream QEMU/KVM基线，是同代码移除跨节点因素的控制。
同一VF0c:02.0、同guest #26、4CPU/4GiB两NUMA、同kernel append、
无guest vIOMMU、两queue RX512/TX128/RPS/XPS0、data IRQ精确guest CPU0/1。
native与giant启动参数不完全相同：native显式禁两个ioeventfd属性，
giant保持既有aggregate设备配置；没有据此精确分解DSM代价。

负载为Jinx198.18.10.3→guest enp0s7，单连接/接收CPU0，
parent固定CPU0/1，每轮256MiB，3轮，pattern/字节数/source elapsed均正确。
TCP source每块8MiB，共32块，guest每次recv最多64KiB并检查字节模式。
不是长时间饱和线速或标准benchmark；固定字节模式也可能使baseline合并受益。
普通VM A→B→A，只改下面三个DMA开关，不改软件版本/IRQ模式：
A: LOOM_VFIO_PHYSICAL_DMA=1、LOOM_VFIO_DSM_DMA=1、LOOM_VFIO_LOCAL_STAGE=1，
保留软件DMA staging/完成发布，但所有CPU都在同一主机。
B: 上述三个开关均0，直接映射guest RAM作VFIO DMA。
每次切换均正常guest poweroff、QEMU EXIT0、VF恢复iavf后再新启动。
host线程未固定相同CPU，host RAM落点、guest启动状态也没有严格控制；
所以不能把A/B/A作为无混杂的固定损失比例证明。

| 单卡RX MiB/s | round0 | round1 | round2 | median |
| --- | ---: | ---: | ---: | ---: |
| 巨虚本地Garen单卡 | 119.390 | 126.448 | 122.167 | 122.167 |
| 普通VM，staging A | 323.290 | 265.443 | 265.333 | 265.443 |
| 普通VM，直接DMA B | 637.772 | 640.610 | 464.406 | 637.772 |
| 普通VM，恢复staging A | 352.532 | 360.552 | 275.501 | 352.532 |

直接DMA高于前后两次staging，虽然staging重启前后波动明显。
这改变优化优先级：RX不仅有跨节点开销，软件DMA暂存/完成发布在普通VM里
也有可观成本。不能用638-265简单归因于memcpy，或用265-122归因于DSM。
native阶段guest error级dmesg检查为空；A中出现hrtimer耗时warning，
没有调整HPET。现有FAT未正常卸载warning在各次启动仍出现，本轮未做fsck。

恢复staging后做12秒主机CPU采样：
perf6.8.12、cpu-clock:H 99Hz、DWARF8192、memfd、禁止buildid缓存，
实时binary inode匹配、evlist exclude_guest=1核实。
首次record失败且未给出错误字符串，任务已结束/memfd关闭；不使用该次证据。
同一VM重试成功，939样本/9.484848秒CPU权重，bad block0/script stderr空。
原打印limits继承旧24秒字样，实际record子命令sleep12，按12秒解释。
939样本的互斥分类：VFIO53.887%、OTHER46.006%、DSM名称栈0.106%。
唯一DSM名称样本是本地instruction-fetch经过kvm_dsm_release_page，
不表示有跨节点RDMA传输；不要宣称所有DSM命名函数都不存在。
VFIO分项（全部被采主机CPU的比例）：
完成同步34.611%、提交准备10.543%、materialize5.005%、完整sync0.852%。
unknown叶14.590%（VFIO内unknown13.206%），精度有限，
不含guest CPU/阻塞等待/外部kernel worker，也未独立检查零lost events。
采样流量与三轮性能数字分开，不拿采样期间吞吐作干净性能对照。

实际热点链：
flatview_translate → address_space_write → merge_buffer →
loom_vfio_dma_publish_snapshot → loom_iavf_sync_data_completion。
当前binary DWARF将+0x3f5/+0x41d定位loom-iavf.c:1032/1035，
对应RX descriptor metadata[0..7]与[9..31]发布；
每次按baseline差异拆成多个changed-byte run，每段重新做地址翻译/查表。
这是具体优化线索，不是证明整个34.6%都来自地址翻译或这些两处调用。

下一步优先研究减少RX完成发布的零碎写调用：
只能在证明active RX descriptor所有权、DD之前CPU不能复用该项、
queue/ring alias和generation失效检查成立后，合并owned metadata范围；
仍须保留payload与metadata先于最后DD发布、范围外CPU数据与baseline、
错误情况下不提前宣布完成。不能直接复制整页或取消DD顺序。
RX payload按已捕获packet length而非capacity复制也是候选，
但尚未核实完整硬件/driver所有权契约，当前没有实现或收益声明。
这比继续围绕完整DMA扫描微调更值得优先验证；整体目标仍未完成。

三次普通VM PID4033938/4036103/4038670均正常退出/VF恢复，
最后恢复巨虚：Garen4044085（session18296）、Ezreal748351（session57301）。
guest正常启动/login、两卡UP、两盘枚举配置保留、#26/HPET/4CPU在线；
data vectors按名称绑定0/1/2/3，error级guest dmesg为空。
Jinx source正常STOP退出、两个临时IP已删除、49162无监听，
perf/所有流量任务已结束、memfd关闭；没有实验文件或新磁盘。
Garen无关worker-g4 PID6122保持，没有host reboot。

## 2026-10-11：一次物理 DMA 发布内复用 RAM 地址翻译

代码 commit `8be831297b`。只修改 QEMU `hw/vfio/loom-dma.c`，
不改 guest/host kernel，不调整 HPET。针对前述 RX 完成 metadata 中多个
changed-byte run 重复调用 address_space_write/flatview_translate 的成本。

merge_buffer 的第一次 changed run 仍走旧路径；第二段起，只有
loom_physical、BQL 已持有且开关开启时，创建一次临时 MemoryRegionCache。
仅缓存完整覆盖当前 buffer 的普通可写 RAM；短映射、MMIO、RAMDevice 等回退。
缓存仅活到此次 merge 结束，持有 RCU/MemoryRegion/FlatView 引用，
不会保存在 DMA registration 中。错误出口也销毁缓存并解锁。
每段仍只写与 baseline 不同的字节，成功后才更新该段 baseline；
不扩大 metadata/payload 范围，不改变 payload→metadata→最后 DD 的顺序。
直接 RAM 写走 cache_invalidate，保留 dirty/TB accounting。
本树 address_space_cache_init 在 RDMA DSM 模式清空 cache.ptr，
因此 cached_slow 仍走 KVM_DSM_MEMCPY，不能绕过 DSM 获取裸指针写副本。

默认开启；对照时在 QEMU 进程环境设置
`LOOM_VFIO_PUBLICATION_CACHE=0` 关闭或 `=1` 开启。
gvm-device-vfio-node.py 外层 env 可传递该开关；容器创建时读取。
此前提交快照和 TX type batching 保持开启。

验证：
- Ezreal/Garen 编译通过、diff --check 通过。
- 抽取实际 skip_matching_bytes/merge_buffer，外部内存与 cache 接口用测试替身，
  memfd 编译 ASAN/UBSAN，43,200 案例 PASS。覆盖未对齐/0/短/4KiB范围、
  并发 CPU 改过但 producer 未变的字节保留、每段失败和 baseline部分提交、
  默认/关闭/非物理/BQL未持有/短映射/非RAM/模拟DSM慢路径/设备方向、
  cache引用与RCU在失败时释放。替身测试不证明真实内存接口本身正确。
- 真实普通VM关闭→开启→关闭，仅改变上述环境变量，使用同一新 binary；
  普通VM仍是本树 QEMU 的全本地4CPU模式，未换 upstream QEMU/KVM模块。
  guest #26、4GiB/两NUMA/HPET、X710 VF 0c:02.0、2队列、
  RX512/TX128、RPS/XPS0、接收CPU0、data IRQ按名称绑CPU0/1保持。
  三次正常poweroff/QEMU EXIT0、VF恢复iavf后才重启。
  Jinx198.18.10.3→enp0s7单连接，每轮256MiB，7轮，
  source8MiB×32、guest recv最多64KiB，固定字节模式/计数/source elapsed均正确。

| 模式，MiB/s | r0 | r1 | r2 | r3 | r4 | r5 | r6 | median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| OFF | 274.630 | 251.125 | 295.185 | 264.111 | 290.390 | 278.975 | 261.121 | 274.630 |
| ON | 279.705 | 338.512 | 353.683 | 323.002 | 321.323 | 367.657 | 359.902 | 338.512 |
| OFF_RESTORED | 298.742 | 317.501 | 254.172 | 344.843 | 260.636 | 345.582 | 288.876 | 298.742 |

开启中位数高于前后关闭组，方向一致；不能宣称固定13%或23%端到端收益。
host线程/内存落点、guest启动状态未严格锁定，关闭组自身也漂移；
固定模式/短测试不是长时间标准benchmark，不能与先前直接DMA组混算。
第一次关闭组执行器误把中途IRQ配置标记当完成，输出未完整采集；
确认 guest已回到root prompt后修正标记重跑，表格只使用完整7轮。
关闭组/恢复巨虚启动或关机仍出现hrtimer耗时warning，未改变HPET；
原有FAT warning也仍在，未做fsck。

最后恢复巨虚Garen4057385（session47327）、Ezreal753023（session70039），
同一代码/default cache开启。guest正常login、CPU0–3在线、HPET、
两NIC UP、两个既有1GiB数据盘枚举。
两卡同时各接收256MiB：G5.251914s/E6.268068s，数据均PASS。
发送功能每卡2workers×6秒，G591.068/E471.540/双卡总355.062MiB/s，
每种仅1轮，8条流接收计数/内容全部PASS，不能当作优化前后的性能收益。
TCP累计重传仍有，两NIC错误/丢包计数0、guest error级dmesg空。
首次发送的临时服务因exec在函数局部命名空间导致time/stop NameError，
服务断开连接；定位并结束这两个测试进程、确认端口关闭、修正执行命名空间后
重试成功。这次失败不作为设备路径故障或性能证据。

Jinx source/sink均已停止、两个临时IP删除、49161/49162无监听；
所有测试仅stdin/memfd，没有实验文件/新盘。Garen worker-g4 PID6122保持。
后续需进一步采样真实DSM路径的完成开销、验证缓存收益与退化情况，
并继续处理长时间DMA映射生命周期、reset恢复及其验证；
整体“除DSM以外开销尽可能低”目标仍未完成。

## 2026-10-11：地址缓存后的巨虚 RX 调用粒度审计

运行 binary 代码 `8be831297b`、README HEAD `8a8b8435a6`，
Garen4057385/Ezreal753023仍在运行；本轮未修改程序代码或内核。
上一轮属于实质进展（代码/测试/实机对照），本轮继续用新证据确定下一步。

同一 #26/HPET/4CPU/两NUMA，data IRQ仍0/1/2/3。
Jinx→Garen guest enp0s7，接收CPU0、parent0/1，单连接256MiB循环约28秒，
13次计数/模式验证PASS；source与guest计时相符。
perf仅采Garen QEMU主机CPU，cpu-clock:H 99Hz/DWARF8192，
memfd、no-buildid-cache、实时binary inode/evlist exclude_guest=1核实，
24秒record、2122样本/21.434343秒CPU权重、bad0/script stderr空。
启动相对traffic存在错位、尾部可能含空闲，不是严格稳态成本比例。
首次traffic拼接脚本多出一个括号，未产生有效负载；
确认guest返回root后结束专属串口执行器、修正并先compile验证。
首次perf任务正常结束但无合格负载，不用作此轮结果；表述只用后一次。

按原探针“函数名优先匹配”的互斥分类：
VFIO48.916%、DSM名称26.673%、OTHER23.327%、RPC1.084%，unknown叶6.880%。
VFIO分类分项占全部被采主机CPU：
完成同步37.936%、prepare6.079%、materialize2.026%、完整sync1.178%。
**这不表示37.936%是纯QEMU或完全排除了DSM开销。**
原分类仅识别kvm_dsm/ivy_kvm_dsm名称，不包括
kvm_vm_ioctl_dsm等封装，以及其外侧系统调用入口/退出。
真实栈中这些封装会落在VFIO类。不能把分类数字解释成逻辑上的
“DSM全部成本”和“所有非DSM成本”；也不含guestCPU/阻塞等待/外部kernel worker。
不能用与之前不同负载的百分比相减宣称cache优化收益。

关键新证据：完成发布仍频繁调用KVM_DSM_MEMCPY。
perf同时看到旧address_space_write与新的address_space_write_cached_slow，
后者确实经flatview_write_continue_step→kvm_vm_ioctl进入内核，
验证临时地址缓存并没有绕过真实DSM协议。
出现ioctl、syscall_enter_from_user_mode、mutex_lock/unlock、
copy_from_user等叶热点；地址缓存不减少changed-byte run数量。

另一次独立接收负载，BCC kprobe/kretprobe kvm_dsm_memcpy，
只过滤Garen QEMU tgid4057385，24秒计数，探针随后完全卸载。
参数按实际源码第4项length、第5项write，return非0记error。
没有BPF map dropped/pending，所有已统计条目error0。

| 统计项 | 值 |
| --- | ---: |
| write calls | 12,750,497 |
| write calls中长度≤8字节 | 12,739,338（99.912%） |
| write总bytes | 32,164,759 |
| 平均每次write bytes | 2.523 |
| 窗口平均write calls/s | 531,270.708 |
| read calls | 1,471,090 |
| read总bytes | 2,167,131,744 |

write各长度主要为：1字节4,960,003次、2字节3,933,388次、
3字节1,093,171次、4字节1,478,650次、8字节1,201,300次。
此计数含这个QEMU内所有DSM memcpy，不是只含RX descriptor；
可能包括packet header、TX ACK ring、其他RAM访问，尚未做逐发布来源归因。
不能声称每个packet/descriptor精确对应多少次调用。

kprobe明显扰动性能，采样时256MiB大多约3.36–3.47秒，
探针结束后的轮次约1.85–2.11秒；这两个数字不能用于报告系统带宽收益。
latency ns也包括等待、并发线程和探针成本，不能当CPU时间或简单累加成墙钟。
10轮流量内容/计数均PASS，探针目标是确定调用数量与粒度。

对源码的核实：每个KVM_DSM_MEMCPY重新copy参数、进SRCU，
查询hvaslot，对shared页加全局dsm_lock及page lock、检查/取得所有权、
执行copy并解锁。即使页已本地，也反复支付这条路径；
这比继续只减少地址翻译更具体。
下一步应评估批量提交同页的changed spans，而非复制整页：
保留变化字节与baseline部分提交、载荷先于metadata/最后DD，
同页所有权可在多个片段之间复用，发生错误须明确完成了哪些片段。
可以研究有界瞬时MEMPIN后直接改变化字节（现有unpin release路径），
或新增有界scatter-copy ioctl；**均未实现/部署，也未证明安全或收益**。
瞬时pin必须核实正在运行kernel的实现、限制单页避免部分pin回滚、
处理pin/unpin失败，不使用address_space_map当前忽略pin失败的接口。
scatter ioctl需要检查ABI能力、旧kernel回退、条目预检查与部分失败语义；
这些是host/QEMU侧改动，不要求guest OS变化。
不能把真正的RDMA page transfer、ownership等待等直接归入可消除的调用开销。

所有perf/BCC/traffic进程结束、memfd关闭、source STOP正常退出、
Jinx临时10.3/11.3 IP已删、49162无监听。
巨虚继续运行，guest error级dmesg空、CPU0–3/HPET保持，
Garen worker-g4 PID6122未动，没有host reboot或实验文件。
整体目标仍未完成，双卡线性吞吐、长时间mapping生命周期及reset恢复仍缺证据。

## 2026-10-11：同页碎片发布使用有界 DSM write pin

最终代码为 `edf6c75ec5`（候选 `03c30cd22e` 的正返回值判断已修正）。
只改 QEMU hw/vfio/loom-dma.c；未改 guest/host kernel、HPET。
针对上轮大量1–8字节KVM_DSM_MEMCPY，复用当前内核已有的MEMPIN接口。

默认关闭，需要显式 `LOOM_VFIO_PUBLICATION_PIN=1`，
同时保持 `LOOM_VFIO_PHYSICAL_DMA=1`、publication cache开启。
开关可通过启动wrapper外层sudo env传给QEMU；`=0`关闭。
容器创建时读取，不能当运行时开关。
只在BQL/RCU持有、完整可写普通RAM cache、RDMA DSM、host4KiB、
当前merge范围落在同一个host4KiB页内时使用。
第一次changed run仍走原路径；从第二段起至少还有4段变化，
才用pin/unpin两个ioctl替代这些片段的独立MEMCPY。
只写snapshot相对baseline变过的字节，不复制整页，
保持每段成功后的baseline更新、dirty/TB accounting；
所有直接写与dirty/baseline更新在release barrier/unpin前完成。
缓存直接指针只在pin成功后暂时提供，返回前清除、销毁缓存、释放RCU。
pin失败不做直接写；单页限制避免多页pin失败的部分回滚。
unpin失败会设置容器publication_failed，之后所有merge均失败；
既有VFIO错误路径停止VM，映射/暂存页保持，不能当正常完成继续。
不更改既有payload→metadata→最后DD字节发布顺序。
短片段/native/private布局等不满足条件时保留既有路径。

此实验依赖**atomic、无需DSM mutex的unpin实现**，不能只看kernel名字。
Garen运行vmlinux Build ID
d02a26705ec6d1a898f9d819d0e27ba508760a49，
与树中vmlinux ELF notes匹配；反汇编确认unpin分支原子减引用，
在独立pin路径的mutex_lock之前分支返回。
Ezreal运行KVM module Build ID
cf7c63a0013c211941cbbac133c5be12888c4b91，
sysfs module note与树中kvm.ko匹配、反汇编确认相同分支。
此处没有替换host模块或重启host。其他kernel应先核实，不默认开启。

首次候选版本暴露真实ABI问题：ivy fault写成功返回ACC_ALL访问掩码，
mempin安装pin后也会返回该正数（private页可返回0）。
不能把非0当失败；初版误判后master停在internal-error、pin未正常释放。
现场先查QMP/日志，slave仍running，master日志为VFIO DMA同步EIO；
在约kernel初始化1秒阶段暂停，没有运行性能测试。
两端通过QMP quit退出，EXIT0/VF恢复iavf，销毁旧KVM上下文后修正代码；
退出时master曾报RDMA transport retry，是对端先退出后的teardown消息。
修正版接受所有非负pin返回，成功后始终unpin，不继续复用旧context。
这次失败及修正前关闭组不计入下面同一修正版binary的正式对照。

验证实际helper/merge抽取、内存/cache/ioctl外部接口替身，memfd ASAN/UBSAN，
118,218案例PASS，随机案例一半用正掩码7、一半0，
另有专门正返回成功/释放案例。覆盖0/短/未对齐/4KiB、跨页回退、
fragment threshold/flags/non-DSM/shm/host page大小、pin负返回、
写失败仍释放、unpin失败sticky错误、baseline部分提交与CPU未变字节保留。
替身测试不能替代实际kernel一致性验证。Ezreal/Garen build与diff --check PASS。

同一最终binary，Garen master/Ezreal slave，PIN0→1→0，仅改该环境变量。
#26、4CPU在线、4GiB/两NUMA、HPET、两VF0c:02.0、
两队列/RX512/TX128/RPSXPS0、data IRQ按名称绑CPU0/1/2/3保持。
guest parent0/1，接收CPU0、enp0s7；Jinx10.3单连接256MiB×3轮，
source8MiB×32、recv最多64KiB，pattern/计数/source elapsed一致。

| 单卡RX MiB/s | r0 | r1 | r2 | median |
| --- | ---: | ---: | ---: | ---: |
| OFF | 131.535 | 130.424 | 122.264 | 130.424 |
| ON | 148.177 | 148.464 | 140.313 | 148.177 |
| OFF_RESTORED | 127.935 | 134.198 | 122.090 | 127.935 |

开启高于前后关闭组，本次中位数约高14–16%，仅为这组短测试。
host线程/内存落点、guest页分配/DSM状态、TCP4tuple/RSS等并未严格固定，
不能当稳定收益比例、纯syscall收益或标准长期benchmark结论。
第三组时Jinx source达到600秒正常EXIT0、aliases/listeners消失；
在场核实是流量服务退出，VM仍存活，未重启VM/host。
受影响3轮timeout/no-route全部舍弃；相同source重启、
guest只flush该测试目标的failed neighbor并ping确认后重测完整3轮。
表格只用重测PASS数据。连接/ARP服务恢复不计为带宽收益。
原有FAT warning/部分hrtimer耗时warning仍在，未fsck/未改时钟；
少量printk时间戳跨CPU前后跳动可见，测试固定CPU0并核实独立source elapsed，
不据此推出全系统时钟正确性。

开启组另验证两卡同时各接收256MiB，G4.448425s/E5.794540s，均PASS；
发送各卡2workers×6秒，每种1轮：G537.636/E700.041/双卡总315.548MiB/s，
8条流接收计数/内容PASS。此发送测试用于功能，
没有同一binary关闭/开启/关闭的TX性能对照，不能报TX收益。
双卡吞吐仍差，整体目标未完成；TCP重传仍有、两NIC错误/丢包计数0、
guest error级dmesg空。开启组正常guest poweroff、两QEMU EXIT0/VF恢复，
未出现pin残留导致关机卡住；不能据短测试证明所有长期/异常生命周期。

最后重新以PIN1启动已核实的两端：
Garen4089937（session18988）、Ezreal762540（session84767），
两进程environ核实PIN1、QMP均running、guest正常login/配置，
CPU0–3/HPET/#26、两NIC UP、两既有1GiB data disk枚举保持，
data IRQ36/37/39/40分别CPU0/1/2/3，error级dmesg空。
source/sinks已结束、Jinx临时IP删除、49161/49162无监听；
Garen worker-g4 PID6122未动，没有实验文件/新盘/host reboot。

下一步先核实真实调用数量减少多少，再评估把RX descriptor的有序
8字节metadata/23字节metadata/最后DD作为一次32字节范围内的发布，
在一个有界lease内仍只写changed spans并保持DD最后，减少剩余小调用。
不能扩大为整页复制或跳过ownership；该有序合并尚未实现。
长期DMA mapping容量与失效、reset恢复及性能验证仍待完成。

## 2026-10-11：碎片发布优化的真实 kernel 调用数量对照

本轮未修改功能代码，仍为 edf6c75ec5；README 同步后为 ac60305f9d。
先在已运行 PIN1 上采样，再正常 poweroff/两端 EXIT0，以 PIN0 启动相同
binary，重新配置同样的 NIC/IRQ 后采样，最后恢复 PIN1。
未改 guest/kernel/HPET；CPU0–3 保持在线。没有 Lux 测试或 host reboot。
只生成 stdin/memfd 中的探针与流量程序，不保存实验文件。

Garen owner 单卡接收 Jinx：CPU0/enp0s7，单连接 256MiB × 3，
两 NIC 2 queues、RX512/TX128、RPS/XPS0，data IRQ36/37/39/40→0/1/2/3。
两个组都验证全部 805,306,368 字节的内容、计数，并收到 source ACK。
BCC kprobe/kretprobe 只过滤对应 QEMU tgid，记录 kvm_dsm_memcpy 的
write/length 与 kvm_dsm_mempin 的 pin/unpin；正 access mask 不是错误。
进入/返回计数完整，两个组 errors=0、dropped=0、pending=0。

| 768MiB 流量窗口中的调用 | PIN0 | PIN1 |
| --- | ---: | ---: |
| write MEMCPY，length ≤8 | 5,721,725 | 2,840,235 |
| MEMPIN acquire，所有 length | 553 | 560,117 |
| MEMPIN release，所有 length | 553 | 560,117 |

PIN1 的 559,559 对 pin/unpin 落在 1025–2048 字节桶；16/32 字节等短范围
合计很少。因此现有优化主要覆盖 payload，不等于已合并 RX descriptor。
≤8 字节写复制减少50.36%；仅对表中三类调用求和，减少30.80%。
这不是所有 syscall 总数的变化，也不是纯非DSM CPU开销的测量。
全部 QEMU 活动纳入窗口，不能把所有短写归为 RX descriptor；
相同流量之外有 idle margins，窗口长度 PIN0 16.071s、PIN1 17.461s，
两个 boot 的内存分配/DSM 状态、TCP ACK/RSS 等未固定，不能推出精确因果收益。
ON→OFF 顺序只有各一个采样窗口，不能代替重复/随机对照。
探针本身增加每次入口/返回处理，ON 各轮2.765/2.815/3.196s、
OFF3.635/3.658/3.565s；这些速度不能混入上一节无探针带宽表。

代码核对：hw/vfio/loom-iavf.c 的 RX completion 当前仍分别发布
descriptor[0..7]、descriptor[9..31]，最后 wmb 与 descriptor[8] DD，
每个 publish_snapshot 独立进入 merge_buffer，故短 metadata 很难达到
当前“至少4段剩余变化”的 pin 门槛。潜在下一步是在同一个32字节、
单页且已验证可写的范围中共用 cache/有界 pin，仍仅写变化字节、
payload先完成、metadata先于DD、release在所有写之后；
须覆盖失败时部分baseline、dirty、unpin与停止VM语义。
此处只记录证据与候选设计，尚未实现该新API或报告其收益。
长期映射/失效/reset 恢复与双卡扩展目标仍未完成。

## 2026-10-11：RX descriptor 共用有界 lease，保持 DD 最后发布

代码 commit `65356823b0`，只改 QEMU 三个文件：
hw/vfio/loom-dma.c、hw/vfio/loom-iavf.c、include/hw/vfio/loom-dma.h。
guest/host kernel、HPET 没有修改，没有 Lux/磁盘重新测量或 host reboot。
新增 `LOOM_VFIO_ORDERED_PUBLICATION=1`，默认关闭，首次建立 container 时读取；
outer wrapper 的 sudo env 传给两端。PIN 仍需
`LOOM_VFIO_PUBLICATION_PIN=1`，并沿用上文已核实 atomic-unpin kernel。
`=0` 是对照路径，不能当运行时切换开关。

新 publish_rx_descriptor 接收 caller 已捕获的不可变 32-byte X710 writeback。
caller 已完成所有 payload 发布和 queue/lifetime/range 检查；
本函数仍按[0,8)、[9,32)、wmb、[8,9)写变化片段，
绝不复制整页/覆盖未变化的 CPU 字节。每段成功后更新 baseline 与 dirty/TB。
仅 physical/BQL/cache、32-byte aligned IOVA、一个 device-writable buffer 完整
覆盖时复用一次 RAM cache；完整可写普通 RAM、RCU 生命周期都检查。
跨 registration、不对齐、cache 不可用等使用原来的三次发布；
device-read-only 范围保持既有跳过行为。内核暂存映射/registration 不跨调用缓存。
同页至多32字节、至少3段变化时，复用现有 write pin/unpin。
无 PIN/native/shm/跨host页等仍采用 cache 原有访问方式，不强行提供直接指针。
pin 负返回不发布任何新字节；0/正访问掩码均接受并必须释放；
任一 cached write 失败停止后续范围，所以不会提前发布 DD。
unpin 仍有 release barrier、清 pointer、失败sticky；既有 VFIO 调用方停止 VM。
返回前销毁 cache、退出 RCU；没有 guest 修改或持久 DMA 直映 DSM RAM。

抽取实际生产函数进行 memfd ASAN/UBSAN 测试，外部 cache/ioctl/registration
为替身：有序路径108,018例、原有 fragmented pin 路径118,218例 PASS；
包括随机 producer/CPU 字节、0/2 writable direction、正mask7/private0、
每段写失败/partial baseline、DD时metadata已完成、dirty/释放顺序、
pin负返回、unpin sticky、native/flag/readonly/missing/overflow/短cache、
non-RAM/host page/shm/noKVM/不对齐/跨host页回退。实际常量也从源码抽取。
边界测试曾误要求“所有 unaligned 回退不得 pin”，断言失败；
既有 per-merge 路径允许在它自己的单页片段中 pin，因此修正测试期望：
跨host页新lease必须无pin，legacy回退只要求成功pin全部释放。
这不是新的生产代码故障。两端编译与 diff --check PASS。
这些替身测试不能证明真实长期 coherence/故障恢复。

同一最终 binary（每台各自保持），PIN1不变，仅ORDERED0→1→0。
Garen master/Ezreal slave、#26、4CPU在线、4GiB/两NUMA、HPET，
NIC 2 queues/RX512/TX128、RPS/XPS0，data IRQ按名称绑CPU0/1/2/3，
IRQ编号随boot变化，未硬编码。Garen enp0s7 RX CPU0、parent0/1，
Jinx10.3单连接256MiB×3、pattern/计数/source独立elapsed全部核实。
这张表无探针：

| RX MiB/s | r0 | r1 | r2 | median |
| --- | ---: | ---: | ---: | ---: |
| OFF | 157.431 | 158.671 | 153.888 | 157.431 |
| ON | 188.460 | 189.189 | 138.463 | 188.460 |
| OFF_RESTORED | 157.638 | 131.001 | 135.310 | 135.310 |

开启中位数高于前后关闭组，但boot内存/DSM分配、host线程、TCP4tuple/RSS
未固定；关闭组有波动、开启第三轮较慢。短固定pattern测试不等于稳态benchmark，
不能归因为固定比例、所有非DSM CPU开销下降或完成整体聚合目标。

额外对照在同一 binary 的 ON 与 OFF_RESTORED，各接收768MiB；
BCC只过滤对应QEMU tgid，kprobe/kretprobe count MEMCPY/MEMPIN，
同样的 uprobe/uretprobe count新API进入/返回。探针在所有这些组均开启，
计数完整、errors/dropped/pending/ordered_pending均0。
| 调用 | ORDERED0 | ORDERED1 |
| --- | ---: | ---: |
| write MEMCPY ≤8 bytes | 2845472 | 590531 |
| MEMPIN acquire 所有长度 | 559926 | 1119498 |
| MEMPIN release 所有长度 | 559926 | 1119498 |
| acquire length32 | 0 | 559627 |
| 新API返回 count | 559797 | 559541 |

小复制减少79.25%；仅前三行合计减少28.64%，不含 read memcpy/其他syscall。
32-byte acquire与新API count相近（还有其他32字节pin），结合源码说明
有序发布实际在运行，不能把全部QEMU短写归为该API。
流量字节数相等，窗口ON19.867s、OFF28.464s含idle margins，
背景QEMU活动/ACK包数等仍有差异，不能当精确无扰动cycle测量。
ON探针下三轮2.800/3.034/2.775s，OFF3.336/3.665/3.313s；
uprobe与kernel探针显著影响吞吐，不混入上面无探针表。

开启组双卡同时各收256MiB：G2.994096s/E4.387757s，计数/内容PASS。
TX各卡2workers×6秒、每种1轮：G684.055/E634.886/双卡总403.005MiB/s，
全部8条流内容/接收计数/独立时钟PASS。没有同一binary的TX开关性能对照，
只报功能，不能归因发送收益；双卡吞吐仍低、TCP重传仍有。
开启及回切组guest error级dmesg空，两NIC四项errors/drops均0，
CPU0–3/HPET、两既有1GiB raw data disk枚举正常。磁盘带宽不重测。
三组均正常guest poweroff，两端wrapper EXIT0并完成恢复原VF driver的finally路径；未见本次lease残留。
原FAT/hrtimer warnings与跨CPU printk时间戳跳动仍可见，未改时钟/fsck；
测试CPU固定、独立source elapsed核实，不推出全系统时钟正确性。
source/sinks已STOP/EXIT0，Jinx临时IP删除、49161/49162无监听；
探针cleanup/EXIT0，没有实验文件。最终恢复ORDERED1/PIN1，
启动状态与配置在下文核实。default仍off，长期mapping/失效/reset恢复、
fresh native对照与双卡扩展目标尚未完成。

最终运行核实：Garen QEMU4114115/session45943、Ezreal769218/session77461，
两端proc environ均PIN1/ORDERED1，QMP query-status均running；guest正常登录、
CPU0–3/HPET、两NIC UP/2queues512128/data IRQ36/37/39/40→0/1/2/3，
两既有1GiB盘枚举正常，error级dmesg与两NIC错误/丢包计数0。
Garen独立worker-g4 PID6122仍运行、未操作；临时流量源和接收端均已退出。
下一步定位剩余约59万次短写与CPU开销，并做同规格native对照；
不把双卡吞吐问题或长期映射/reset限制当已解决。

## 2026-10-11：剩余短写归因与提前 payload pin（暂不启用）

候选 `398ab02b19`、默认快速判断修正 `4b6bc5f6fb`；
仅 hw/vfio/loom-dma.c，guest/host kernel/HPET 均未改、未操作Lux或重新测磁盘。
上文 ordered descriptor 与 bounded pin 保持开启。
新 `LOOM_VFIO_PUBLICATION_EARLY_PIN=1` 默认关闭，
仅在 container 创建时读取，wrapper sudo env传入；0为旧发布策略。

先在上轮正在运行的PIN1/ORDERED1进程4114115采样：
uprobe/uretprobe跟踪 publish_snapshot 的 size，
kernel probe跟踪同线程进入/返回 kvm_dsm_memcpy 的≤8-byte write。
768MiB单卡RX，总592,327次短写，其中559,181在size1536的发布上下文，
占94.40%；该size的API调用559,222次，约每次payload一次。
size2048为TX ring等发布，不与RX payload混为一类。
errors/dropped/pending/context残留均0。实际代码确认first changed run独立
MEMCPY，second run以后才考虑cache/pin，因此不是仅凭栈名猜测。
这里的归因probe有额外开销，不作为带宽测量。

新候选在physical/BQL/cache/pin条件下，完整view≤4096、
GPA范围在一4KiB页、至少4段变化时，允许第一段就初始化cache并pin。
仍由既有acquire核实真实host页/普通可写RAM/DSM/非shared布局、
接受非负返回；只写变化字节、每段更新baseline/dirty、返回前unpin/RCU cleanup。
!locked确保成功初始化后第二段不会重复lock/cache/pin。
pin负返回在第一段之前失败，无部分新写；写失败仍释放，
unpin失败仍sticky并由VFIO调用方停止VM。unsupported/flag0按既有路径处理。
first-pin不会扩大为整页复制，DD发布仍由上一节有序API保证。
最终guard修正将runs/early开关判断前置，EARLY0第一段不查额外
physical/cache/BQL/page条件；只是等价gate重排，没有改变资格或写顺序。
ASAN/UBSAN抽取生产函数+外部接口替身：
merge/pin161,422例、有序发布108,018例PASS（guard后重验），两端build/diff check PASS。
测试覆盖同时开启/关闭early、CPU未变字节保留、每段失败/partialbaseline、
正返回7/0、第一段pin负返回、第一段写失败仍unpin、unpin sticky、
4KiB/跨页/短cache/readonly等回退。替身不证明所有真实长期/异常生命周期。

实际对照binary均为398ab02b19（gate重排前），EARLY0→1→0；
PIN1/ORDERED1固定，4CPU在线、4GiB/2NUMA、#26、HPET、
两NIC2queues512/128、RPS/XPS0、data IRQ按名称绑0/1/2/3。
Jinx10.3→G enp0s7 CPU0，单连接256MiB×3，计数/内容/独立source elapsed均PASS。
以下无探针，不能用gate修正后的boot替换这些数据：
| RX MiB/s | r0 | r1 | r2 | median |
| --- | ---: | ---: | ---: | ---: |
| OFF | 147.020 | 167.261 | 157.832 | 157.832 |
| ON | 160.953 | 136.398 | 138.533 | 138.533 |
| OFF_RESTORED | 150.040 | 142.496 | 171.835 | 150.040 |
ON低于前后OFF中位数，没有证明带宽改善，故最终**EARLY0**；
不能归因确定性能回退：host线程/内存落点、DSM状态、TCP4tuple/RSS未固定，
每组仅短3轮。候选保留用于诊断，不把小syscall减少当成性能收益。

同一候选binary ON/OFF_RESTORED各768MiB，使用相同的copy/pin/orderedAPI probes：
| 调用 | EARLY0 | EARLY1 |
| --- | ---: | ---: |
| write MEMCPY ≤8 | 582759 | 27532 |
| pin acquire | 1120780 | 1116154 |
| pin release | 1120780 | 1116154 |
errors/dropped/pending/ordered_pending均0，pin/release各自平衡。
前三行合计减少19.99%，只是这些调用数、不是CPU/全syscall/吞吐收益。
窗口ON22.423s/OFF47.363s有不同idle margins及background/ACK，
流量相同但不能认为全部其他状态一致；采样速度不并入无探针表。

EARLY1另做24s perf cpu-clock:H/DWARF、memfd、exclude_guest=1已核实，
2185 samples/22.071 sampled hostCPU seconds；guest重复RX19×256MiB约28s全部PASS，
窗口不同不能按全部19轮除出CPU/MiB。
named DSM body19.268%、VFIO_without_namedbody44.439%、OTHER35.103%、RPC1.190%；
VFIO_ioctl_boundary inclusive24.943%与DSM等重叠，
VFIO_without_namedbody含ioctl/SRCU/DSM wrapper，**不是纯非DSM CPU**。
叶子syscall_enter3.753%、mutex_unlock3.616%、mutex_lock2.838%、ioctl3.295%；
skip_matching_bytes仅约0.412%hostCPU；unknown8.696%仍需归因。
不能据此继续盲改字节扫描，也不把CPU采样比例当带宽损失比例。

开启组dualRX各256MiB G3.429530s/E5.260372s计数/内容PASS。
TX各卡2workers×6s每种1轮 G773.348/E554.185/双卡408.827MiB/s，
8流内容/计数/独立clock PASS，TCP重传仍有；不是TX开关对照或收益证明。
guest errors空、NIC errors/drops0、4CPU/HPET、两既有1GiB盘枚举正常。
OFF/ON/OFF_RESTORED均正常poweroff，两端wrapper EXIT0完成VF原driver恢复finally，
最终gate修正后重新以EARLY0/PIN1/ORDERED1启动；其启动配置核实如下。
source/sinksSTOP EXIT0、probes cleanup、memfd关闭，没有实验文件/新盘/host reboot。
FAT/hrtimer/跨CPU printk时间戳warning仍可见，未fsck/未动时钟；
不能把局部源端clock核实扩展成全系统clock正确性。
下一步先固定TCP四元组/核对RSS落队列，再做同规格native与hostCPU时间对照，
区分调度/内存位置/DSM等波动。可再用跨数据块变化的payload补内容校验。
未知叶子/长期mapping与失效/reset恢复、双卡扩展目标仍未完成。

最终4b6bc5f6fb运行核实：Garen4138637/session7977、Ezreal774756/session80525；
两端environ均PIN1/ORDERED1/EARLY0、QMP running。guest #26正常登录、
CPU0–3/HPET、两NIC UP/2queues512128，NIC IRQ37/38/39/40→2/0/3/1，
两1GiB盘枚举正常，error级dmesg空、NIC四项errors/drops均0。
Garen worker-g4 PID6122仍运行、未操作；Jinx只剩原管理IP、49161/49162无监听。
最终gate修正binary只做启动/配置核实，以上性能/流量对照均明确来自398ab02b19。

### 2026-10-11：接收描述符按页批量发布（实验开关）

`LOOM_VFIO_RX_PAGE_BATCH=1`（默认关闭）允许同一完成处理中的连续 RX
描述符共用一次短暂的 RAM translation / DSM write-pin。guest 不变，
不修改 HPET。依赖已有 `LOOM_VFIO_ORDERED_PUBLICATION=1`、
`LOOM_VFIO_PUBLICATION_PIN=1`，以及已经验证的 atomic-unpin host kernel。

- 调用者先完成所有选中 payload 的发布，随后把不可变的 32 字节描述符快照
  按连续、同 4KiB 页、同环区间拆分；单批最多 128 个，不跨 ring wrap。
- DMA 层还检查完整注册覆盖、device-writable direction、完整普通 RAM mapping、
  GPA 和真实 host pointer 的单页边界；指针不越过 BQL/RCU 或本次调用。
- 每个描述符仍只写与 baseline 不同的字节，metadata 两个区间先写，
  write barrier 后 DD byte 最后写；保留 dirty/TB accounting 和 baseline 更新。
  不合并整页写回，不覆盖 CPU 对未改变字节的修改。
- cache 子视图借用同一个 mapping/lease，只有父视图负责 destroy/unpin。
  pin 错误不降级为无所有权写入；unpin 错误保持 sticky failure 并由既有路径停机。
  不支持的布局和关闭开关均保留逐描述符发布。
- 编译通过；从真实生产函数抽取、memfd 编译执行的 ASAN/UBSAN 验证通过：
  原有 ordered RX 108018 cases + batch 14408 cases。覆盖随机 1–128 描述符、
  不覆盖 guest 未修改字节、每个 DD 的 metadata 顺序、逐写失败、
  pin 返回正值/零/负值、unpin sticky failure、缓存短映射、非 RAM、
  BQL/pin/ordered/batch 关闭、native RAM、错误参数及 dirty offset。
  这些测试使用 mock RAM/DSM，真实设备吞吐仍需另行验证。

修改前 Garen owner 实测：四个 guest CPU 在线、#26 guest kernel / HPET，
enp0s7 固定 RX 队列 1、IRQ CPU1、接收进程 CPU0，
Jinx 198.18.10.3:49162 -> guest 198.18.10.2:50042，
3 x 256MiB 全部 count/pattern 校验通过。
BCC uprobe 观察到 559019 个 RX 描述符，可分为 27896 个连续同页区间，
约 20.04 描述符/区间；90306 次 completion 调用中 24110 次非空，
2914 次只有一个描述符，单次最大 192、同页最大 128。
pending/dropped/outside/errors 均为 0。
该统计说明存在摊薄调用的机会；probe 本身影响时序，
不能把理论减少比例当成已实现的 ioctl 或吞吐提升。

#### 同一 QEMU 二进制的 OFF / ON / OFF / ON 验证

功能代码 `2fb129b5b1` 已在 Ezreal、Garen 编译。仅正常关闭/启动 giant guest，
没有 host reboot、host kernel/module 切换或 guest 源码修改；四个 guest CPU 均在线，
4GiB / 两 NUMA 节点、guest #26、HPET、两张 X710 VF 与两个现有 1GiB raw 后端不变。

每阶段接收 9 x 256MiB；固定 guest 198.18.10.2:50042 与 Jinx
198.18.10.3:49162，SO_REUSEADDR、RX RSS 全部导向队列 1，
IRQ q0/q1 -> CPU0/1，接收进程 CPU0，RPS/XPS 为 0，
RX/TX ring 512/128、两 queues。无 BCC/perf probe 的性能轮：

| RX page batch | 接收中位数 MiB/s | Garen owner host CPU ms/MiB |
| --- | ---: | ---: |
| OFF | 189.952 | 6.194 |
| ON | 213.665 | 5.378 |
| OFF 回测 | 172.070 | 6.706 |
| ON 再测 | 208.967 | 5.729 |

36 个 flow 的精确字节数和固定内容校验均通过，Jinx sender 也收到成功 ACK。
实际 per-flow 秒数（每行依次对应上表）：
```
1.34770664, 1.32799241, 1.38586588, 1.34412617, 1.26758769, 1.31594354, 1.36575653, 1.41490025, 1.40481190
1.20559569, 1.19813588, 1.15733405, 1.20997279, 1.19124968, 1.14217056, 1.23688604, 1.16616214, 1.28029926
1.38489367, 1.49469693, 1.48776230, 1.48340989, 1.49156951, 1.56066234, 1.47048406, 1.55277927, 1.45726843
1.19128794, 1.25143365, 1.22507261, 1.23664967, 1.26829366, 1.17857364, 1.16383369, 1.20808921, 1.24207535
```

CPU 来源是 QEMU 进程组 /proc/PID/stat 的
(user + system - guest) / CLK_TCK；内核 account_guest_time 同时增加
utime/gtime，故减去 guest time。四个阶段分别消耗 14.27、12.39、15.45、
13.20 host CPU 秒，采样窗口 12.748、11.347、13.875、11.407 秒。
这些是 NIC owner QEMU 的整体 host CPU，包含它的 DSM/VFIO/其它处理，
不包含 Ezreal slave 总成本或独立 kernel threads，也不能称为纯非 DSM 成本。
RSS key 每次启动变化，但所有 entries 均强制为 1；host 线程与内存落点仍可能变化，
所以当前结果支持优化方向，不保证固定提升比例或长时间稳定吞吐。

独立 BCC 计数轮（每阶段另收 3 x 256MiB，均校验通过；不作性能比较）：
- OFF pin/unpin 各 1117892 次，ON 各 584095 次，减少 47.750%。
- OFF 32-byte pin 558704 次，ON 1819 次；ON 增加的较长 pin
  包含按页分批的 RX 描述符，不能把“32-byte pin减少”直接当作总调用下降。
- 所有 kernel copy / pin / unpin 与被观测的单描述符 API 返回错误为 0，
  BPF dropped / pending / ordered_pending 均为 0，结束后 detach / cleanup。
- 小范围 payload 写仍然存在，OFF/ON <=8-byte write 579265 / 588620 次；
  payload read 约 559k 次、payload pin/unpin 仍占主要剩余调用。
  需要继续定位，不能把当前改动视为已经实现“只剩 DSM 开销”。

ON 状态下两卡并发接收各 256MiB 通过（Garen 2.691s，Ezreal 4.342s）；
最终 ON guest 又完成 G/E/DUAL 三种发送功能检查，各卡两 workers、每种 6 秒，
共 8 flow，Jinx 字节数和内容全部匹配。单轮 G/E/DUAL 分别
779.085/622.190/353.666 MiB/s，仅作功能记录，没有对应 OFF TX 对照。
Jinx 两目标地址共享一条 10GbE 链路，这不是双卡线性扩展验证。

四阶段硬件 rx_discards 末值分别 53/0/0/51；最终 enp0s7 rx_dropped=51、
enp0s8=0，两卡 rx_errors/tx_errors/tx_dropped=0，guest dmesg error 为空。
因此不能宣称已消除丢包或 TCP 重传；本轮未修改重传处理。
结束后 enp0s7 RSS equal 2 已恢复（0/1交替），四 CPU 在线、HPET、
IRQ q0/q1 local / remote 分布不变。两 host 当前保留
`LOOM_VFIO_RX_PAGE_BATCH=1`（代码默认仍关闭），PIN=1、ORDERED=1、EARLY=0。
测试 source/sinks 均收到 STOPLOOM 并 EXIT0，Jinx aliases 删除，未留实验文件。
Garen 无关 worker-g4 PID6122 未动；未使用 Lux，未重测已接受的磁盘读写。

### 2026-10-11：RX payload 同页双 buffer lease（默认关闭）

guest 不变（四 CPU、HPET、6.12.0-vanilla-vfio #26）。
本次先在同一次启动中做远端 CPU2/3 online/offline/online 对照，
固定本地任务 CPU0/1 与本地 NIC data IRQ。单卡到 Jinx 发送三轮中位数
938.942/937.159/809.148 MiB/s；固定 RX 队列的 256MiB 接收三轮中位数
243.355/255.418/236.650 MiB/s；既有 owner0 1GiB raw 的
O_DIRECT、1MiB、两个 worker、每 worker QD4 读取三轮中位数
2005.000/2007.125/1975.875 MiB/s。发送基线波动大，磁盘未见明显提升；
这些结果不能证明 DSM 因果关系，远端 CPU offline 也没有停止 slave QEMU/DSM。
CPU 与 IRQ 已恢复，无 guest dmesg error。未使用 Lux。

独立 BCC 上下文计数轮接收 3 x 256MiB，精确长度/内容/ACK 均通过：
- RX rearm 的 1536-byte sync_to_device_range：559120 次 CPU 回读，
  共 858808320 bytes。
- 1536-byte payload publish_snapshot：558644 次；
  对应 payload pin/unpin 各 558514 次。
- RX descriptor API 内 pin/unpin 各 26806 次。
- dropped/nested/pending/scopes/error 均为 0，探针已退出和清理。
- 另一独立页面邻接轮：559312 个 RX payload，283682 个同页连续组，
  同页最大连续组为 2；错误/丢失/残留上下文均为 0。
探针对吞吐有明显影响，以上仅用于归属和次数分析，elapsed ns 不是 CPU 时间。
host CPU 与内存固定 node0 是本轮控制条件，不代表已证明通用收益。

新增 LOOM_VFIO_RX_PAYLOAD_BATCH=1（默认 0），
入口 loom_vfio_dma_publish_rx_payload_pair：
只在同一有效注册、同一 4KiB IOVA/GPA/host page、
两个不重叠 RX payload 均有变化且普通 RAM/BQL/RCU 条件满足时，
共享一次临时写 pin。两份不可变快照先从设备 staging 读取，
然后逐个按原顺序只发布变化字节，分别 dirty/TB accounting 和 baseline 更新；
不写间隙，不覆盖未变化的 CPU 字节，所有 payload 仍先于任何 RX DD。
cache/pin 不可用、跨页、无变化等情况回退原逐 buffer 路径；
pin 错误立即返回，unpin 错误保留 sticky failure，
不在失败后发布描述符。不持久缓存 RAM pointer 或 ownership。
本次保留 rearm CPU 回读、AdminQ/未知操作全量回退与注册失效机制。

编译 PASS。memfd 中提取实际生产函数进行 ASAN+UBSAN：
已有 ordered RX 108018 cases、descriptor page batch 14408 cases、
新增 payload pair 16009 cases 均 PASS。新增随机测试含逆序同页地址、
CPU 未变化字节保护、buffer 间隙保护、逐变化区间失败注入、
关开关/非RAM/短映射/无BQL/无pin/native/host page 非4KiB/跨页回退，
pin 0/7 成功、negative pin 与 sticky unpin failure、参数错误。
硬件开关对照尚待完成，当前不能宣称吞吐收益或启用为默认。

### RX payload pair 硬件对照结果（功能版本 7f5ae3b103）

同一二进制 OFF -> ON -> OFF，各阶段 9 x 256MiB，无性能探针。
沿用固定 RX RSS q1 / IRQ CPU1 / receiver CPU0 / 固定端口50042，
四 guest CPU、4GiB、两 guest NUMA、#26、HPET 不变。
两 host 使用 numactl --cpunodebind=0 --membind=0；EARLY=1、PIN=1、
ORDERED=1、RX_PAGE_BATCH=1 在三个阶段相同，只改变 RX_PAYLOAD_BATCH。

| Payload pair | 接收中位数 MiB/s | owner QEMU host CPU ms/MiB |
| --- | ---: | ---: |
| off | 229.442 | 5.117 |
| on | 237.773 | 4.883 |
| off2 | 229.023 | 5.742 |

每轮秒数：
```
off: 1.12012898, 1.17239966, 1.15130861, 1.06583608, 1.06288816, 1.12273335, 1.03377890, 1.11575276, 1.05324854
on: 1.28188609, 0.99264670, 1.03177753, 1.05776991, 1.07665850, 1.09652984, 1.08015773, 1.08141297, 0.98645091
off2: 1.31861682, 1.11779183, 1.09523698, 1.11834208, 1.10785708, 1.11781450, 1.10423429, 1.17723276, 1.09862686
```
27 flows 精确长度、内容校验与 sender ACK 全部 PASS。
三个 owner QEMU host CPU 总秒数分别 11.79/11.25/13.23，计算仍采用
进程组 (utime+stime-gtime)/CLK_TCK，不能解释为纯 VFIO 或纯非 DSM CPU。
ON 吞吐比两个 OFF 中位数约高 3.6%/3.8%，CPU 消耗低约4.6%/15.0%；
CPU 基线有明显波动，只能视为小幅收益证据，未验证长时间稳定性。

独立 BCC 计数轮 ON/OFF 各接收3 x 256MiB校验通过：
总 pin/unpin 各313419/582792，ON 减少46.221%；
2048 bucket CPU read 各559440/558752，说明 rearm 回读仍未减少。
所有观测API errors、dropped、pending、ordered_pending为0，探针已cleanup。
ON/OFF观察窗口6.228/13.911秒，空闲边缘不同，不能比较ns总和或背景统计；
按已校验数据集的主要payload read及pin计数判断调用变化，不作CPU耗时比较。

ON时两张卡并发接收各256MiB内容校验通过，Garen2.508s/Ezreal4.030s，
该功能检查不是聚合带宽验证。三个性能阶段硬件rx_discards末值0/24/0，
不能宣称消除丢包。当前运行留在OFF回测：
Garen actual QEMU15059、Ezreal actual QEMU789956，
RX_PAYLOAD_BATCH=0，其余开关同上；保留默认关闭，后续可复测启用。
最终四CPU在线、HPET、两卡guest错误/丢弃统计为0、dmesg error为空，
RSS equal2恢复，中断配置正常。source收到STOPLOOM并EXIT0，aliases删除。
未产生实验文件，未使用Lux，未改guest源码；本轮除了CPU offline诊断，
未重跑已接受的磁盘聚合。下一步仍需降低rearm回读和逐变化区间处理成本。

### 2026-10-11：RX rearm 同页回读（实验开关，默认关闭）

既有 payload pair 降低 pin 次数，但 rearm 仍每 buffer 回读。
本轮在 prepare_physical_data 上独立 BCC 邻接观察：3 x 256MiB 校验通过，
559184 个 1536-byte buffer 可分为314541个连续同页组，最大组2；
34949个非空准备调用均包含16个buffer。dropped/errors=0，
outside=24（其它入口的同步）、pending=1（结束边界仍在途），
所以不是全局完整路径计数，探针已detach/cleanup。观测吞吐不能用作性能值。

新增 LOOM_VFIO_RX_REARM_BATCH=1（默认0），
loom_vfio_dma_sync_rx_pair 对同一有效注册、同一4KiB IOVA/GPA页面、
不重叠的两个RX buffer，在BQL/RCU下验证完整普通RAM映射，
一次读取包括间隙的有界span，再按原buffer顺序分别merge到staging。
间隙不提交、不改baseline，不改变device-only数据与未变化CPU字节的保护。
DSM cache保持ptr=NULL，仍通过address_space_read_cached的DSM读路径；
不直接访问未经授权的DSM RAM pointer，不跳过回读。
不可用/跨页/非RAM/短映射等回退原逐buffer同步；读取错误不提交快照，
部分staging写失败沿用原merge失败行为，BAR gate不能继续提交描述符/tail。
物理RX tail先验证全部描述符、payload与sentinel，再准备两range的注册。
TX、AdminQ与启动时RX全队列准备保持既有流程。

Ezreal编译PASS。memfd提取实际生产sync_rx_pair、sync_to_device_range、
sync_buffer、merge_buffer等函数做ASAN+UBSAN，18009 cases PASS：
随机1..2048byte及完整2KiB pair、正逆序地址、间隙/原device-only字节保护、
逐changed-run失败注入、开关关闭/非RAM/短映射/无BQL/非physical/
非indexed/跨页/native回退、整批读取失败与原逐buffer第二次读取失败、
参数错误与sticky failure。当前尚未验证硬件性能收益。

### RX rearm batch 硬件验证（功能版本 d2384d9eba）

同一binary OFF -> ON -> OFF，固定host numactl CPU/memory node0，
各9 x 256MiB，无性能探针；沿用固定guest端口50042、RSS全q1、IRQ CPU1、
receiver CPU0，四CPU、4GiB、两guest NUMA、#26、HPET不变。
EARLY/PIN/ORDERED/RX_PAGE_BATCH=1、RX_PAYLOAD_BATCH=0一致，
只切换RX_REARM_BATCH。27 flows精确长度、内容与sender ACK全PASS。

| Rearm batch | 接收中位数 MiB/s | owner QEMU host CPU ms/MiB |
| --- | ---: | ---: |
| off | 220.215 | 5.039 |
| on | 243.610 | 5.004 |
| off2 | 225.059 | 5.304 |

每轮秒数：
```
off: 1.16959157, 1.18126917, 1.11802360, 1.12037718, 1.17517417, 1.16250282, 1.13983047, 1.17590422, 1.14153388
on: 1.01563975, 1.01747408, 1.07195030, 1.05086042, 1.05522892, 1.03498411, 1.01861165, 1.05863193, 1.07826694
off2: 1.13747960, 1.12556712, 1.21230274, 1.26045865, 1.11559176, 1.14488293, 1.23112985, 1.07976969, 1.13443954
```

owner host CPU秒分别11.61/11.53/12.22，窗口10.862/10.015/10.884秒，
仍是进程组(user+system-guest)/CLK_TCK，不是纯VFIO或纯非DSM成本。
ON吞吐比两个OFF高约10.6%/8.2%，CPU低约0.7%/5.6%，
尚未证明长时间稳定收益，也未达到native普通VM接收吞吐。

独立BCC ON/OFF各3 x 256MiB计数轮都PASS：
OFF read2048bucket558976次/858587136bytes，read4096bucket4676次/19152896bytes；
ON read2048bucket69848次/107286528bytes，read4096bucket245203次/879183872bytes。
两个bucket合计读取调用563652 ->315051，减少44.105%；
字节合计877740032 ->986470400，增加12.387%，包括page gap及少量背景读取。
这不是跳过回读或减少同等比例数据搬运，完整注册、CPU owner、基线仍保留。
计数窗口OFF9.655/ON8.865秒，背景不同，bucket不是精确独占RX归属。
全部观测API errors/dropped/pending/ordered_pending为0，探针已cleanup；
elapsed_ns包含等待与探针成本，不作CPU或吞吐依据。

另一独立新API观察轮记录244517次size1536的sync_rx_pair，
以及244517次3584-byte CPU读取；结束errors/dropped/pending/contexts为0。
该上下文还出现34931次1536-byte及5次4096-byte读取，不将上下文集合
解释为独占/完整rearm来源；全量kernel bucket与代码路径一起判断变化。
probe throughput不计入性能对照。

ON时两卡同时收各256MiB，Garen3.018s/Ezreal4.458s，内容校验PASS，
不是聚合带宽或线性扩展验证。三个性能阶段hardware rx_discards末值0/7/0，
未消除TCP重传/丢包。结束留在OFF回测：Garen actualQEMU27523、
Ezreal actualQEMU793245，REARM_BATCH=0、PAYLOAD_BATCH=0，代码默认均关闭。
最终两卡guest rx_errors/rx_dropped/tx_errors/tx_dropped为0，dmesg error为空，
四CPU、HPET、两块原1GiB数据盘仍在，RSS equal2恢复。
source STOPLOOM / EXIT0、aliases删除、所有本轮探针退出，未产生实验文件。
未动Lux、guest源码和磁盘；下一步需检查逐changed-run处理与完成发布成本，
并验证两个同页优化组合，而非直接宣称只剩DSM额外开销。

### RX 两个同页优化组合及远端 CPU offline 对照（2026-10-11）

功能版本 d2384d9eba，同一 binary，RX_REARM_BATCH/RX_PAYLOAD_BATCH 同时
OFF -> ON -> OFF -> ON，各9 x 256MiB；性能轮不加探针。
固定host CPU/memory node0、guest四CPU/4GiB/两NUMA/#26/HPET，
receiver CPU0、RSS全q1、IRQ CPU1、端口50042。其他PAGE/EARLY/PIN/ORDERED均1。
第一次OFF沿用已有启动；后三阶段重新启动，uptime不同。

| 组合 | 接收中位数 MiB/s | owner host CPU ms/MiB |
| --- | ---: | ---: |
| off | 242.323 | 5.048 |
| on | 259.701 | 4.701 |
| off2 | 224.198 | 5.425 |
| on2 | 253.199 | 4.779 |

36 flows长度、内容、sender ACK全PASS。owner host CPU=(user+system-guest)/CLK_TCK，
不是整机或纯非DSM成本；OFF基线有波动，不宣称精确或长期稳定的提升。
每flow秒数：
```
off: 1.09384634, 1.09980687, 1.05644299, 1.12041736, 1.01797175, 1.04879838, 1.05395756, 1.12626601, 1.02766606
on: 1.16241642, 0.97753121, 0.98574720, 1.02884295, 1.03063891, 0.98160796, 0.96580648, 1.06443645, 0.97011530
off2: 1.36560704, 1.17015009, 1.18932120, 1.09523087, 1.23013156, 1.08221713, 1.11904657, 1.13707402, 1.14184684
on2: 0.96310990, 1.04397155, 1.01106311, 0.96838712, 1.08896009, 0.98002767, 0.96368242, 1.08714649, 1.07978479
```

独立24s、99Hz owner CPU perf采样（带探针轮不纳入带宽对比）：
2035有效samples，无bad sample。inclusive pair payload约11.06%、rearm pair约6.54%、
g_hash_table_lookup约4.42%、skip_matching_bytes约3.59%、invalidate_and_set_dirty约1.08%。
这些inclusive比例重叠，不能相加，也不代表等待时间；仅采样owner QEMU。
按栈中名称分类不能完整排除DSM；ioctl中可能包含DSM。未证明只剩DSM。
同页buffer首尾注册generation查询仍重复，是下一步可审查的优化点，本轮未改该代码。
ON2两卡单独/同时TX功能共8flows精确长度及内容校验PASS；无匹配OFF TX性能轮，
不作为组合优化的TX收益证据。performance阶段hardware rx_discards末值0/0/0/1，
未宣称消除丢包或TCP重传。

随后保持ON2同一次启动，guest CPU0/1固定本地任务；CPU2/3在线 -> offline -> 恢复在线。
确认lscpu node0=CPU0/1、node1=CPU2/3。本地Garen VF enp0s7 TX到Jinx，
2workers各6s、每phase3轮；读取Garen owner vda（原1GiB raw），
O_DIRECT/libaio、1MiB、QD4/worker、2workers、warm2s+measure8s、每phase3轮。
没有改guest源码，没有产生实验文件；该raw后端结果不等同真实物理盘极限。

| guest CPU | 单卡TX中位数 MiB/s | 本地单盘read中位数 MiB/s |
| --- | ---: | ---: |
| ONLINE_BEFORE | 805.593 | 1997.500 |
| REMOTE_OFFLINE | 1098.108 | 2022.250 |
| ONLINE_RESTORED | 722.091 | 1988.375 |

原始samples：
```
{
  "ONLINE_BEFORE": {
    "NIC": {
      "samples": [
        805.5931660525573,
        695.1546878472865,
        964.9407434821591
      ],
      "median": 805.5931660525573
    },
    "DISK": {
      "samples": [
        2000,
        1931.125,
        1997.5
      ],
      "median": 1997.5
    }
  },
  "REMOTE_OFFLINE": {
    "NIC": {
      "samples": [
        845.6008832878584,
        1098.1081024784962,
        1098.2044405695128
      ],
      "median": 1098.1081024784962
    },
    "DISK": {
      "samples": [
        2023.25,
        1958.5,
        2022.25
      ],
      "median": 2022.25
    }
  },
  "ONLINE_RESTORED": {
    "NIC": {
      "samples": [
        807.5847062946585,
        722.0907896340591,
        714.8652639826252
      ],
      "median": 722.0907896340591
    },
    "DISK": {
      "samples": [
        1988.375,
        1915.125,
        1991.125
      ],
      "median": 1988.375
    }
  }
}
```

此轮offline TX明显提高且恢复后回落；读盘仅约1.24%变化。
每phase只有3轮、TX端口/RSS流分布及TCP重传有变化，未同步采集DSM计数，
不能据此证明DSM因果或长期稳定收益。offline不关闭slave QEMU/设备owner/DSM，
也不禁止访问远端memory；不能用它替代设备聚合优化。
最后CPU0-3、IRQ原亲和性、RSS equal2均恢复，HPET不变，guest dmesg error为空。
Garen NIC guest rx_dropped=1（本轮开始已有1，结束仍1），其他NIC error/drop计数0。
两块原1GiB盘仍在。运行保留两个组合开关ON；代码默认仍均OFF。
Garen实际QEMU35567、Ezreal794998。Jinx source/sinks STOPLOOM后EXIT0，aliases清理。
未动Lux，未重启host，未改guest源码；本轮只有README更新。

### RX registration generation 同页查询复用（67315bb40b）

rx_slot_set/valid对同4KiB页的首尾地址在单次调用中复用generation；
仍校验两个保存endpoint，不保留跨调用pointer/cache，跨页仍独立查询。
valid补齐size/overflow防御；缺失、write-only/nonindexed返回0、unmap与重新注册
代次变化保持失效。底层page_generation只在page_indexed情况下返回代次，
buffer_at以iova/4096为key；现有序列化调用中同页两endpoint等价。

Ezreal/Garen编译与diff-check PASS。memfd提取生产函数和a2adc5481b旧函数做
ASAN+UBSAN差分264004 cases两端PASS：1..4096随机长度、跨页/同页、
高地址、stale首/尾代次、撤销/重新注册/不可用映射、非法长度与overflow。
正常同页set/valid各2->1查询；跨页保持2；未改变DMA/descriptor发布顺序。

实机各9x256MiB无探针，旧运行binary d2384d9eba -> 新binary 67315bb40b，
组合REARM/PAYLOAD/PAGE/EARLY/PIN/ORDERED=1；host CPU/memory node0，
四guest CPU/4GiB/两NUMA/#26/HPET，RSS全q1/IRQ CPU1/receiver CPU0/端口50042。

| 代码 | RX中位数 MiB/s | owner host CPU ms/MiB |
| --- | ---: | ---: |
| old | 262.099 | 4.484 |
| new | 251.094 | 4.870 |

未证明吞吐或CPU收益，本轮新代码读吞吐较低、CPU消耗较高。
旧运行经历之前CPU offline/restore且uptime较长，新运行重新启动；
启动时间/内存分配/RSS key有差异；只有一次跨启动对照，不能隔离代码因果。
保留正确且可证明少一次查询的改动，不能当作网卡已提速证据。
原始每flow秒数：
```
old: 0.94378758, 0.90328401, 0.96428568, 1.02464068, 1.00610260, 0.93085878, 0.97673097, 1.00468695, 1.00315521
new: 0.92323437, 0.91769624, 1.01953699, 1.02241172, 1.02625192, 1.04012225, 0.93074669, 1.06619572, 0.92135540
```
owner hostCPU seconds old 10.33/new 11.22，
(user+system-guest)/CLK_TCK；非全机、非纯非DSM。
18个单卡性能flow精确长度/内容/ACK全部PASS；
另两卡同时接收各256MiB，Garen4.399s/Ezreal4.959s，内容PASS，仅功能验证。

独立带BCC的3x256MiB轮全部PASS，owner进程：
2048/4096bucket读取合计315789次；write-pin 312202次，与unpin数一致；
全部observed errors/dropped/pending/ordered_pending=0，probe正常cleanup。
该窗口18.355s包含背景，bucket不精确独占RX；elapsed_ns包括等待与探针，
不用作性能/CPU证明。未做匹配旧代码计数，未归因于本次generation修改。
这验证组合优化仍生效，但每MiB仍有较多copy/pin ioctl边界，下一步应审查
调用批处理或完成扫描成本，保留DSM ownership与失效机制。

最终四CPU在线、HPET/RSS equal2、两原1GiB盘保持；
两卡guest error/drop计数0、dmesg error为空。
Garen实际QEMU46128（wrapper9747），Ezreal797466（wrapper33710），运行新代码。
Jinx source STOPLOOM/EXIT0、测试aliases删除，探针均退出。
未改guest源码、未动Lux/host内核、未创建实验文件；仅改QEMU源及README。

### 完成路径新采样及全量扫描定位（2026-10-11，功能67315bb40b）

两端工作树clean、运行actual QEMU Garen46128/Ezreal797466。
未改guest/HPET/host内核，组合优化ON；本轮未改功能代码或重启VM。
独立24s、99Hz owner hostCPU perf采样、18x256MiB全部内容/长度/ACK PASS。
2022有效samples、bad0。inclusive completion pair payload10.485%、
rearm pair8.012%、hash lookup3.956%、byte scan3.017%、
dirty invalidate1.039%、RAM materialize3.165%（分类不同、inclusive会重叠，不能相加）。
按stack name分类DSM named body14.985%、VFIO without namedbody34.817%、
OTHER49.159%；VFIO部分包含DSM wrapper，OTHER包含虚拟化/计时等，不能当成纯非DSM。
不以带perf测试的吞吐比较性能，也不把采样比例解释为全部带宽损失。

采样部分unknown libc栈来自snapshot_from_device <- loom_vfio_dma_sync
<- vfio_region_read/MSI等。于是另用短时uprobe核实：
第一次3x256MiB（内容PASS）窗口10.214s：
scoped completion正返回83921次，未记录零或负返回；
完整device->CPU sync中88次来自MMIO read，40次来自其他调用，
另8次完整CPU->device sync。不是数据队列频繁失败回退。
MMIO地址read数与其full-sync数一一相同：
0x4800=8、0x5000=8、0x6400=32、0x6800=8、
0x7400=8、0x8000=16、0x8800=8。
guest原iavf_register.h确认：ICR01/ICR0_ENA1、ATQH/ATQLEN、
ARQH/ARQLEN、VFGEN_RSTAT；iavf_flush读VFGEN_RSTAT。
这些AdminQ/状态/control操作仍需完成同步，不直接删掉。

第二次独立3x256MiB窗口7.861s：
scoped正返回83224次，snapshot_from_device进入265200次；
非direction1 buffer范围合计1086259200 bytes，无direction1字节。
这个计数是候选比较/拷贝范围，不等于已复制字节。

第三次独立3x256MiB窗口9.113s，在entry+return分别统计：
snapshot进入343364次、候选range1406418944 bytes；
成功返回且snapshot_ready为true仅101次/413696 bytes（404KiB）。
GDB读取正在运行相同binary的DWARF确认LoomDMABuffer
size偏移16、direction24、snapshot_ready56，sizeof96，避免猜offset。
helper源码在完整范围memcmp未变化时返回，不复制scratch；
ready=true表示该range完成了snapshot复制。范围计数仍不是CPU耗时或
精确memcmp读取字节量；probe开销、背景control频率和较小窗口不可忽略。
该轮scoped正返回79987次；read/snapshot active contexts均0；全部probe cleanup。

这给出下一步明确候选：将已知AdminQ及数据队列的完成发布分别限定到
对应已提交描述符和payload，以免control访问重复比较整个registration集合。
此设计尚未实现/验证。必须保留immutable snapshots、先payload后completion、
覆盖及generation检查、AdminQ异步event/reset/未知opcode/未知设备完整回退，
不得简单跳过completion、跳过DSM或按read地址粗暴关闭同步。
同页rearm pair还重复materialize同一页，可审查合并authority检查，但非主要结论。

所有27个接收flow内容、长度、ACK通过；未做新native基线或吞吐对照，
未证明达到只剩DSM开销。最终四CPU、HPET、RSS equal2恢复，
两卡error/drop计数0、dmesg error为空，两原1GiB盘保持。
Jinx source STOPLOOM/EXIT0、aliases删除，探针退出，未动Lux或创建实验文件。

### RX warm pair 注册检查合并（98971fc949）

新增loom_vfio_ram_materialize_pair，仅对同4KiB页、互不重叠的两个range、
现有page_indexed且已有pool mapping时，合并为一次materialize调用。
原materializer仍检查完整页RAM authority、readonly、map/buffer大小、
GPA、direction和pool identity；没有省略失效验证，gap不参与payload staging。
新入口多做一次map lookup，不能称所有lookup减少一半。
冷页、缺失map、非indexed、跨页/重叠/非法geometry保留两次scalar原顺序。

差分测试发现直接将两个cold调用合一会改变nonindexed布局的失败语义：
首调用创建mapping，第二调用可能拒绝不一致布局。已改为warm-only合并；
该初始方案未提交，实际98971fc949保留冷/unsupported路径。
Ezreal/Garenbuild及diff-check PASS；memfd提取实际production materializer、
pair API、iavf调用片段，与f263c6ec8c旧代码ASAN+UBSAN差分56000 cases：
随机正逆序/长度/间隙、高地址、缺失authority/readonly、
failed/materializing、nonindexed、陈旧size/GPA/direction/pool/read-only、
map失败、cold/warm、跨页/重叠/零长度，返回值/映射/失败状态相同。
正常warm pair scalar calls与authority检查2->1；cold failure order保持。

同规格新启动无探针9x256MiB内容/长度/ACK全PASS：
RX中位数267.485MiB/s，owner hostCPU 4.501ms/MiB，
hostCPU 10.37s，窗口9.198s。
四guest CPU/4GiB/两NUMA/#26/HPET、host CPU/memory node0、
RX RSS全q1/IRQ CPU1/receiver CPU0/固定50042，原组合开关ON。
```
0.98196559, 0.95706124, 0.87678632, 0.99330294, 0.88506078, 0.89354027, 1.00180123, 0.95242472, 1.01419801
```
没有匹配旧代码OFF回测，未证明本次改动吞吐/CPU收益，未达到native性能。
该9flow阶段hardware rx_discards从0->519；不能称丢包已消除，
也没有证据将增加归因于这项改动。

独立带uprobe3x256MiB全PASS：244530次pair仅一个materialize子调用；
另1次pair保留两个子调用。没有非零返回组，pending/dropped0，
窗口15.571s包含背景和probe开销，不用作带宽或CPU依据。
两卡同时收各256MiB也PASS，Garen3.351s/Ezreal4.049s，仅功能证据。
最终四CPU在线、HPET、RSS equal2，Garenrx_dropped519，
其他NIC error/drop计数0，guest dmesg error为空，两原1GiB盘保留。
实际QEMU Garen64221（wrapper86842）/Ezreal802030（wrapper27714）。
Jinx source STOPLOOM/EXIT0及aliases清理，probe cleanup；
未改guest或host内核、未动Lux、未创建实验文件。

### AdminQ scoped completion 必须按硬件head设计（尚未实施）

guest现有iavf_adminq.c:598的iavf_asq_done读取VF_ATQH1并比较next_to_use，
注释明确head比DD timing更可靠；ASQ command head完成后直接复制descriptor
及buff_size bytes。ARQ clean_element读取VF_ARQH1后直接消费descriptor和payload，
用完成descriptor datalen作为有效msg长度，再恢复原bi->size/address并写ARQT1。
因此仅按AQ DD选择完成项会漏发布guest已允许消费的数据；不能套用RX DD逻辑。

下一实现应先采样硬件AQ heads，再从有明确代次/初始化状态的已发布游标
到采样head确定完成窗口；pread(head)必须先于immutable descriptor/payload捕获，
全部依赖捕获/验证完再发布，guest看到head前须完成对应内容发布。
队列wrap、posted tail、cold seed、reset、重新配置、撤销及alias需要显式处理；
设备稍后推进不能让未捕获payload的新descriptor被重新采样发布。
ASQ使用posted capacity覆盖driver buff_size消费语义，ARQ保留posted容量与
writeback datalen的关系。AQ descriptor/payload和数据队列别名需拒绝或统一捕获，
AdminQ event/reset/未知opcode/布局/失效保留完整同步。
当前ASQ next只跟踪提交观察、ARQ next只跟踪控制observer，
不等于可直接复用的完整发布head cursor。控制路径仍使用原full同步。
这仍是未完成项，不能宣称只剩DSM成本或全量扫描已消除。

### AdminQ scoped completion 原型（默认关闭，待实机验证）

LOOM_IAVF_ADMIN_SCOPED=1 启用；默认关闭，guest/HPET不改。
已知BAR0四字节control reads（4800/5000/6400/6800/7400/8000/8800）
在硬件pread之后尝试AdminQ+data completion；控制vector0 IRQ同样尝试。
未知read/write保留原路径。返回0表示在任何新AQ或data发布前回退full；
发布开始后的同步错误为负，停止VM，不再重新采样/回退。

本阶段捕获完整ASQ/ARQ ring和每个已声明buffer，不按DD筛选，
也不把已有asq_next/arq_next当发布游标。覆盖原生按hardware head消费的语义：
ASQ按baseline原capacity捕获，ARQ保留posted capacity与firmware datalen校验。
每次控制同步有短期分配，后续可按head窗口缩减，尚未证明CPU/吞吐收益。

启动/未配置/队列pending/unknown布局/共享container走full。
只允许现有普通RAM、writable identity mapping；新增只读验证API
loom_vfio_ram_identity_range校验authority、poison/materializing、nativepage、
map/buffer/GPA/permission/pool一致性，不创建map、不保留指针。
预先收集数据descriptor及活跃RX/TX payload范围，排序合并允许data域内部重叠。
拒绝AQ ring/ring、AQ/ring-data、AQ payload/descriptor、AQ payload/data、
AQ payload/payload别名；拒绝新配置/enable ACK和reset事件，以免错误沿用data快照。
所有AQ descriptor和payload先捕获；再执行既有data completion；
再发布全部AQ payload、全部metadata字节2..31、barrier、最后全部flags字节0..1。
捕获/检查错误返回0且不发布；data或AQ发布失败不当成fallback。

Ezrealbuild PASS。memfd提取production control function/extent/tx-validation，
ASAN+UBSAN85050 cases PASS：默认OFF、DD0/1、1..4096 capacities与短/零writeback
datalen、live pool在data completion期间改变的immutable snapshot测试，
正常direct ASQ、unknown/pending/disabled/zero/large rings、RX/TX/descriptor/AQ别名、
bad identity、unknown opcode、queue ACK/reset/malformed event、
全部11个fixture read失败点、10个发布失败点、data fallback/error、
80000个随机merged-extent overlap差分查询。publisher/data组件stub用于控制顺序和错误注入；
不是完整硬件/DSM等价证明。新增identity真实函数20个authority/map/GPA/readonly/
poison/overflow/边界cases与现有warm pair56000回归一起ASAN/UBSAN PASS。
实机启用、控制操作、性能和scan counts仍待验证，默认不启用。


### AdminQ 原型实机初验（2026-10-11，functional b486e282c6）

Ezreal/Garen 同源、clean，production控制测试在BQL fail-closed修正后
ASAN+UBSAN 85051 cases PASS BOTH；identity真实函数+warm回归56020 PASS BOTH。
未改guest源代码、host内核、HPET。先前“尚未实施”和“待实机验证”为历史记录，
本节记录当前实现与实机结果；按head窗口进一步缩减仍未实施。

同一二进制、host CPU/memory bind0、guest #26四核4GiB/两NUMA，
两VF、两原1GiB数据盘；Garen单卡RSS所有流固定queue1，worker CPU0，
owner IRQ queue0/1=CPU0/1、远端=CPU2/3。跨启动仅AdminQ开关OFF→ON。
无探针9×256MiB接收均验证内容、长度和ACK：
OFF median 264.550 MiB/s，owner CPU 10.82s /
4.696181 ms/MiB；
ON median 276.711 MiB/s，owner CPU 10.6s /
4.600694 ms/MiB。
吞吐中位数约+4.60%，CPU约-2.03%；只有一对跨启动、样本重叠，
RSS key每boot重新生成（已固定queue），不能宣称稳定收益或普通VM性能已追平。
CPU=/proc/<actual-qemu>/stat (user+system-guest)/HZ，窗口含串口启动边缘；
不是纯VFIO或DSM成本。OFF/ON该9轮rx_discards都0。

独立uprobe三轮接收，内容PASS（带probe速度不作性能基线）：
OFF已知MMIO read对应full from-device共88次，其他from-device40、to-device8；
control ret0=96，data retpositive=80590。
ON已知read full=0，其他from-device36、to-device9；
control retpositive=98、ret0=2、negative0，data retpositive85752。
OFF full-sync snapshot candidates 290624页/1190395904 bytes，
其中snapshot_ready实际copy95页/389120 bytes；
ON candidates151240页/619479040 bytes，copy7页/28672 bytes。
这些计数只覆盖full同步，不包含新AQ/data scoped capture/publish；
候选bytes不是实际copy、更不是CPU成本或可归因吞吐损失。
两个窗口9.314/10.314s含背景；readonly探针detach cleanup、
active read/snapshot均0。不能因候选量减少就声称全部非DSM开销降低同样比例。

最终ON同次运行的caller探针（两卡同时收各256MiB，内容PASS）：
Garen16次非read from-device full均定位到两直接返回地址：
vfio_loom_sync.constprop.0+0x14 8次；
loom_iavf_physical_bar_write+0x362 8次。
frame-pointer用户栈不完整，仅直接caller有确定证据。
源码核实physical_bar_write非data控制写先full completion、
reconcile/observe再AdminQ prepare/write；helpers非scoped写也保留full。
下一步应记录确切BAR地址及控制阶段，验证安全的提交/完成依赖边界，
再考虑控制写限定范围；不可直接跳过full或复用过期发布快照。

ON两卡同时RX各256MiB PASS，G3.451s/E4.072s（probe期间，仅功能）。
TX G/E/DUAL各一轮6s，8条连接内容/长度PASS，
805.930/606.268/425.956 MiB/s；没有对应OFF对照，仅功能证据，
Jinx两测试IP共用一个10G物理口，不能用这组数字证明线性双卡扩展。
final guest CPU0-3、HPET、RSS equal2，两盘仍各1GiB；
两卡rx/tx errors/dropped0，error级dmesg空。
当前actual QEMU Garen101450(wrapper91087)/Ezreal809236(wrapper17412)，
LOOM_IAVF_ADMIN_SCOPED=1仅本次显式启用，source默认OFF。
所有探针终止cleanup，Jinx sink/source STOPLOOM/EXIT0及aliases移除；
未动Lux、worker-g4，未创建实验文件。目标仍未完成。


### Native AdminQ/control writes 限定同步原型（默认关闭）

新增 LOOM_IAVF_ADMIN_WRITE_SCOPED=1，且需要原
LOOM_IAVF_ADMIN_SCOPED=1使control completion有效。
仅native PCI X710 VF、BAR0、4-byte、5000/5c00/7000/8400；
新API loom_iavf_sync_control_write复用已有AQ+data immutable capture、
identity/alias/queue状态验证和fatal publication语义。
物理ARQT/ATQT仍在completion后走原prepare/observe/admin write gate；
startup、LEN/BASE/HEAD/reset/未知写不扩展，ret0仍完整fallback。
helpers仅在5000成功scoped completion之后跳过原full CPU→device：
guest iavf register/main确认这是ICR0_ENA/AdminQ interrupt mask，不提交DMA地址；
scoped失败/关闭时原full completion与full submit保持不变。
5c00原本已不提交DMA，但旧路径completion使用full，现在可scope。

真实运行baseline三轮RX PASS：full writes5000 from13/to13，
5c00 from13、7000 from8、8400 from8；另2次MSI fallback；
总非read from44/to13，probe12.368s，非性能证据。
原生产API与helpers full函数/physical_bar_write提取编译ASAN+UBSAN，
884762 cases每模式OFF/ON/invalid均PASS：
地址/大小/BAR/type/irq全矩阵、success/fallback/error、
物理tail仍经prepare gate入口、启动配置full、data completion不变、
full失败/控制发布失败/提交失败停止、失败状态不再提交。
外部DMA、AdminQ gate本体用stub验证调用顺序及错误，不是完整硬件等价证明。
Ezreal build PASS。实机开启/失效/性能验证待后续本节记录；source默认OFF。


### Native control writes 实机验证与保留关闭（2026-10-11）

functional 0f135beaf0；b8753a3269补充scoped完成后原AdminQ observer顺序。
修正后OFF/ON/invalid每模式884762 cases ASAN+UBSAN PASS BOTH，build PASS BOTH。
0f135版本的早期OFF基线与未修正ON启动仅作调试，不纳入最终性能对照；
最终同二进制b8753a3269，仅WRITE_SCOPED=0→1，原ADMIN_SCOPED=1保持。
guest/host内核、四CPU、4GiB、HPET、host bind0、两VF与原两1GiB盘不改。

Garen单卡、CPU0、RSS allqueue1、9×256MiB无probe内容/长度/ACK均PASS：
OFF median 269.650 MiB/s，owner host CPU 10.31s /
4.474826 ms/MiB；
ON median 267.644 MiB/s，owner host CPU 10.39s /
4.509549 ms/MiB。
ON约-0.744%吞吐、+0.776%CPU，没有性能收益证据，不认定稳定退化。
一对跨启动、RSSkey变化但queue固定、样本重叠；
OFF/ON九轮rx_discards从0分别增长74/88，不能忽略或归因优化。
观察到全量扫描减少并未带来这组吞吐或CPU改善，不能宣称瓶颈已解决。

独立probe三轮各256MiB PASS：
OFF WRITE_FULL 5000 from12/to12、5c00 from12、
7000 from9、8400 from9；control_write ret0=42。
非read full from44（其中MSI2）、to12；read full0；
control_completion成功118/回退2、data_completion成功75868。
ON control_write成功66/回退0/negative0，WRITE_FULL空、active_writes0。
full只剩read6400一次和MSI一次，to0；control_completion成功260/回退2，
data_completion成功82493，均没有负返回。
OFF full snapshot候选83108页/340410368bytes，实际snapshot_ready copy4页/16384bytes；
ON候选3938页/16130048bytes，实际copy同为4页/16384bytes。
不是scoped AQ/data capture/publish的总复制量；
候选bytes并非实际copy/CPU，两个窗口13.019/27.185s包含不同背景，
不把候选差或时长当精确速度收益。所有probe正常cleanup，active reads/snapshots0。

ON TX G/E/DUAL一轮6s八连接内容/长度均PASS，
768.281/619.865/456.857MiB/s；仅功能、无对应OFF吞吐，
Jinx两IP共享一10G物理口，不证明双卡线性扩展。
随后两卡RX ring 512→256→512成功，CONFIG ACK mask0x3两端再次观察到，
恢复2queues/RX512/TX128和owner IRQ亲和性，post-reset同时RX各256MiB PASS
G2.953s/E3.792s。error级dmesg空、CPU0-3、HPET、两盘各1GiB。
post-reset NIC rx/tx_errors0、rx_dropped0，但tx_dropped G4/E3；
reset可清零NIC统计，不能用post-reset rx0宣称之前74/88discard已解决。
没有精确记录这些TX drop的起始时点，不据此定位原因。

ON正常poweroff两端EXIT0，随后同二进制恢复WRITE_SCOPED=0，
新路径source默认OFF保持，READ的ADMIN_SCOPED=1保留原实验配置。
当前actual QEMU Garen122238(wrapper29536)/Ezreal813813(wrapper63607)。
最终恢复启动、队列配置、两owner ping、diagnostics另由本轮完成核实。
全部probe、Jinx临时source/sinks/aliases清理，未动Lux/worker-g4，
未创建实验文件。磁盘已接受性能未重复测试。

下一步回到每包数据路径做成本计数：现有RX completion对每个ready slot
先read_pool 8-byte DD状态、dma read barrier，再read_pool完整32-byte descriptor并
复核status未变化；此处可能重复同一mapping查找。先量化再考虑合并查找，
保留DD→dma_rmb→完整immutable snapshot→status复核、冷路径和失效回退。
尚未实现这项；无法声明只剩DSM额外成本。

最终恢复核实：两端进程environ WRITE_SCOPED=0、ADMIN_SCOPED=1；
两owner各ping3/3 PASS，四CPU在线、HPET、RSS equal2、两盘各1GiB；
新boot两卡error/drop计数0、guest error级dmesg空。这不覆盖上一boot的discard。
Jinx两个sink与source均STOPLOOM OK/EXIT0，ports及两alias移除；worker6122仍存活。


### RX DD + descriptor 同调用映射复用原型（默认关闭）

LOOM_VFIO_RX_DESC_READ_PAIR=1启用，默认OFF；guest/HPET不改。
新的loom_vfio_dma_read_rx_descriptor先读8-byte DD状态、原DMA read barrier，
DD为1时再捕获完整32-byte immutable descriptor；caller仍比较snapshot status
与先采样word，仍执行长度/status/generation/alias等原检查。
两个原read_pool barrier pairs全保留。DD0 snapshot不写。
仅BQL持有、page_indexed、32B对齐、现有同4KiB buffer/map及pool指针一致
复用本次局部pool地址；不跨调用缓存、不导出DSM地址、不创建map。
其余布局（跨页、partial/non-indexed、cached pool不匹配等）原scalar两读取；
read失败返回原错误，捕获前不发布guest内存。

实机旧路径三个256MiB接收任务内容PASS（带probe每轮约8s，非性能基线）：
RX8-byte calls755050、RX32-byte calls560072；每call均1 map_at+1 buffer_at。
两段合计1315122次各类lookup，ready描述符重复查找560072次。
TX2048-byte快照read194978次；其他payload/control reads另计。
这些是57.683s probe窗口含背景，ns为带多重uprobes的elapsed、不是CPU；
probe显著扰动吞吐，不用来量化可节省CPU百分比。errors0、active/dropped0、
probe正常cleanup。

memfd提取实际struct/page_key/map_at/buffer_at/range_covered/pool_copy/
read_pool/新helper，使用真实GLib索引/链表：
ASAN+UBSAN30005 cases每mode OFF/ON/invalid PASS。
差分返回值、status、全snapshot、barrier次数及caller status复核决定；
四个barrier时点模拟DMA改status/metadata、DD0/1及DD翻转，
native/read-only/direction变体、非indexed/BQL未持有、pool mismatch/null、
缺失map或buffer、仅status8B注册、较大map、32B非对齐/跨页/status跨页、
空container/output/overflow；native ready实际lookup2+2→1+1，DD0仍1+1。
DMA硬件变化由barrier hook模拟，不是完整硬件等价证明。
Ezreal build PASS，Garen同步/构建与实机性能待后续记录。source默认OFF。

### 2026-10-11 RX descriptor 实机验证与远端 CPU offline 对照

代码6dcd4d5b80、e1e40cb1cd两端同步构建PASS；e1关闭开关时保持原caller
scalar调用。两端实际caller+helper GLib索引差分ASAN/UBSAN30005 cases
每mode OFF/ON/invalid均PASS。实机ON计数：DD0 75098、DD1 186952次，
每次恰好1 map_at+1 buffer_at、原scalar子调用0、read errors0；
active/dropped0，probe正常清理。仅证明lookup减少，不证明净CPU收益。

无probe同binary三次启动OFF→ON→OFF恢复，每阶段9次256MiB内容+ACK校验：
RX中位281.132→251.272→259.008 MiB/s；
owner QEMU host CPU (user+system-guest)10.02→10.55→10.29s，
4.349→4.579→4.466 ms/MiB。跨启动有漂移，ON没有证明稳定收益，
RX_DESC_READ_PAIR source默认及当前runtime都OFF。ADMIN_SCOPED=1、
ADMIN_WRITE_SCOPED=0保留；guest内核#26、HPET不改。
ON九轮硬件rx_discards0→23；热ring重配置会清计数，不能宣称修复丢包。
ON两卡各自/并发TX内容校验PASS，RX512→256→512热重配置、IRQ恢复PASS，
重配置后双卡各256MiB并发接收PASS(2.753/4.761s)。
这些功能测试不证明线性聚合；Jinx两个IP共用一个10G物理口。

用户要求的远端CPU offline在ON原型同一次启动中单独对照，
先四CPU在线→CPU2/3 offline→恢复。NIC和磁盘worker都固定CPU0/1，
本地owner网卡enp0s7向Jinx发送2 workers、每轮6s、每阶段3轮，
内容/字节数校验PASS；本地vda 1GiB raw O_DIRECT读，1MiB、
每worker QD4、2 workers、预热2s/测量8s、每阶段3轮。
三阶段网卡发送样本MiB/s：
923.510,771.135,924.512 / 888.622,944.462,968.823 /
932.263,715.469,762.499；
中位923.510→944.462→762.499。
磁盘中位1987.375→2005.375→2009.625 MiB/s。
offline磁盘对初始+0.91%、对恢复-0.21%，没有明显收益；
NIC对初始+2.27%、对恢复+23.86%，在线样本波动大，不能当稳定提升证明。
该轮未测DSM fault数量，CPU offline不停止slave QEMU/DSM服务，
不能排除DSM，也不能把所有波动归因DSM。仅读盘，未做该条件下写盘结论。
本轮不改源代码/guest OS/HPET，仅guest在线状态及IRQ配置。

测试后正常guest关机、两QEMU退出0，再按关闭原型配置启动；
当前Garen154340/Ezreal820039，四CPU在线、HPET、两卡UP、
两数据盘各1GiB；guest error级dmesg空、当前boot网卡error/drop0。
Jinx source/sinks STOPLOOM/EXIT0，测试alias及监听清理；
未动Lux或Garen worker-g4，未创建实验文件。
下一步需要对单卡的数据处理、DMA同步及DSM成本作可区分的测量，
不再以减少lookup次数直接推断带宽提升。
