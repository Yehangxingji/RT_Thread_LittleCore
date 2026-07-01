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
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "datafifo_snapshot.h"
#include "file.h"
#include "nalu_datafifo.h"
#include "rtp.h"
#include "rtsp.h"
#include "snapshot_writer.h"

#ifndef NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS
#define NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS 3000ULL
#endif

typedef enum {
    STREAM_SOURCE_FILE = 0,
    STREAM_SOURCE_DATAFIFO = 1
} stream_source_t;

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

int g_play = 0;
int g_client_addr_set = 0;
struct sockaddr_in g_client_addr;

static stream_source_t g_source = STREAM_SOURCE_FILE;
static k_u64 g_fifo_phy_addr = 0;
static uint8_t *g_file_buf = NULL;
static nalu_t *g_nalus = NULL;
static size_t g_nalu_count = 0;
static int g_snapshot_ready = 0;

#define DATAFIFO_LOG_INTERVAL 30U

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s input.h265\n", prog);
    printf("  %s --fifo <datafifo_phy_addr>\n", prog);
    printf("\n");
    printf("Example:\n");
    printf("  %s girlshy.h265\n", prog);
    printf("  %s --fifo 0x12ffa000\n", prog);
    printf("\n");
    printf("DATAFIFO snapshot: set mpp_nalu_ipc_msg.reserved bit0 (0x%x).\n",
           MPP_NALU_IPC_FLAG_SNAPSHOT);
}

static void cleanup_resources(void)
{
    if (g_snapshot_ready) {
        snapshot_writer_deinit();
        g_snapshot_ready = 0;
    }
    datafifo_snapshot_deinit();
    free(g_nalus);
    g_nalus = NULL;
    free(g_file_buf);
    g_file_buf = NULL;
}

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

static int parse_args(int argc, char **argv)
{
    const char *input_path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fifo") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            g_source = STREAM_SOURCE_DATAFIFO;
            g_fifo_phy_addr = parse_u64_arg(argv[++i]);
            if (g_fifo_phy_addr == 0) {
                return -1;
            }
        } else if (argv[i][0] == '-') {
            return -1;
        } else {
            if (input_path != NULL) {
                return -1;
            }
            input_path = argv[i];
        }
    }

    if (g_source == STREAM_SOURCE_DATAFIFO) {
        if (input_path != NULL) {
            return -1;
        }
        printf("[source] DATAFIFO mode, fifo phy=0x%llx\n",
               (unsigned long long)g_fifo_phy_addr);
        return 0;
    }

    if (input_path == NULL) {
        return -1;
    }

    g_source = STREAM_SOURCE_FILE;
    return load_file_source(input_path);
}

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

static void wait_for_rtsp_play(struct sockaddr_in *dest)
{
    pthread_mutex_lock(&g_mutex);
    while (!g_play || !g_client_addr_set) {
        pthread_cond_wait(&g_cond, &g_mutex);
    }
    *dest = g_client_addr;
    pthread_mutex_unlock(&g_mutex);
}
static uint64_t monotonic_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }

    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000U);
}

static void datafifo_log_reader_idle(uint64_t last_seq,
                                     uint64_t last_item_ms,
                                     unsigned int idle_loops)
{
    static uint64_t last_idle_log_ms;
    uint64_t now = monotonic_ms();

    if (now == 0) {
        return;
    }

    if (last_idle_log_ms == 0 ||
        now - last_idle_log_ms >= NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS) {
        uint64_t idle_ms = last_item_ms ? now - last_item_ms : 0;

        printf("[datafifo] reader idle last_seq=%llu idle_ms=%llu idle_loops=%u\n",
               (unsigned long long)last_seq,
               (unsigned long long)idle_ms,
               idle_loops);
        last_idle_log_ms = now;
    }
}

static void log_datafifo_stats(const mpp_nalu_ipc_msg *msg,
                               int playing,
                               unsigned int dropped_count,
                               uint64_t last_seq,
                               uint64_t send_cost_ms,
                               unsigned int frame_count,
                               int force)
{
    uint64_t seq_gap = 0;

    if (msg == NULL) {
        return;
    }

    if (last_seq != 0 && msg->seq > last_seq + 1ULL) {
        seq_gap = msg->seq - last_seq - 1ULL;
    }

    if (!force && dropped_count == 0 && seq_gap == 0 &&
        (frame_count % DATAFIFO_LOG_INTERVAL) != 0) {
        return;
    }

    printf("[datafifo] seq=%llu drop=%u gap=%llu packs=%u total=%u pts=%llu submit=%llu send=%llums play=%d flags=0x%x\n",
           (unsigned long long)msg->seq,
           dropped_count,
           (unsigned long long)seq_gap,
           msg->pack_cnt,
           msg->total_len,
           (unsigned long long)msg->frame_pts,
           (unsigned long long)msg->submit_time_ms,
           (unsigned long long)send_cost_ms,
           playing,
           msg->reserved);
}


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

