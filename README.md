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
