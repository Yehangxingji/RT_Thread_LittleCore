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

#include "file.h"
#include "nalu_datafifo.h"
#include "rtp.h"
#include "rtsp.h"
#include "snapshot_writer.h"

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

#define H265_START_CODE_SIZE 4U
#define H265_NAL_BLA_W_LP    16
#define H265_NAL_CRA_NUT     21
#define H265_NAL_VPS         32
#define H265_NAL_SPS         33
#define H265_NAL_PPS         34
#define H265_SNAPSHOT_MAX_GOP_BYTES (8U * 1024U * 1024U)

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} h265_byte_buffer_t;

typedef struct {
    h265_byte_buffer_t au;
    int copy_au;
    int has_vps;
    int has_sps;
    int has_pps;
    int has_irap;
} h265_snapshot_frame_t;

typedef struct {
    h265_byte_buffer_t vps;
    h265_byte_buffer_t sps;
    h265_byte_buffer_t pps;
    h265_byte_buffer_t last_irap;
    h265_byte_buffer_t current_gop;
    uint64_t last_irap_pts;
    uint64_t last_irap_seq;
    uint64_t current_gop_pts;
    uint64_t current_gop_start_seq;
    uint64_t current_gop_last_seq;
    int last_irap_has_vps;
    int last_irap_has_sps;
    int last_irap_has_pps;
    int current_gop_has_irap;
} h265_snapshot_cache_t;

static h265_snapshot_cache_t g_h265_snapshot_cache;

typedef struct {
    const mpp_nalu_ipc_msg *msg;
    void *item;
} datafifo_pending_item_t;

static int process_datafifo_snapshot_frame(const mpp_nalu_ipc_msg *msg);

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
    free(g_h265_snapshot_cache.vps.data);
    free(g_h265_snapshot_cache.sps.data);
    free(g_h265_snapshot_cache.pps.data);
    free(g_h265_snapshot_cache.last_irap.data);
    free(g_h265_snapshot_cache.current_gop.data);
    memset(&g_h265_snapshot_cache, 0, sizeof(g_h265_snapshot_cache));
    free(g_nalus);
    g_nalus = NULL;
    free(g_file_buf);
    g_file_buf = NULL;
}

static void h265_buffer_reset(h265_byte_buffer_t *buffer)
{
    if (buffer != NULL) {
        buffer->len = 0;
    }
}

static int h265_buffer_reserve(h265_byte_buffer_t *buffer, size_t extra_len)
{
    size_t needed;
    size_t new_cap;
    uint8_t *new_data;

    if (buffer == NULL) {
        return -1;
    }

    if (extra_len > SIZE_MAX - buffer->len) {
        return -1;
    }

    needed = buffer->len + extra_len;
    if (needed <= buffer->cap) {
        return 0;
    }

    new_cap = buffer->cap ? buffer->cap : 4096U;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2U) {
            new_cap = needed;
            break;
        }
        new_cap *= 2U;
    }

    new_data = (uint8_t *)realloc(buffer->data, new_cap);
    if (new_data == NULL) {
        return -1;
    }

    buffer->data = new_data;
    buffer->cap = new_cap;
    return 0;
}

static int h265_buffer_append(h265_byte_buffer_t *buffer,
                              const uint8_t *data,
                              size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (data == NULL || h265_buffer_reserve(buffer, len) != 0) {
        return -1;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return 0;
}

static int h265_buffer_replace(h265_byte_buffer_t *buffer,
                               const uint8_t *data,
                               size_t len)
{
    h265_buffer_reset(buffer);
    return h265_buffer_append(buffer, data, len);
}

static int h265_buffer_append_start_code(h265_byte_buffer_t *buffer)
{
    static const uint8_t start_code[H265_START_CODE_SIZE] = {0x00, 0x00, 0x00, 0x01};

    return h265_buffer_append(buffer, start_code, sizeof(start_code));
}

static int h265_find_start_code(const uint8_t *buf,
                                size_t len,
                                size_t offset,
                                size_t *start_code_len)
{
    size_t i;

    if (buf == NULL || start_code_len == NULL || offset >= len) {
        return -1;
    }

    for (i = offset; i + 3U <= len; i++) {
        if (buf[i] == 0x00 && buf[i + 1U] == 0x00 && buf[i + 2U] == 0x01) {
            *start_code_len = 3U;
            return (int)i;
        }

        if (i + 4U <= len &&
            buf[i] == 0x00 &&
            buf[i + 1U] == 0x00 &&
            buf[i + 2U] == 0x00 &&
            buf[i + 3U] == 0x01) {
            *start_code_len = 4U;
            return (int)i;
        }
    }

    return -1;
}

static int h265_buffer_starts_with_start_code(const uint8_t *buf,
                                              size_t len,
                                              size_t *start_code_len)
{
    if (buf == NULL || len < 3U) {
        return 0;
    }

    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) {
        if (start_code_len != NULL) {
            *start_code_len = 3U;
        }
        return 1;
    }

    if (len >= 4U &&
        buf[0] == 0x00 &&
        buf[1] == 0x00 &&
        buf[2] == 0x00 &&
        buf[3] == 0x01) {
        if (start_code_len != NULL) {
            *start_code_len = 4U;
        }
        return 1;
    }

    return 0;
}

