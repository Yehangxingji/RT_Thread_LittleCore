# 小核低延迟改动对比

本文记录当前小核 RTSP/DATAFIFO 低延迟相关改动，重点说明修改前后的行为差异、预期效果和验证方法。

## 目标

当前目标是降低摄像头采集到 RTSP 客户端显示的端到端延迟。优化原则是实时优先：当小核发送链路落后时，允许丢弃旧帧，优先发送 DATAFIFO 中最新的一帧，避免客户端补播历史画面。

## 改动摘要

| 文件 | 修改点 | 目的 |
| --- | --- | --- |
| `RTSP_Create.c` | DATAFIFO 发送循环改为 drain 到最新帧 | 避免 PLAY 后补发旧帧 |
| `RTSP_Create.c` | `g_play == 0` 时仍持续读取并 `READ_DONE` | 防止客户端未播放期间堆积旧画面 |
| `RTSP_Create.c` | 旧 item 立即 `READ_DONE`，只保留最新 item | 释放大核 VENC/DATAFIFO buffer |
| `RTSP_Create.c` | DATAFIFO 模式不使用文件模式固定 sleep 节拍 | 以大核实际到达帧为节拍 |
| `RTSP_Create.c` | 增加低频统计日志 | 观察丢帧、跳帧和发送耗时 |
| `rtp.h` | `VIDEO_FPS` 为 15，RTP timestamp step 为 6000 | 对齐当前 15fps 码流 |
| `nalu_datafifo.h` | 小核 ABI 包含 `submit_time_ms`，entries 为 3 | 对齐大核 DATAFIFO 协议 |
| `nalu_datafifo.c` | 增加 DATAFIFO 可读长度和消息长度校验 | 尽早发现 ABI 错位或脏数据 |

## DATAFIFO 发送策略对比

### 修改前

小核读取到 DATAFIFO item 后按顺序发送。若 RTSP 客户端尚未 PLAY，或 RTP 发送速度低于大核产帧速度，旧帧可能在 DATAFIFO/发送链路中排队。

典型风险：

- VLC 发起 PLAY 后，先看到连接前或排队中的旧画面。
- 小核如果追着旧帧发，会把端到端延迟越攒越大。
- 大核只有少量 pending buffer，小核 `READ_DONE` 不及时会反向拖住大核。

### 修改后

新增 `datafifo_read_latest()`，每轮读取当前所有可读 DATAFIFO item：

1. 读取第一条 item 后暂存为候选最新帧。
2. 如果又读到更新 item，则对上一条旧 item 立即调用 `READ_DONE` 并计入 `dropped_count`。
3. 循环结束后只保留最后读到的最新 item。
4. 如果 RTSP 已 PLAY，则只发送这条最新帧。
5. 如果 RTSP 未 PLAY，则不发送，但仍对最新 item 调用 `READ_DONE`，保持 DATAFIFO 被持续清空。

这样可以保证小核的行为是追实时，而不是补历史。

## READ_DONE 行为对比

### 修改前关注点

如果小核拿到 DATAFIFO item 后长时间不 `READ_DONE`，大核侧 VENC stream 释放会变慢，pending 可能升高。

### 修改后行为

- 被丢弃的旧 item：立即 `READ_DONE`。
- 被发送的最新 item：发送完成后 `READ_DONE`。
- 未播放时读到的最新 item：不发送，但本轮结束时 `READ_DONE`。

这能保护大核 DATAFIFO 和 VENC buffer，不让小核侧的播放器状态影响大核持续产流。

## RTP 节拍对比

### 文件模式

文件模式仍保留 `usleep(SEND_INTERVAL_US)`，因为文件回放没有真实采集节拍，需要人为模拟 15fps。

### DATAFIFO 实时模式

DATAFIFO 模式不再按文件模式固定 sleep 发送。小核读到大核提交的帧后立即处理，由大核实际产帧节奏决定 RTP 发送节奏。

当前 `rtp.h` 中：

```c
#define VIDEO_FPS            15
#define RTP_TS_STEP          (RTP_CLOCK_RATE / VIDEO_FPS)
```

