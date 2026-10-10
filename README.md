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
