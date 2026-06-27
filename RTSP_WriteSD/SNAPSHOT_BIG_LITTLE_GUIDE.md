# 大小核快照沟通指南

## 1. 目标

快照触发由大核决定，小核负责接收 DATAFIFO 消息、识别快照标志，并把对应帧保存到 `/sdcard/snapshots`。

当前小核快照线程会先保存 H.265 临时码流，再调用 `ffmpeg` 抽取第一帧生成 JPG 图片。转换成功后会删除临时 `.h265`；转换失败时保留 `.h265` 便于排查。输出文件名形如：

```text
/sdcard/snapshots/snapshot_000001.jpg
/sdcard/snapshots/snapshot_000002.jpg
```

当前转换依赖板端可执行 `ffmpeg`。如果板端没有 `ffmpeg`，需要改接 K230 MPP VDEC/JPEG 编码链路，或改由大核在能拿到原始帧/YUV 的位置生成 JPG。

## 2. 线程模型

小核侧建议保持三个职责分离：

```text
DATAFIFO/RTP 线程
  -> 读取大核 DATAFIFO 消息
  -> 检查 reserved 快照标志
  -> 在 READ_DONE 前复制被标记帧的数据
  -> RTP 发送
  -> DATAFIFO_CMD_READ_DONE

快照线程
  -> 等待快照队列
  -> 生成不覆盖的文件名
  -> 写入 /sdcard/snapshots
  -> 后续如需图片，在此线程做解码/转 JPG

RTSP 控制线程
  -> 处理 OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN
```

注意：不要把 SD 卡写入、H.265 解码、JPEG 编码放在 DATAFIFO/RTP 线程里，否则会增加卡顿风险。

## 3. 大核协议

复用 `mpp_nalu_ipc_msg.reserved` 字段作为快照标志位，不改变 DATAFIFO item 大小和消息布局。

约定：

```c
#define MPP_NALU_IPC_FLAG_SNAPSHOT  (1U << 0)
```

大核在需要保存快照的那一帧设置：

```c
msg.reserved |= MPP_NALU_IPC_FLAG_SNAPSHOT;
```

不需要保存快照时：

```c
msg.reserved = 0;
```

如果未来还需要更多事件，可以继续使用 `reserved` 的其他 bit，例如：

```c
bit0: 保存快照
bit1: OSD 告警
bit2: 预留
```

新增 bit 前必须大小核双方同步定义，避免含义冲突。

## 4. 大核职责

大核负责：

- 判断哪一帧需要快照，例如 AI 检测命中、运动检测触发、人工触发。
- 只在目标帧的 `reserved` 中设置 `MPP_NALU_IPC_FLAG_SNAPSHOT`。
- 为保证小核能转 JPG，优先标记 IDR 帧，并确保快照码流包含 VPS/SPS/PPS 等解码参数集。
- 保持 DATAFIFO 消息结构、entry 数量、item size 与小核一致。
- 不向小核传普通虚拟地址，只传物理地址、长度、PTS、pack 信息。
- 等待小核 `DATAFIFO_CMD_READ_DONE` 后再释放对应 VENC stream buffer。

大核不负责小核 SD 卡写入是否成功，但应通过日志确认快照标志已经发出。

## 5. 小核职责

小核负责：

- 打开 DATAFIFO 并持续读取消息，避免大核 pending 表堆满。
- 校验 `magic/version/pack_cnt/phys_addr/len`。
- 看到 `reserved & MPP_NALU_IPC_FLAG_SNAPSHOT` 时，在 `READ_DONE` 前映射 pack 并复制一份快照数据。
- 将复制后的数据放入快照队列，由快照线程异步写 SD 卡。
- 队列满时丢弃快照，优先保证 RTSP/RTP 发送不被阻塞。
- 文件创建使用不覆盖策略，避免多次保存时覆盖旧快照。

关键原则：小核不能把 DATAFIFO 消息里的物理地址直接丢给快照线程后立刻 `READ_DONE`。因为 `READ_DONE` 后大核可能释放 buffer，快照线程再访问就不安全。

## 6. 生命周期顺序

推荐顺序如下：

```text
1. 大核生成一帧 VENC stream
2. 大核判断该帧是否需要快照
3. 若需要，设置 msg.reserved bit0
4. 大核写入 DATAFIFO
5. 小核读取 DATAFIFO item
6. 小核校验消息
7. 小核发现 bit0 被设置
8. 小核 mmap 每个 pack，并复制编码数据
9. 小核把复制后的数据入快照队列
10. 小核继续 RTP 发送
11. 小核调用 DATAFIFO_CMD_READ_DONE
12. 快照线程先写临时 H.265，再调用 ffmpeg 生成 snapshot_xxxxxx.jpg
```

如果后续要保存 JPG，建议把第 12 步扩展为：

```text
快照线程: H.265 -> 解码 -> YUV/RGB -> JPG -> 写 SD 卡
```

或更推荐由大核在原始帧路径上直接生成 JPG，再把 JPG 数据交给小核写入。

## 7. 日志对齐

大核建议打印：

```text
[big] snapshot flag set seq=123 pts=456 reserved=0x1
```

小核收到后应能看到：

```text
[datafifo] seq=123 ... flags=0x1 ...
[snapshot] queued len=... pts=... reason=datafifo-reserved
[snapshot] saved /sdcard/snapshots/snapshot_000001.jpg ...
```

如果出现：

```text
[snapshot] queue full, drop ...
```

表示快照线程来不及处理，本次快照被丢弃，但视频发送应继续运行。

## 8. 测试步骤

1. 大核只对单帧设置 `MPP_NALU_IPC_FLAG_SNAPSHOT`。
2. 小核运行：

```sh
./rtsp_sender --fifo <datafifo_phy_addr>
```

3. VLC 打开：

```text
rtsp://<板子IP>/stream
```

4. 观察大小核日志里的 `seq` 是否对得上。
5. 检查快照目录：

```sh
ls -lh /sdcard/snapshots
```

6. 确认多次触发后文件名递增，旧文件不被覆盖。
7. 确认生成 `.jpg` 图片；如果只留下 `.h265`，查看小核日志中的 ffmpeg 转换失败原因。

## 9. 风险和约束

- 当前目标输出是 `.jpg` 图片；`.h265` 只是转换用临时文件，转换失败时会保留用于排查。
- 真正图片快照需要额外解码和编码，必须放到快照线程或大核低优先级任务中。
- DATAFIFO/RTP 线程只允许做短时间复制，不应做 SD 卡写入或图片编码。
- 队列满时丢弃快照是可接受策略，不能为了保存快照阻塞视频流。
- 大小核必须使用完全一致的 `mpp_nalu_ipc_msg` 定义和 `MPP_NALU_IPC_FLAG_SNAPSHOT` 值。