15fps 下 RTP timestamp 每帧递增 6000。

## 日志对比

### 修改前

原逐帧日志主要打印 seq、pack 数、total、pts、flags、play 状态，无法直接看出小核是否在丢旧帧追实时，也看不出发送耗时。

### 修改后

DATAFIFO 日志改为低频输出，默认每 30 帧打印一次；发生丢帧、seq gap 或发送耗时过长时会额外打印。

日志格式：

```text
[datafifo] seq=<seq> drop=<dropped_count> gap=<seq_gap> packs=<pack_cnt> total=<total_len> pts=<frame_pts> submit=<submit_time_ms> send=<send_cost_ms>ms play=<0|1> flags=<reserved>
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `seq` | 当前发送或处理的最新帧序号 |
| `drop` | 本轮 drain 时丢弃的旧 item 数量 |
| `gap` | 当前帧和上一帧已发送 seq 之间跳过的帧数 |
| `packs` | 当前帧包含的 pack 数 |
| `total` | 当前帧总码流长度 |
| `pts` | 大核填入的帧 PTS |
| `submit` | 大核提交到 DATAFIFO 的时间戳 |
| `send` | 小核发送当前帧所有 pack 的耗时 |
| `play` | 当前 RTSP 是否处于 PLAY 状态 |
| `flags` | `reserved` 字段，bit0 用于快照触发 |

## 快照路径关系

快照仍走异步 snapshot writer 线程，不在 RTP 发送快路径直接写 SD 卡。

当前小核在验证到 DATAFIFO msg 有效后调用 `enqueue_datafifo_snapshot()`。如果大核通过 `reserved` bit0 标记快照，快照任务会入队给写盘线程处理；RTP 发送线程继续工作，不等待 SD 卡写入或 H.265 到图片转换完成。

## 预期效果

- RTSP 客户端 PLAY 后更快看到当前画面。
- 如果小核或网络偶尔慢于 15fps，画面会跳过旧帧追上实时，而不是逐帧补发导致延迟扩大。
- 大核 pending 应更稳定，不应因为客户端未 PLAY 或 VLC 缓冲导致小核不释放 DATAFIFO item。
- 串口日志量下降，减少日志输出对实时发送的干扰。

## 需要重点观察的现象

| 现象 | 判断 |
| --- | --- |
| `drop` 偶尔大于 0，延迟下降 | 正常，说明小核在追实时 |
| `gap` 偶尔大于 0，画面轻微跳帧 | 正常，实时优先策略生效 |
| `send` 经常大于 66ms | 小核发送、mmap、网络或客户端链路仍慢 |
| `total` 连续出现几百万且像时间戳递增 | 仍可能存在 ABI 错位或旧程序未更新 |
| 大核 pending 长期升高 | 小核 `READ_DONE`、发送阻塞或 DATAFIFO 协议仍需继续排查 |

## 测试建议

1. 启动小核 DATAFIFO RTSP 服务。
2. 暂时不连接 RTSP 客户端，观察小核是否持续 drain，日志中 `play=0` 时不发送 RTP。
3. 用 ffplay 低缓存模式连接：

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -rtsp_transport udp rtsp://<board-ip>/stream
```

4. 再用 VLC 默认缓存和低缓存参数分别对比：

```bash
vlc --network-caching=100 --live-caching=100 --rtsp-caching=100 --clock-jitter=0 --drop-late-frames --skip-frames rtsp://<board-ip>/stream
```

5. 观察小核日志中的 `drop`、`gap`、`send`，同时观察大核 pending 是否稳定在 0 到 2。

## 回退思路

如果比赛验证要求“不允许跳帧”，可以把 DATAFIFO 策略从“只发最新帧”改回“逐帧发送”。但这会牺牲低延迟，一旦小核或客户端慢下来，延迟会重新累积。

如果只想降低丢帧强度，可以保留 drain 机制，但改为最多丢弃到最近 IDR 或关键帧，再从关键帧继续发送。该方案需要大核在 DATAFIFO msg 中明确标出 IDR/关键帧信息。
