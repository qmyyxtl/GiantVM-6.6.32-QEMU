# 磁盘读数据的 RDMA 直接传输

这是 `gvm-private-io.md` 中私有缓冲区路径的后续优化。代码只修改 QEMU，
复用现有 KVM `KVM_DSM_MEMPIN` / `KVM_DSM_MEMCPY`，不修改 guest。
Garen / Ezreal 的 KVM 保持 `98793b60896fa3855ed31493ca3af5143267c365`。

## 三种可对照的启动模式

两端仍设置 `GVM_PRIVATE_IO=1`，额外设置相同的 `GVM_PRIVATE_IO_ZCOPY`：

| 值 | 磁盘读结果的跨机传输 |
|---|---|
| `off` 或不设置 | 原来的打包 SEND/RECV + DSM memcpy |
| `send` | 磁盘与 RDMA 共用已注册私有缓冲池；接收端仍 DSM memcpy |
| `direct` | 共用缓冲池 + 接收端 DSM lease，直接 RDMA WRITE 到 guest RAM |

必须通过 `sudo -n env GVM_PRIVATE_IO=1 GVM_PRIVATE_IO_ZCOPY=direct ...`
传给实际 QEMU 进程，不能只在 sudo 外设置。保留原启动参数、HPET、
`-nic none`、NUMA、IOThread、cache=none / aio=threads。模式不可在线切换。
两端协议版本与模式必须相同，启动 echo 会检查模式。

## 第一步：磁盘与网卡共用源缓冲区

每个 QEMU 建立 64 MiB、4 KiB 对齐的私有缓冲池，在对应 PD 注册 MR，
用位图分配请求的缓冲区。池不属于 guest RAM，也不参与 DSM。
磁盘直接读入池中的块，RDMA 直接从这些块取数据：

- `io_post_pool_write` 根据 SG 生成 RDMA WRITE，合并两端都连续的段。
- send 模式目标仍为接收节点私有接收缓冲区。
- 数据 WRITE 后在同一 RC QP 提交控制 SEND；SEND 成功完成后才允许回收源块。
- MR 未启用 relaxed ordering；处理下一次请求前补充 receive。
- 每次最多 240 段、2 MiB；较长请求分批处理。

这消除了发送侧的全数据打包复制。写盘仍沿用 READV 取 guest 数据的路径，
因此不能把此改动称为整个双向存储协议完全零复制。

## 第二步：接收端授权直接写 guest

只对本批次至少 4 KiB 的读回数据启用 direct，小消息走 send 路径：

1. 设备 owner 完成磁盘读取，向内存 home 发送目标偏移和长度列表。
2. home 校验范围，将涉及的页面排序、去重、合并相邻范围。
   只合并请求涉及的页，不跨越未请求的页面。
3. home 调用 KVM_DSM_MEMPIN(write=true)，取得独占 DSM 所有权并 pin。
   ioctl 返回时锁已释放；网络等待期间不持有 DSM 全局 mutex 或页锁。
4. home 返回自身固定 RAM backing 的 MR 地址/rkey。
5. owner 从已注册私有池直接 RDMA WRITE 到 guest RAM。
6. 同一 QP 上紧随 WRITE 的 COMMIT SEND 到达 home，表明数据先于提交可见。
7. home 执行内存屏障、设置 dirty 并 unpin，再确认；owner 此后才能发布 virtio 完成。

DSM pin 与 RDMA MR 的物理页生命周期 pin 是不同的保护，缺一不可。
NUMA home 不是对当前所有权的假定，pin 仍须执行 DSM 的所有权检查。
pin 从磁盘完成后才开始，不覆盖磁盘读取等待时间。

现有 KVM pin ioctl 成功可能返回正数访问权限掩码，应只把负数视为失败。
最初 direct 版本误判了正数并在启动中退出，已修复；没有修改 host 内核。

## 范围与限制

- 当前验证的是 x86、两节点、固定 RAM memdev、原生 virtio-blk guest 驱动。
- direct 首次使用时注册本节点整个 RAM memdev，当前配置每节点 2 GiB；
  注册在 VM 运行期间保留，禁止迁移，要求不改变 RAM backing 或拓扑。
- 所有权 pin 按请求释放，MR 不按请求注销。rkey 不是每请求硬件撤销的 capability；
  这是相互信任的 QEMU 节点之间的软件授权协议，不适用于不可信远端。
- 仍有 virtqueue/请求头/状态字的 DSM 与小量复制，不能声称整个系统无 DSM。
- 每 peer RPC 仍串行，direct 增加授权往返；零复制不保证所有大小的请求更快。
- 异常、无效序号、未授权提交、超时、CQ 错误采用 fail-closed，
  不报告 guest 成功、不重用不确定状态的 DMA 缓冲区。
- 尚无重连、节点故障恢复或 live migration；硬件缓存没有因 O_DIRECT 被关闭。

## 验证方法

同一 host/guest、两块原有 1 GiB raw，guest O_DIRECT、host cache=none，
1 MiB 请求、每 worker QD4。单盘 2 worker，双盘 4 worker。
跨机读每次完成后访问每个 4 KiB 页；每项 8 秒 × 3 轮中位数。
写数字包含最后 fsync，不是每次请求同步落盘。