static void *datafifo_rtp_sender_loop(int sock)
{
    nalu_datafifo_reader_t reader;
    uint16_t seq = 0;
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;
    uint64_t last_sent_seq = 0;
    uint64_t last_read_seq = 0;
    uint64_t last_item_seq = 0;
    uint64_t last_item_ms = monotonic_ms();
    unsigned int frame_count = 0;
    unsigned int idle_loops = 0;

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
        uint64_t send_start_ms = 0;
        uint64_t send_cost_ms = 0;
        k_u32 i;

        if (nalu_datafifo_read(&reader, &msg, &item) != 0) {
            idle_loops++;
            datafifo_log_reader_idle(last_item_seq, last_item_ms, idle_loops);
            usleep(NALU_DATAFIFO_READ_IDLE_US);
            continue;
        }
        idle_loops = 0;
        if (msg != NULL) {
            last_item_seq = msg->seq;
        }
        last_item_ms = monotonic_ms();

        get_rtsp_target(&playing, &dest, &dest_valid);

        if (nalu_datafifo_validate_msg(msg) == 0) {
            int snapshot_ret = 0;
            int send_failed = 0;
            int stale_frame = 0;

            if (last_read_seq != 0 && msg->seq <= last_read_seq) {
                stale_frame = 1;
                printf("[datafifo] seq rollback/stale current=%llu last=%llu flags=0x%x item=%p\n",
                       (unsigned long long)msg->seq,
                       (unsigned long long)last_read_seq,
                       msg->reserved,
                       item);
            }
            if (!stale_frame) {
                last_read_seq = msg->seq;
            }

            if (!stale_frame && (msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT)) {
                snapshot_ret = datafifo_snapshot_process(msg, g_snapshot_ready);
                printf("[snapshot] seq=%llu process ret=%d flags=0x%x\n",
                       (unsigned long long)msg->seq,
                       snapshot_ret,
                       msg->reserved);
            }

            if (stale_frame) {
                printf("[datafifo] drop stale seq=%llu last=%llu, READ_DONE only\n",
                       (unsigned long long)msg->seq,
                       (unsigned long long)last_read_seq);
            } else if (playing && dest_valid) {
                send_start_ms = monotonic_ms();
                for (i = 0; i < msg->pack_cnt; i++) {
                    uint8_t marker = (i + 1U == msg->pack_cnt) ? 1 : 0;

                    if (send_datafifo_pack(sock,
                                           &dest,
                                           msg->seq,
                                           i,
                                           &msg->packs[i],
                                           &seq,
                                           timestamp,
                                           ssrc,
                                           marker) != 0) {
                        send_failed = 1;
                        printf("[datafifo] send pack[%u] failed: phys=0x%llx len=%u\n",
                               i,
                               (unsigned long long)msg->packs[i].phys_addr,
                               msg->packs[i].len);
                    }
                }
                send_cost_ms = monotonic_ms() - send_start_ms;
                timestamp += RTP_TS_STEP;
                frame_count++;
                log_datafifo_stats(msg,
                                   1,
                                   0,
                                   last_sent_seq,
                                   send_cost_ms,
                                   frame_count,
                                   send_cost_ms > (1000U / VIDEO_FPS) || send_failed || snapshot_ret != 0);
                last_sent_seq = msg->seq;
            } else {
                printf("[datafifo] seq=%llu no RTP target play=%d dest=%d flags=0x%x\n",
                       (unsigned long long)msg->seq,
                       playing,
                       dest_valid,
                       msg->reserved);
                frame_count++;
                log_datafifo_stats(msg,
                                   0,
                                   0,
                                   last_sent_seq,
                                   0,
                                   frame_count,
                                   snapshot_ret != 0);
            }
        } else {
            printf("[datafifo] skip invalid item but still READ_DONE item=%p\n",
                   item);
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

int main(int argc, char **argv)
{
    pthread_t sender_tid;
    int listen_fd;

    if (parse_args(argc, argv) != 0) {
        print_usage(argv[0]);
        return -1;
    }

    if (g_source == STREAM_SOURCE_DATAFIFO) {
        if (snapshot_writer_init(SNAPSHOT_DEFAULT_DIR) == 0) {
            g_snapshot_ready = 1;
        } else {
            printf("[snapshot] writer init failed, DATAFIFO snapshots disabled\n");
        }
    }

    if (pthread_create(&sender_tid, NULL, rtp_sender_thread, NULL) != 0) {
        printf("pthread_create RTP sender failed\n");
        cleanup_resources();
        return -1;
    }
    pthread_detach(sender_tid);

    listen_fd = start_rtsp_server();
    if (listen_fd < 0) {
        cleanup_resources();
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
    cleanup_resources();
    return 0;
}
