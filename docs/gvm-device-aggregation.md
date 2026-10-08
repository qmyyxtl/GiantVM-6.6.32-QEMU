# RDMA 版设备聚合

本分支在 GVM 的 RDMA 巨虚上移植 Loom PCI owner/remote 结构。一个 OS 分别看到各节点持有的 PCI 设备，设备不合并为逻辑卷或网络接口。

## 启动

Host 使用现有 6.6.32-krdma+，QEMU 基于 9.0.90。默认 4 CPU、4 GiB，Ezreal 执行 CPU0/1，Jinx 执行 CPU2/3。两端必须紧接着启动，沿用已有 IB 地址 172.0.1.2、172.0.1.3。

在 Ezreal/Jinx 的 /home/xtl/GVM-qemu 分别运行：

```bash
GVM_QUIET=1 ./scripts/gvm-device-node.sh 0  # Ezreal
GVM_QUIET=1 ./scripts/gvm-device-node.sh 1  # Jinx
```

节点0使用已有 /home/xtl/start/bzImage 和原始 Debian 根盘；节点1不指定 kernel。脚本不创建日志、PID 文件、磁盘副本或 overlay。默认各 owner 一个 EDU，guest BDF 为 00:05.0、00:06.0。LOOM_DEVICE_AGGREGATION=1 启用聚合；设备以 loom-aggregate=on 标记，addr 必须全局唯一。额外 QEMU 参数可以直接追加到脚本后。

串口与 QMP 分别监听各主机的 loopback 5555（master）和 4444：

```bash
ssh -t Ezreal 'socat STDIO,raw,echo=0,escape=0x1d TCP:127.0.0.1:5555'
```

## 机制

- owner 持有唯一设备模型，其他节点创建 loom-remote-pci。启动前通过 RDMA 获取根总线静态目录；不映射共享 DAX，也不依赖跨主机共享指针或原子操作。
- 配置与 BAR 操作定向 owner，读写均得到处理确认。BAR 映射前缀按需从 owner 刷新；扩展配置读取由 owner 执行。
- RDMA 调用有长度检查、每 peer 的响应串行化及超时；等待期间释放 BQL。超时意味着操作结果未知，不自动重试设备 I/O。
- 模拟 DMA 沿用 GVM 的 KVM_DSM_MEMCPY/MEMPIN。真实 VFIO 硬件 DMA 尚未接入这一协议。
- 第一版跨节点 MSI 支持 fixed、physical、xAPIC 目标，按全局 CPU 归属路由，在设备回调结束后发送。聚合模式停用本地 irqfd 快捷路径。
- INTx 经 master 的统一 PCI/IOAPIC 视图投递。配置复位具有设备代次；访问失败停止当前 QEMU，不提供透明恢复。

## 验证

scripts/gvm-device-guest-test.py 在 guest 中验证 CPU0/CPU2 × owner0/owner1 的枚举、BAR、有序访问、DMA 往返和双向并发请求。DMA 缓冲区使用启动参数预留的 64 KiB guest RAM。

MSI 目标测试使用 Linux 现有 IRQ-work 向量 0xf6，观察 /proc/interrupts 的 IWI 计数。它证明投递到指定 CPU，但不能代替 PCI 驱动的 IRQ 分配、亲和性、MSI-X 或 irqfd 集成测试。测试代码可经串口以内存 exec 运行，无需在 guest 保存脚本。

## 当前边界

只支持单根总线上的静态 PCI endpoint；动态热插拔、逻辑/广播 MSI、内核 irqfd 加速、真实 VFIO DMA、跨节点故障 supervisor 尚未实现。现有根盘仍使用 master 的旧设备路径。本分支不修改 host 内核。