static int h265_nalu_is_irap(int nal_type)
{
    return nal_type >= H265_NAL_BLA_W_LP && nal_type <= H265_NAL_CRA_NUT;
}

static int h265_append_annexb_nalu(h265_byte_buffer_t *dst,
                                   const uint8_t *nalu,
                                   size_t nalu_len)
{
    while (nalu_len > 0 && nalu[nalu_len - 1U] == 0x00) {
        nalu_len--;
    }

    if (nalu_len < 2U) {
        return 0;
    }

    if (h265_buffer_append_start_code(dst) != 0 ||
        h265_buffer_append(dst, nalu, nalu_len) != 0) {
        return -1;
    }

    return 0;
}

static void h265_snapshot_update_param_set(int nal_type,
                                           const uint8_t *annexb_nalu,
                                           size_t annexb_len,
                                           h265_snapshot_frame_t *frame)
{
    h265_byte_buffer_t *target = NULL;

    if (nal_type == H265_NAL_VPS) {
        target = &g_h265_snapshot_cache.vps;
        frame->has_vps = 1;
    } else if (nal_type == H265_NAL_SPS) {
        target = &g_h265_snapshot_cache.sps;
        frame->has_sps = 1;
    } else if (nal_type == H265_NAL_PPS) {
        target = &g_h265_snapshot_cache.pps;
        frame->has_pps = 1;
    }

    if (target != NULL && h265_buffer_replace(target, annexb_nalu, annexb_len) != 0) {
        printf("[snapshot] cache param set failed type=%d len=%lu\n",
               nal_type,
               (unsigned long)annexb_len);
    }
}

static int h265_snapshot_feed_nalu(h265_snapshot_frame_t *frame,
                                   const uint8_t *nalu,
                                   size_t nalu_len)
{
    size_t nalu_offset;
    size_t old_len;
    int nal_type;
    int is_param_set;

    if (frame == NULL || nalu == NULL || nalu_len < 2U) {
        return -1;
    }

    nal_type = h265_nalu_type(nalu, nalu_len);
    is_param_set = nal_type == H265_NAL_VPS ||
                   nal_type == H265_NAL_SPS ||
                   nal_type == H265_NAL_PPS;
    if (h265_nalu_is_irap(nal_type)) {
        frame->has_irap = 1;
    }

    if (!frame->copy_au &&
        !frame->has_irap &&
        !is_param_set &&
        !g_h265_snapshot_cache.current_gop_has_irap) {
        return 0;
    }

    nalu_offset = frame->au.len;
    if (h265_append_annexb_nalu(&frame->au, nalu, nalu_len) != 0) {
        return -1;
    }

    old_len = frame->au.len - nalu_offset;
    h265_snapshot_update_param_set(nal_type,
                                   frame->au.data + nalu_offset,
                                   old_len,
                                   frame);

    if (frame->has_irap || (!is_param_set && g_h265_snapshot_cache.current_gop_has_irap)) {
        frame->copy_au = 1;
    }

    return 0;
}

