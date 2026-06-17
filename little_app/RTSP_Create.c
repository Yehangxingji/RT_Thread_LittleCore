/*
 * Minimal RTSP + RTP/H.265 sender.
 *
 * File test mode:
 *   ./rtsp_sender input.h265
 *
 * DATAFIFO mode, used with teammate big-core mpp_pipeline:
 *   ./rtsp_sender --fifo 0x12ffa000
 *
 * VLC:
 *   rtsp://<board-ip>/stream
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "file.h"
#include "nalu_datafifo.h"
#include "rtp.h"
#include "rtsp.h"

typedef enum {
    STREAM_SOURCE_FILE = 0,
    STREAM_SOURCE_DATAFIFO = 1
} stream_source_t;

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

int g_play = 0;// 是否播放
int g_client_addr_set = 0;// 是否设置了客户端地址
struct sockaddr_in g_client_addr;// 客户端地址

static stream_source_t g_source = STREAM_SOURCE_FILE;
static k_u64 g_fifo_phy_addr = 0;// DATAFIFO物理地址

static uint8_t *g_file_buf = NULL;// 文件缓冲区
static nalu_t *g_nalus = NULL;// NALU列表
static size_t g_nalu_count = 0;// NALU数量


// 打印使用说明
static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s input.h265\n", prog);
    printf("  %s --fifo <datafifo_phy_addr>\n", prog);
    printf("\n");
    printf("Example:\n");
    printf("  %s girlshy.h265\n", prog);
    printf("  %s --fifo 0x12ffa000\n", prog);
}

// 解析64位无符号整数参数
static k_u64 parse_u64_arg(const char *text)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return 0;
    }

    return (k_u64)value;
}

// 加载文件源
static int load_file_source(const char *input_path)
{
    size_t file_size = 0;
    nalu_t *nalus = NULL;
    size_t nalu_count = 0;
    uint8_t *file_buf;

    file_buf = read_whole_file(input_path, &file_size);
    if (file_buf == NULL) {
        return -1;
    }

    if (load_annexb_nalus(file_buf, file_size, &nalus, &nalu_count) != 0 ||
        nalu_count == 0) {
        printf("No Annex-B NALU found in %s\n", input_path);
        free(file_buf);
        return -1;
    }

    g_file_buf = file_buf;
    g_nalus = nalus;
    g_nalu_count = nalu_count;

    printf("[source] file=%s size=%lu nalus=%lu\n",
           input_path,
           (unsigned long)file_size,
           (unsigned long)nalu_count);

    return 0;
}

// 获取RTSP目标地址
static void get_rtsp_target(int *playing,
                            struct sockaddr_in *dest,
                            int *dest_valid)
{
    pthread_mutex_lock(&g_mutex);
    *playing = g_play;
    *dest_valid = g_client_addr_set;
    if (g_client_addr_set) {
        *dest = g_client_addr;
    } else {
        memset(dest, 0, sizeof(*dest));
    }
    pthread_mutex_unlock(&g_mutex);
}

// 等待RTSP播放
static void wait_for_rtsp_play(struct sockaddr_in *dest)
{
    pthread_mutex_lock(&g_mutex);
    while (!g_play || !g_client_addr_set) {
        pthread_cond_wait(&g_cond, &g_mutex);
    }
    *dest = g_client_addr;
    pthread_mutex_unlock(&g_mutex);
}

// 文件模式下的RTP发送循环
static void *file_rtp_sender_loop(int sock)
{
    uint16_t seq = 0;
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;
    size_t idx = 0;

    while (1) {
        struct sockaddr_in dest;
        const nalu_t *nalu;
        int nal_type;

        wait_for_rtsp_play(&dest);

        if (g_nalu_count == 0) {
            usleep(SEND_INTERVAL_US);
            continue;
        }

        nalu = &g_nalus[idx];
        nal_type = h265_nalu_type(nalu->data, nalu->len);

        if (send_h265_nalu_rtp(sock,
                               &dest,
                               nalu->data,
                               nalu->len,
                               &seq,
                               timestamp,
                               ssrc,
                               1) != 0) {
            usleep(SEND_INTERVAL_US);
            continue;
        }

        printf("[rtp:file] nalu=%lu type=%d len=%lu ts=%lu seq=%u -> %s:%d\n",
               (unsigned long)idx,
               nal_type,
               (unsigned long)nalu->len,
               (unsigned long)timestamp,
               (unsigned int)seq,
               inet_ntoa(dest.sin_addr),
               ntohs(dest.sin_port));

        idx++;
        if (idx >= g_nalu_count) {
            idx = 0;
        }

        timestamp += RTP_TS_STEP;
        usleep(SEND_INTERVAL_US);
    }

    return NULL;
}

// 发送DATAFIFO包
static int send_datafifo_pack(int sock,
                              const struct sockaddr_in *dest,
                              const mpp_nalu_ipc_pack *pack,
                              uint16_t *seq,
                              uint32_t timestamp,
                              uint32_t ssrc,
                              uint8_t marker)
{
    void *virt_addr;
    int ret;

    virt_addr = nalu_datafifo_mmap_pack(pack);
    if (virt_addr == NULL) {
        return -1;
    }

    ret = send_h265_buffer_rtp(sock,
                               dest,
                               (const uint8_t *)virt_addr,
                               pack->len,
                               seq,
                               timestamp,
                               ssrc,
                               marker);
    //printf("rtp:Datafifo");
    nalu_datafifo_munmap_pack(pack, virt_addr);
    return ret;
}

// DATAFIFO模式下的RTP发送循环
static void *datafifo_rtp_sender_loop(int sock)
{
    nalu_datafifo_reader_t reader;
    uint16_t seq = 0;
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;

    if (nalu_datafifo_open(&reader, g_fifo_phy_addr) != 0) {
        printf("[datafifo] open failed, RTP sender stopped\n");
        return NULL;
    }

    while (1) {
        const mpp_nalu_ipc_msg *msg = NULL;
        void *item = NULL;
        struct sockaddr_in dest;
        int playing = 0;
        int dest_valid = 0;
        k_u32 i;

        if (nalu_datafifo_read(&reader, &msg, &item) != 0) {
            usleep(NALU_DATAFIFO_READ_IDLE_US);
            continue;
        }

        get_rtsp_target(&playing, &dest, &dest_valid);

        if (nalu_datafifo_validate_msg(msg) == 0) {
            printf("[datafifo] seq=%llu chn=%u packs=%u total=%u pts=%llu play=%d\n",
                   (unsigned long long)msg->seq,
                   msg->chn,
                   msg->pack_cnt,
                   msg->total_len,
                   (unsigned long long)msg->frame_pts,
                   playing && dest_valid);

            if (playing && dest_valid) {
                for (i = 0; i < msg->pack_cnt; i++) {
                    uint8_t marker = (i + 1U == msg->pack_cnt) ? 1 : 0;

                    if (send_datafifo_pack(sock,
                                           &dest,
                                           &msg->packs[i],
                                           &seq,
                                           timestamp,
                                           ssrc,
                                           marker) != 0) {
                        printf("[datafifo] send pack[%u] failed: phys=0x%llx len=%u\n",
                               i,
                               (unsigned long long)msg->packs[i].phys_addr,
                               msg->packs[i].len);
                    }
                }

                timestamp += RTP_TS_STEP;
            }
        }

        /*
         * This is mandatory. The big core releases the VENC stream only after
         * the reader marks this DATAFIFO item as done.
         */
        nalu_datafifo_read_done(&reader, item);
    }

    nalu_datafifo_close(&reader);
    return NULL;
}