正确性覆盖四路并发（两设备 × 两 CPU 节点），512 B 至 2 MiB 随机数据全比较、
写后改写源缓冲区、flush、迁移到另一节点 CPU 再读回，共 72 例。
另外 12 例使用偏移 512 B 的 guest 缓冲区，检查跨页数据与前后 guard 字节，
并在另一节点 CPU 校验可见性。未创建新的实验数据文件或测试盘。

## 跨机初步结果（8 秒 × 3 轮）

单位 MiB/s，原版基线是本次开始时在旧进程重新测量的结果，不是沿用上次最好值。

| 路径 | Garen 磁盘跨机读 | Ezreal 磁盘跨机读 | Garen 跨机写 | Ezreal 跨机写 |
|---|---:|---:|---:|---:|
| 修改前私有缓冲区复制路径 | 1004.39 | 1156.89 | 949.40 | 941.14 |
| 仅共用发送缓冲池 | 1230.52 | 1484.75 | 943.29 | 1019.33 |
| 直接写 guest，尚未合并 pin 范围 | 1155.02 | 1417.43 | 942.83 | 1012.41 |
| 直接写 guest，合并 pin 范围 | 1386.75 | 1495.24 | 924.61 | 1011.93 |

最终读中位数相对初始基线 +38.07% / +29.25%。仅消除发送复制为 +22.51% / +28.34%。
第二步相对第一步为 +12.70% / +0.71%，Ezreal 的小差异不应解释为确定收益。
直接写入但尚未合并 pin 的版本一度更慢，说明省复制不能抵消任意的 ioctl/往返成本。
写路径没有实现对称的直接 RDMA 读取 guest；写数字用于回归检查。

最终读三个样本：
- Garen：[1535.55, 1386.75, 1175.88]。
- Ezreal：[1498.20, 1477.70, 1495.24]。

Garen 样本有波动；以上均取全部样本中位数，不使用最好值。


## 当前两端完整启动参数

以下命令在已有 VM 正常退出、端口释放后分别在对应 host 执行。只有 master 指定 guest bzImage。

Garen：

```sh
cd /home/xtl/start
sudo -n env \
  GVM_PRIVATE_IO=1 \
  GVM_PRIVATE_IO_ZCOPY=direct \
  'GVM_IPLIST=172.0.1.4 172.0.1.2' \
  GVM_GUEST_EXTRA_ARGS=clocksource=hpet \
  GVM_MACHINE=q35 \
  GVM_USER_NET=0 \
  GVM_QUIET=1 \
  LOOM_RDMA_RECV_CQ_EVENTS=1 \
  GVM_GUEST_KERNEL=/home/xtl/GVM-guest-kernel/arch/x86/boot/bzImage \
  numactl --cpunodebind=0 --membind=0 /home/xtl/GVM-qemu/scripts/gvm-device-node.sh 0 \
  -object memory-backend-ram,id=n0,size=2G \
  -object memory-backend-ram,id=n1,size=2G \
  -numa node,nodeid=0,cpus=0-1,memdev=n0 \
  -numa node,nodeid=1,cpus=2-3,memdev=n1 \
  -numa dist,src=0,dst=1,val=20 \
  -numa dist,src=1,dst=0,val=20 \
  -object iothread,id=owner-io,poll-max-ns=0 \
  -drive file=/home/xtl/start/gvm-device-data-node0.raw,format=raw,if=none,id=data-owner-0,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-0,id=data-owner-0,bus=pcie.0,addr=09.0,serial=GVM-DATA-OWNER0,ioeventfd=on,vectors=5,loom-aggregate=on,loom-numa-node=0,iothread=owner-io
```

Ezreal：

```sh
cd /home/xtl/start
sudo -n env \
  GVM_PRIVATE_IO=1 \
  GVM_PRIVATE_IO_ZCOPY=direct \
  'GVM_IPLIST=172.0.1.4 172.0.1.2' \
  GVM_GUEST_EXTRA_ARGS=clocksource=hpet \
  GVM_MACHINE=q35 \
  GVM_USER_NET=0 \
  GVM_QUIET=1 \
  LOOM_RDMA_RECV_CQ_EVENTS=1 \
  numactl --cpunodebind=0 --membind=0 /home/xtl/GVM-qemu/scripts/gvm-device-node.sh 1 \
  -object memory-backend-ram,id=n0,size=2G \
  -object memory-backend-ram,id=n1,size=2G \
  -numa node,nodeid=0,cpus=0-1,memdev=n0 \
  -numa node,nodeid=1,cpus=2-3,memdev=n1 \
  -numa dist,src=0,dst=1,val=20 \
  -numa dist,src=1,dst=0,val=20 \
  -object iothread,id=owner-io,poll-max-ns=0 \
  -drive file=/home/xtl/start/gvm-device-data-node1.raw,format=raw,if=none,id=data-owner-1,cache=none,aio=threads \
  -device virtio-blk-pci,drive=data-owner-1,id=data-owner-1,bus=pcie.0,addr=0a.0,serial=GVM-DATA-OWNER1,ioeventfd=on,vectors=5,loom-aggregate=on,loom-numa-node=1,iothread=owner-io
```

