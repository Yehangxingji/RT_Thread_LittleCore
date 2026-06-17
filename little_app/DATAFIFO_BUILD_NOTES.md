# DATAFIFO RTSP 发送器编译与运行说明

本目录是一份最小 RTSP + RTP/H.265 发送器，默认面向小核 Linux；同时保留 RT-Smart 分支。

## 1. 支持的两种输入模式

### 文件测试模式

用于不接大核时，先验证 RTSP/RTP/H.265 发送链路：

```sh
./rtsp_sender input.h265
```

VLC 打开：

```text
rtsp://<板子IP>/stream
```

### DATAFIFO 模式

用于对接队友大核 `mpp_pipeline` 输出的 H.265/NALU 码流：

```sh
./rtsp_sender --fifo 0x12ffa000
```

这里的 `0x12ffa000` 只是示例，实际值来自大核日志：

```text
[MPP] NALU IPC DATAFIFO init OK: phy_addr=0x12ffa000
```

注意：这个地址是 DATAFIFO ring buffer 的物理地址，不是某个 NALU pack 的物理地址。

## 2. 代码文件

需要一起编译的源文件：

```text
RTSP_Create.c
rtsp.c
rtp.c
file.c
nalu_datafifo.c
```

头文件位于同一目录：

```text
rtsp.h
rtp.h
file.h
nalu_datafifo.h
```

## 3. SDK 依赖

默认小核任务需要的 SDK 头文件：

```text
/home/ubuntu/k230_sdk/src/big/mpp/include/k_type.h
/home/ubuntu/k230_sdk/src/common/cdk/user/component/datafifo/include/k_datafifo.h
```

RT-Smart 任务额外需要：

```text
/home/ubuntu/k230_sdk/src/big/mpp/userapps/api/mpi_sys_api.h
```

默认小核任务链接：

```text
/home/ubuntu/k230_sdk/src/common/cdk/user/component/datafifo/slave/lib/libdatafifo.a
```

RT-Smart 任务链接：

```text
/home/ubuntu/k230_sdk/src/common/cdk/user/component/datafifo/host/lib/libdatafifo.a
/home/ubuntu/k230_sdk/src/big/mpp/userapps/lib/libsys.a
```

## 4. 数据处理流程

当前 NALU IPC DATAFIFO 参数按 SDK VENC DATAFIFO 对齐：`128` 个 entry，每个 item `512` 字节，必须和大核 writer 侧 `u32EntriesNum` / `u32CacheLineSize` 完全一致。小核代码在 `nalu_datafifo.h` 中用 `NALU_DATAFIFO_EXPECTED_ITEM_SIZE` 做编译期检查；如果外部 `mpp_nalu_ipc.h` 定义成其他 item size，会直接编译失败。

### 小核 Linux

```text
kd_datafifo_open_by_addr()
  -> kd_datafifo_read()
  -> 校验 mpp_nalu_ipc_msg
  -> 对每个 pack 使用 /dev/mem + mmap 映射
  -> 拆 Annex-B 起始码并发送 RTP/H.265
  -> munmap()
  -> DATAFIFO_CMD_READ_DONE
```

### RT-Smart

```text
kd_datafifo_open_by_addr()
  -> kd_datafifo_read()
  -> 校验 mpp_nalu_ipc_msg
  -> 对每个 pack 使用 kd_mpi_sys_mmap()
  -> 拆 Annex-B 起始码并发送 RTP/H.265
  -> kd_mpi_sys_munmap()
  -> DATAFIFO_CMD_READ_DONE
```

`DATAFIFO_CMD_READ_DONE` 必须在每个 item 处理完后调用。大核侧只有收到 READ_DONE，才会在 release callback 中释放对应的 VENC stream buffer。

如果 VLC 还没 PLAY，小核仍然会读取 DATAFIFO item 并调用 READ_DONE，只是不发送 RTP。这样可以避免大核 pending 表被填满。

## 5. VS Code 构建任务

`.vscode/tasks.json` 里提供了两个任务：

```text
build little core app
build rt-smart app
```

默认任务是小核 Linux，输出 `rtsp_sender`。

RT-Smart 任务定义了 `USE_RT_SMART`，输出 `rtsp_sender_rtsmart.elf`。

手动编译小核 Linux 版本时，不能只传源文件；必须同时带上 SDK include path 和 `slave/lib/libdatafifo.a`：

```sh
/home/ubuntu/k230_sdk/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin/riscv64-unknown-linux-gnu-gcc \
  -O2 -g -Wall -Wextra -D_FILE_OFFSET_BITS=64 \
  -I. \
  -I/home/ubuntu/k230_sdk/src/common/cdk/user/component/datafifo/include \
  -I/home/ubuntu/k230_sdk/src/big/mpp/include \
  -o rtsp_sender \
  RTSP_Create.c rtsp.c rtp.c file.c nalu_datafifo.c \
  -L/home/ubuntu/k230_sdk/src/common/cdk/user/component/datafifo/slave/lib \
  -ldatafifo -lpthread
```

## 6. 推荐测试顺序

1. 先运行文件模式，确认 VLC 能访问 `rtsp://<板子IP>/stream`。
2. 再运行大核 `mpp_pipeline`，记录日志里的 DATAFIFO `phy_addr`。
3. 小核运行 DATAFIFO 模式：

```sh
./rtsp_sender --fifo <大核打印的phy_addr>
```

4. Wireshark 过滤：

```text
tcp.port == 554
udp.port == 5004
```

预期结果：

```text
RTSP: OPTIONS -> DESCRIBE -> SETUP -> PLAY
RTP: PT=96, seq 持续递增
大核: 不再持续出现 pending table full 或 stream_export_submit_venc_stream failed
```

## 7. 当前版本限制

- 只支持 RTP over UDP，不支持 RTP over RTSP TCP interleaved。
- 当前 timestamp 按每个 DATAFIFO message 递增一次，后续若播放抖动，再按 access unit 做更精细分组。
- 小核不能把 `phys_addr` 直接当普通指针使用，必须先走 `/dev/mem` 映射。
- RT-Smart 分支仍然依赖 SDK 的 `kd_mpi_sys_mmap()` / `kd_mpi_sys_munmap()`。