static int h265_snapshot_feed_buffer(h265_snapshot_frame_t *frame,
                                     const uint8_t *buf,
                                     size_t len)
{
    if (frame == NULL || buf == NULL || len < 2U) {
        return -1;
    }

    if (!h265_buffer_starts_with_start_code(buf, len, NULL)) {
        return h265_snapshot_feed_nalu(frame, buf, len);
    }

    {
        size_t search_offset = 0;
        int nalu_count = 0;

        while (1) {
            size_t sc_len = 0;
            size_t next_sc_len = 0;
            int sc_pos;
            int next_sc_pos;
            size_t nalu_start;
            size_t nalu_end;

            sc_pos = h265_find_start_code(buf, len, search_offset, &sc_len);
            if (sc_pos < 0) {
                break;
            }

            nalu_start = (size_t)sc_pos + sc_len;
            next_sc_pos = h265_find_start_code(buf, len, nalu_start, &next_sc_len);
            nalu_end = (next_sc_pos < 0) ? len : (size_t)next_sc_pos;

            while (nalu_end > nalu_start && buf[nalu_end - 1U] == 0x00) {
                nalu_end--;
            }

            if (nalu_end > nalu_start) {
                if (h265_snapshot_feed_nalu(frame,
                                            buf + nalu_start,
                                            nalu_end - nalu_start) != 0) {
                    return -1;
                }
                nalu_count++;
            }

            if (next_sc_pos < 0) {
                break;
            }
            search_offset = (size_t)next_sc_pos;
        }

        return (nalu_count > 0) ? 0 : -1;
    }
}

static int h265_snapshot_append_cached_params(h265_byte_buffer_t *out,
                                              const h265_byte_buffer_t *source)
{
    if (source != NULL && source->data != NULL && source->len > 0) {
        return h265_buffer_append(out, source->data, source->len);
    }

    return 0;
}

static int h265_snapshot_append_params(h265_byte_buffer_t *out)
{
    if (h265_snapshot_append_cached_params(out, &g_h265_snapshot_cache.vps) != 0 ||
        h265_snapshot_append_cached_params(out, &g_h265_snapshot_cache.sps) != 0 ||
        h265_snapshot_append_cached_params(out, &g_h265_snapshot_cache.pps) != 0) {
        return -1;
    }

    return 0;
}

static void h265_snapshot_update_gop_cache(const h265_snapshot_frame_t *frame,
                                           const mpp_nalu_ipc_msg *msg)
{
    if (frame == NULL || msg == NULL || frame->au.data == NULL || frame->au.len == 0) {
        return;
    }

    if (frame->has_irap) {
        h265_buffer_reset(&g_h265_snapshot_cache.current_gop);
        g_h265_snapshot_cache.current_gop_has_irap = 1;
        g_h265_snapshot_cache.current_gop_pts = msg->frame_pts;
        g_h265_snapshot_cache.current_gop_start_seq = msg->seq;
    } else if (!g_h265_snapshot_cache.current_gop_has_irap) {
        return;
    }

    if (frame->au.len > H265_SNAPSHOT_MAX_GOP_BYTES ||
        g_h265_snapshot_cache.current_gop.len >
        H265_SNAPSHOT_MAX_GOP_BYTES - frame->au.len) {
        printf("[snapshot] drop cached GOP: seq=%llu len=%lu next=%lu max=%u\n",
               (unsigned long long)msg->seq,
               (unsigned long)g_h265_snapshot_cache.current_gop.len,
               (unsigned long)frame->au.len,
               (unsigned int)H265_SNAPSHOT_MAX_GOP_BYTES);
        h265_buffer_reset(&g_h265_snapshot_cache.current_gop);
        g_h265_snapshot_cache.current_gop_has_irap = 0;
        return;
    }

    if (h265_buffer_append(&g_h265_snapshot_cache.current_gop,
                           frame->au.data,
                           frame->au.len) == 0) {
        g_h265_snapshot_cache.current_gop_last_seq = msg->seq;
    } else {
        printf("[snapshot] cache GOP failed seq=%llu len=%lu\n",
               (unsigned long long)msg->seq,
               (unsigned long)frame->au.len);
        h265_buffer_reset(&g_h265_snapshot_cache.current_gop);
        g_h265_snapshot_cache.current_gop_has_irap = 0;
    }
}