完整启动后，两端 QMP 需要沿用原测试设置：对 `/machine` 的 `loom-bar-miss-index` 执行 `qom-set`，值为 `true`。

## 本地回归与页面保护核验

短测本地读：单盘 1857.34 / 1847.45，双盘 2930.28 MiB/s，聚合率 79.09%。
短测 Garen 单盘写 direct 为 945.98 MiB/s，样本 [902.90, 965.19, 945.98]；
关闭优化的对应短测为 1004.74 MiB/s，即短测差值 -5.85%。
这两项触发了预先固定的 15 秒 × 5 轮同二进制开关对照：

| 本地访问 | zcopy=off | zcopy=direct | 变化 |
|---|---:|---:|---:|
| Garen 单盘读 | 1816.11 | 1853.64 | +2.07% |
| Ezreal 单盘读 | 1823.48 | 1810.20 | -0.73% |
| 双盘读 | 2937.71 | 2937.11 | -0.02% |
| Garen 单盘写（含最终 flush） | 945.13 | 978.94 | +3.58% |

读聚合率 off 为 80.72%，direct 为 **80.16%**；这是该固定五轮的中位数比值，
不能保证每个样本都超过 80%。本地单盘最差变化为 -0.73%，未超过 5% 门槛。
Garen 写的波动在两种模式都存在，不能把 +3.58% 当作确定的写路径优化收益。

Ezreal 本地写与双盘写使用完整的 8 秒 × 3 轮对照：
- Ezreal：off 957.19，direct 1020.82 MiB/s。
- 双盘：off 1808.57，direct 1986.50 MiB/s。
- direct 双盘写 / 同期两单盘之和 = 101.00%，约为 100%；超出部分是分轮测量波动。

每次重新填满缓冲区的跨机写（8 秒 × 3 轮）：
- Garen：off 887.82，direct 872.87 MiB/s，-1.68%。
- Ezreal：off 968.83，direct 920.00 MiB/s，-5.04%，触发额外固定五轮复核。

另外的 20 秒 kprobe 窗口覆盖一次 8 秒跨机读：
- guest 完成 **6,847,201,280 字节**，并逐页消费数据。
- 接收端 gvm-io-memory 的 DSM memcpy 仅 **196,154 字节**（请求头、小消息）。
- 同线程 DSM pin / unpin 均为 **6,905,786,368 字节、1,591,859 次**，完全配平。
- 同线程没有 DSM fetch；vCPU 与控制/ring 路径仍有 DSM，不能声称系统没有 DSM。
- pin 字节比 payload 略多来自页对齐等；大量 pin ioctl 仍是剩余成本。
- 探针期间吞吐约 802.53 MiB/s，明显受密集探针影响，不纳入性能表。
- 探针已退出，正常结果均在无探针时测量。

额外 64 次竞争检查让另一节点 CPU 在 DMA 期间修改同页相邻字节，
验证读数据、相邻 guard 与跨 CPU 可见性，全部通过。

## 随代码保存的验证入口

- `scripts/gvm-device-guest-disk-read.py`：本次 native Linux AIO 读程序，
  只增加 CPU 节点与逐页消费的命令行选项，I/O 循环保持测试版本。
  跨机示例：`--owner 0 --cpu-node 1 --seconds 8 --rounds 3 --skip-prefill`；
  本地聚合示例：`--no-touch-pages --seconds 15 --rounds 5 --skip-prefill`。
- `scripts/gvm-device-guest-rdma-io-test.py --run-io`：上述 72 + 12 + 64 例正确性检查。
  必须在 guest 执行，且会覆盖两块匹配序列号的专用 1 GiB 数据盘。
  源码从串口 stdin 执行即可，不需要创建 guest 脚本、结果文件或测试文件。
- 原有 `gvm-device-guest-disk-bandwidth.py` 保持不变，继续负责本地写对照。

## 最终写入复核与部署状态

Ezreal 跨机重填写按预先固定的 15 秒 × 5 轮复核：
- off：[926.43, 935.77, 934.83, 875.72, 868.22]，中位数 **926.43 MiB/s**。
- direct：[949.41, 924.75, 977.58, 898.74, 952.13]，中位数 **949.41 MiB/s**。
- direct / off 为 +2.48%，未重复出现短测的 -5.04%。
- 两种模式都有波动；不能把本次修改称为确定的写路径性能提升。

最终功能代码为 `26fa60ecfc`；`7ef3a8839e` 仅整理源码注释、文档和验证入口，
两端均重新编译。最终启用 `GVM_PRIVATE_IO_ZCOPY=direct`，
四路 72 例 + 跨页 guard 12 例 + 同页竞争 64 例，总共 **148 例全部通过**。
测试脚本与读性能脚本的实际源码已随仓库保存，原本地写脚本未修改。
host KVM、guest 内核与 HPET 保持不变，没有重启 host，没有接触 Lux。
