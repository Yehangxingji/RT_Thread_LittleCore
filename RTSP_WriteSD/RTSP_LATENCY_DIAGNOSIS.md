# RTSP 端到端延迟优化排查指南

## 1. 目标

当前目标不是马上改代码，而是把摄像头采集到 RTSP 客户端显示之间的 1 到 2 秒延迟拆清楚。

完整链路如下：

```text
Camera/VI
  -> VPSS/VENC
  -> 大核 DATAFIFO writer
  -> 小核 DATAFIFO reader
  -> RTP/RTSP UDP sender
  -> RTSP 客户端缓存/解码/显示
```

低延迟优先级高于完整保留每一帧。如果某一段落后，应优先追实时，必要时丢旧帧。

## 2. 第一优先级：确认 DATAFIFO 数据没有错位

先看小核日志：

```text
[datafifo] seq=... chn=0 packs=1 total=... pts=... flags=... play=...
```

重点判断：

- `total` 应接近 H.265 当前帧或 access unit 的实际码流大小。
- `total` 不应该像时间戳一样连续递增到几百万。
- 如果出现 `total_len mismatch`、`unreasonable pack`、`bad pack`，先停止延迟优化，优先修大小核 ABI 或确认运行的是新编译程序。

大小核必须保持一致：

```text
MPP_NALU_IPC_ITEM_SIZE = 512
NALU DATAFIFO entries = 3
mpp_nalu_ipc_msg 包含 submit_time_ms 字段
```

小核侧对应位置：

```text
nalu_datafifo.h: mpp_nalu_ipc_msg / NALU_DATAFIFO_FIFO_ENTRIES
nalu_datafifo.c: nalu_datafifo_validate_msg()
```

如果 `play=0` 时正常、`play=1` 后卡住，常见原因是小核进入 RTP 发送路径后才开始 mmap/send pack；如果 pack 字段错位，就会卡在错误物理地址或离谱长度上，导致 `DATAFIFO_CMD_READ_DONE` 变慢，大核 pending 很快堆满。

## 3. 第二优先级：先排除客户端缓存

VLC 默认缓存可能贡献接近 1 秒。先用同一场景分别测试 VLC 和 ffplay。

推荐 ffplay 低延迟测试：

```sh
ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental \
  -rtsp_transport udp -probesize 32 -analyzeduration 0 \
  rtsp://<board-ip>/stream
```

VLC 可尝试把网络缓存调低，例如 GUI 中设置 Network caching，或用命令行：

```sh
vlc --network-caching=100 rtsp://<board-ip>/stream
```

判断方法：

- ffplay 明显比 VLC 快：主要延迟在客户端缓存。
- ffplay 和 VLC 都慢：继续查编码端、小核发送和 DATAFIFO 堆积。

## 4. 第三优先级：测量分段时间戳

建议每段只打印必要日志，避免逐帧大量串口输出干扰性能。

建议记录这些时间点：

```text
T0 大核采集/获取帧时间
T1 大核 VENC 输出时间
T2 大核写入 DATAFIFO 时间
T3 小核读到 DATAFIFO 时间
T4 小核 RTP send 完成时间
T5 客户端画面显示时间
```

现有大核消息里有 `frame_pts` 和 `submit_time_ms`，小核可用它们判断跨核延迟。

观察重点：

```text
T3 - T2: DATAFIFO / 跨核排队延迟
T4 - T3: 小核 RTP 发送耗时
T5 - T4: 网络 + 客户端缓存/解码延迟
```

如果 `T3 - T2` 越来越大，说明小核消费慢或大核堆积。
如果 `T4 - T3` 大，说明 RTP 发送、mmap、日志、快照或网络发送阻塞。
如果 `T5 - T4` 大，主要看客户端缓存和网络。

## 5. 小核发送路径检查

DATAFIFO 模式应读到就尽快发送，不应按文件模式固定 sleep。

当前小核文件模式会用：

```text
SEND_INTERVAL_US = 1000000 / VIDEO_FPS
```

但 DATAFIFO 模式不应该额外睡 66ms；它应该跟随大核实时帧节奏。

检查点：

- DATAFIFO 模式中不要在人为发送节奏处 `usleep(SEND_INTERVAL_US)`。
- `sendto()` 不应长期阻塞。
- 每帧日志不要过多，串口很慢时会拖住 DATAFIFO reader。
- 快照、SD 卡写入、H.265 转 JPG 不要放在 DATAFIFO/RTP 线程中。
- `DATAFIFO_CMD_READ_DONE` 必须尽快执行，否则大核释放不了 VENC buffer。

Wireshark 过滤：

```text
tcp.port == 554
udp.port == 5004
```

预期：

```text
RTP 包按 15fps 稳定到达，无长时间停顿，无明显成批突发。
```

## 6. 大核编码和缓冲检查

编码端要重点确认：

- VENC 是否启用 B 帧。低延迟应禁用 B 帧。
- GOP 是否过长。GOP 太长会影响首帧和恢复速度。
- 是否有 lookahead 或码率控制缓冲导致排队。
- VENC stream 是否及时取出并释放。
- VB / stream buffer 数量是否过大导致管线天然排队。
- AI/OSD/快照逻辑是否阻塞 VENC 取流线程。

低延迟方向：

```text
采集后尽快编码
编码后尽快写 DATAFIFO
小核尽快 READ_DONE
客户端低缓存播放
```

如果允许丢帧，实时策略应是：

```text
落后时丢旧帧，追最新帧
优先保留 IDR 或最近可解码帧
不要为了补发历史帧牺牲实时性
```

## 7. 测试矩阵

按下面顺序测试，每次只改一个变量：

```text
1. play=0，仅看 DATAFIFO 是否稳定 READ_DONE
2. play=1，VLC 默认缓存
3. play=1，VLC 低 network-caching
4. play=1，ffplay low_delay
5. 降低逐帧日志后再测
6. 关闭快照转换后再测
7. 调整大核 VENC 低延迟参数后再测
```

每组记录：

```text
端到端肉眼延迟
小核 seq 是否连续增长
大核 pending/full/drop 是否出现
Wireshark RTP 是否稳定
CPU 占用或明显卡顿点
```

## 8. 判断结论模板

如果 ffplay 低缓存明显改善：

```text
主要瓶颈是客户端缓存，优先固定播放器参数或 RTSP SDP/发送节奏。
```

如果连接 RTSP 后大核 pending 增长：

```text
小核发送或 READ_DONE 路径太慢，优先查 mmap/sendto/日志/快照线程影响。
```

如果小核收到帧已经晚很多：

```text
延迟在大核采集、编码或 DATAFIFO writer 前，优先查 VENC/VB/取流释放。
```

如果 RTP 已实时到达但客户端显示慢：

```text
延迟主要在客户端缓存或解码显示策略。
```

## 9. 当前建议优先级

1. 确认小核运行的是包含 `submit_time_ms` 的新版 ABI。
2. 用 ffplay low_delay 和 VLC 低缓存对比客户端延迟。
3. 暂时降低小核逐帧日志量，避免日志影响实时线程。
4. 确认 DATAFIFO 模式没有额外按文件模式 sleep。
5. 在大核确认 VENC 无 B 帧、及时取流释放、缓冲数量不过大。
6. 若仍有堆积，再考虑小核非阻塞发送、丢旧帧追实时策略。