static int h265_snapshot_build_output(const h265_snapshot_frame_t *frame,
                                      h265_byte_buffer_t *out,
                                      int *used_cached_gop)
{
    int have_params;

    if (frame == NULL || out == NULL) {
        return -1;
    }
    (void)frame;

    h265_buffer_reset(out);
    if (used_cached_gop != NULL) {
        *used_cached_gop = 0;
    }

    have_params = g_h265_snapshot_cache.vps.len > 0 &&
                  g_h265_snapshot_cache.sps.len > 0 &&
                  g_h265_snapshot_cache.pps.len > 0;

    if (!have_params) {
        return -1;
    }

    if (g_h265_snapshot_cache.current_gop_has_irap &&
        g_h265_snapshot_cache.current_gop.data != NULL &&
        g_h265_snapshot_cache.current_gop.len > 0) {
        if (used_cached_gop != NULL) {
            *used_cached_gop = 1;
        }
        return h265_snapshot_append_params(out) == 0 ?
               h265_buffer_append(out,
                                  g_h265_snapshot_cache.current_gop.data,
                                  g_h265_snapshot_cache.current_gop.len) :
               -1;
    }

    if (g_h265_snapshot_cache.last_irap.data != NULL &&
        g_h265_snapshot_cache.last_irap.len > 0) {
        return h265_snapshot_append_params(out) == 0 ?
               h265_buffer_append(out,
                                  g_h265_snapshot_cache.last_irap.data,
                                  g_h265_snapshot_cache.last_irap.len) :
               -1;
    }

    return -1;
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

static void datafifo_pending_clear(datafifo_pending_item_t *pending)
{
    if (pending == NULL) {
        return;
    }

    pending->msg = NULL;
    pending->item = NULL;
}

static int datafifo_read_latest(nalu_datafifo_reader_t *reader,
                                datafifo_pending_item_t *latest,
                                unsigned int *dropped_count)
{
    const mpp_nalu_ipc_msg *msg = NULL;
    void *item = NULL;
    unsigned int dropped = 0;
    int got = 0;

    if (latest == NULL) {
        return -1;
    }

    datafifo_pending_clear(latest);
    if (dropped_count != NULL) {
        *dropped_count = 0;
    }

    while (nalu_datafifo_read(reader, &msg, &item) == 0) {
        if (got) {
            if (nalu_datafifo_validate_msg(latest->msg) == 0) {
                process_datafifo_snapshot_frame(latest->msg);
            }
            nalu_datafifo_read_done(reader, latest->item);
            dropped++;
        }
        latest->msg = msg;
        latest->item = item;
        got = 1;
    }

    if (dropped_count != NULL) {
        *dropped_count = dropped;
    }

    return got ? 0 : -1;
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

static int process_datafifo_snapshot_frame(const mpp_nalu_ipc_msg *msg)
{
    h265_snapshot_frame_t frame;
    h265_byte_buffer_t out;
    int used_cached_gop = 0;
    k_u32 i;
    int ret;

    if (msg == NULL) {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    memset(&out, 0, sizeof(out));
    frame.copy_au = (msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT) ? 1 : 0;

    for (i = 0; i < msg->pack_cnt; i++) {
        void *virt_addr = nalu_datafifo_mmap_pack(&msg->packs[i]);
        if (virt_addr == NULL) {
            printf("[snapshot] mmap pack[%u] failed while parsing seq=%llu\n",
                   i,
                   (unsigned long long)msg->seq);
            free(frame.au.data);
            return -1;
        }

        if (h265_snapshot_feed_buffer(&frame,
                                      (const uint8_t *)virt_addr,
                                      msg->packs[i].len) != 0) {
            printf("[snapshot] parse pack[%u] failed seq=%llu len=%u\n",
                   i,
                   (unsigned long long)msg->seq,
                   msg->packs[i].len);
        }

        if (nalu_datafifo_munmap_pack(&msg->packs[i], virt_addr) != 0) {
            printf("[snapshot] munmap pack[%u] failed for seq=%llu\n",
                   i,
                   (unsigned long long)msg->seq);
        }
    }

    h265_snapshot_update_gop_cache(&frame, msg);

    if (frame.has_irap && frame.au.len > 0) {
        if (h265_buffer_replace(&g_h265_snapshot_cache.last_irap,
                                frame.au.data,
                                frame.au.len) == 0) {
            g_h265_snapshot_cache.last_irap_pts = msg->frame_pts;
            g_h265_snapshot_cache.last_irap_seq = msg->seq;
            g_h265_snapshot_cache.last_irap_has_vps = frame.has_vps;
            g_h265_snapshot_cache.last_irap_has_sps = frame.has_sps;
            g_h265_snapshot_cache.last_irap_has_pps = frame.has_pps;
        } else {
            printf("[snapshot] cache IRAP failed seq=%llu len=%lu\n",
                   (unsigned long long)msg->seq,
                   (unsigned long)frame.au.len);
        }
    }

    if ((msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT) == 0) {
        free(frame.au.data);
        return 0;
    }

    if (!g_snapshot_ready) {
        printf("[snapshot] drop datafifo snapshot seq=%llu flags=0x%x: writer not ready\n",
               (unsigned long long)msg->seq,
               msg->reserved);
        free(frame.au.data);
        return -1;
    }

    ret = h265_snapshot_build_output(&frame, &out, &used_cached_gop);
    if (ret != 0 || out.len == 0) {
        printf("[snapshot] cannot build playable stream seq=%llu frame_irap=%d cached_irap=%lu cached_gop=%lu params=%d/%d/%d\n",
               (unsigned long long)msg->seq,
               frame.has_irap,
               (unsigned long)g_h265_snapshot_cache.last_irap.len,
               (unsigned long)g_h265_snapshot_cache.current_gop.len,
               g_h265_snapshot_cache.vps.len > 0,
               g_h265_snapshot_cache.sps.len > 0,
               g_h265_snapshot_cache.pps.len > 0);
        free(out.data);
        free(frame.au.data);
        return -1;
    }

    ret = snapshot_writer_enqueue_h265(out.data,
                                       out.len,
                                       used_cached_gop ?
                                       g_h265_snapshot_cache.current_gop_pts :
                                       msg->frame_pts,
                                       used_cached_gop ?
                                       "datafifo-cached-gop" :
                                       "datafifo-irap");
    if (ret == 0) {
        printf("[snapshot] captured datafifo seq=%llu flags=0x%x len=%lu frame_irap=%d cached_gop=%d gop_seq=%llu-%llu params=%d/%d/%d\n",
               (unsigned long long)msg->seq,
               msg->reserved,
               (unsigned long)out.len,
               frame.has_irap,
               used_cached_gop,
               (unsigned long long)g_h265_snapshot_cache.current_gop_start_seq,
               (unsigned long long)g_h265_snapshot_cache.current_gop_last_seq,
               g_h265_snapshot_cache.vps.len > 0,
               g_h265_snapshot_cache.sps.len > 0,
               g_h265_snapshot_cache.pps.len > 0);
    }

    free(out.data);
    free(frame.au.data);
    return ret;
}

static void *datafifo_rtp_sender_loop(int sock)
{
    nalu_datafifo_reader_t reader;
    uint16_t seq = 0;
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;
    uint64_t last_sent_seq = 0;
    unsigned int frame_count = 0;

    if (nalu_datafifo_open(&reader, g_fifo_phy_addr) != 0) {
        printf("[datafifo] open failed, RTP sender stopped\n");
        return NULL;
    }

    while (1) {
        datafifo_pending_item_t latest;
        struct sockaddr_in dest;
        int playing = 0;
        int dest_valid = 0;
        unsigned int dropped_count = 0;
        uint64_t send_start_ms = 0;
        uint64_t send_cost_ms = 0;
        k_u32 i;

        if (datafifo_read_latest(&reader, &latest, &dropped_count) != 0) {
            usleep(NALU_DATAFIFO_READ_IDLE_US);
            continue;
        }

        get_rtsp_target(&playing, &dest, &dest_valid);

        if (nalu_datafifo_validate_msg(latest.msg) == 0) {
            process_datafifo_snapshot_frame(latest.msg);

            if (playing && dest_valid) {
                send_start_ms = monotonic_ms();
                for (i = 0; i < latest.msg->pack_cnt; i++) {
                    uint8_t marker = (i + 1U == latest.msg->pack_cnt) ? 1 : 0;

                    if (send_datafifo_pack(sock,
                                           &dest,
                                           &latest.msg->packs[i],
                                           &seq,
                                           timestamp,
                                           ssrc,
                                           marker) != 0) {
                        printf("[datafifo] send pack[%u] failed: phys=0x%llx len=%u\n",
                               i,
                               (unsigned long long)latest.msg->packs[i].phys_addr,
                               latest.msg->packs[i].len);
                    }
                }
                send_cost_ms = monotonic_ms() - send_start_ms;
                timestamp += RTP_TS_STEP;
                frame_count++;
                log_datafifo_stats(latest.msg,
                                   1,
                                   dropped_count,
                                   last_sent_seq,
                                   send_cost_ms,
                                   frame_count,
                                   send_cost_ms > (1000U / VIDEO_FPS));
                last_sent_seq = latest.msg->seq;
            } else {
                frame_count++;
                log_datafifo_stats(latest.msg,
                                   0,
                                   dropped_count,
                                   last_sent_seq,
                                   0,
                                   frame_count,
                                   dropped_count > 0);
            }
        }

        /*
         * This is mandatory. The big core releases the VENC stream only after
         * the reader marks this DATAFIFO item as done.
         */
        nalu_datafifo_read_done(&reader, latest.item);
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