// RTP发送线程
static void *rtp_sender_thread(void *arg)
{
    int sock;

    (void)arg;

    sock = create_udp_socket_only();
    if (sock < 0) {
        printf("[rtp] failed to create RTP socket\n");
        return NULL;
    }

    printf("[rtp] RTP socket bound to UDP port %d\n", RTP_SERVER_PORT);

    if (g_source == STREAM_SOURCE_DATAFIFO) {
        datafifo_rtp_sender_loop(sock);
    } else {
        file_rtp_sender_loop(sock);
    }

    close(sock);
    return NULL;
}

// 启动RTSP服务器
static int start_rtsp_server(void)
{
    int listen_fd;
    int opt = 1;
    struct sockaddr_in listen_addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("RTSP socket failed, errno=%d\n", errno);
        return -1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(RTSP_PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        printf("RTSP bind port %d failed, errno=%d\n", RTSP_PORT, errno);
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 5) < 0) {
        printf("RTSP listen failed, errno=%d\n", errno);
        close(listen_fd);
        return -1;
    }

    printf("RTSP server started on port %d\n", RTSP_PORT);
    printf("Open VLC URL: rtsp://<board-ip>/stream\n");
    printf("请先用VLC连接客户端,连接到RTSP!!!!!!!!!!!!!!\n");
    return listen_fd;
}

// 主函数
int main(int argc, char **argv)
{
    pthread_t sender_tid;
    int listen_fd;// RTSP监听文件描述符

    if (argc == 3 && strcmp(argv[1], "--fifo") == 0) {
        g_source = STREAM_SOURCE_DATAFIFO;
        g_fifo_phy_addr = parse_u64_arg(argv[2]);
        if (g_fifo_phy_addr == 0) {
            print_usage(argv[0]);
            return -1;
        }
        printf("[source] DATAFIFO mode, fifo phy=0x%llx\n",
               (unsigned long long)g_fifo_phy_addr);
    } else if (argc == 2) {
        g_source = STREAM_SOURCE_FILE;
        if (load_file_source(argv[1]) != 0) {
            return -1;
        }
    } else {
        print_usage(argv[0]);
        return -1;
    }

    if (pthread_create(&sender_tid, NULL, rtp_sender_thread, NULL) != 0) {
        printf("pthread_create RTP sender failed\n");
        free(g_nalus);
        free(g_file_buf);
        return -1;
    }
    pthread_detach(sender_tid);

    listen_fd = start_rtsp_server();
    if (listen_fd < 0) {
        free(g_nalus);
        free(g_file_buf);
        return -1;
    }

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd;

        client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            printf("accept failed, errno=%d\n", errno);
            continue;
        }

        handle_rtsp_client(client_fd, &cli_addr);
        close(client_fd);
        printf("[rtsp] client disconnected, waiting for next client\n");
    }

    close(listen_fd);
    free(g_nalus);
    free(g_file_buf);
    return 0;
}


