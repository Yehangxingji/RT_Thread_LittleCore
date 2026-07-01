#include "datafifo_snapshot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"
#include "snapshot_writer.h"

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

static int h265_buffer_starts_with_start_code(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < 3U) {
        return 0;
    }

    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) {
        return 1;
    }

    return len >= 4U &&
           buf[0] == 0x00 &&
           buf[1] == 0x00 &&
           buf[2] == 0x00 &&
           buf[3] == 0x01;
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

    if (!h265_buffer_starts_with_start_code(buf, len)) {
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

void datafifo_snapshot_deinit(void)
{
    free(g_h265_snapshot_cache.vps.data);
    free(g_h265_snapshot_cache.sps.data);
    free(g_h265_snapshot_cache.pps.data);
    free(g_h265_snapshot_cache.last_irap.data);
    free(g_h265_snapshot_cache.current_gop.data);
    memset(&g_h265_snapshot_cache, 0, sizeof(g_h265_snapshot_cache));
}

int datafifo_snapshot_process(const mpp_nalu_ipc_msg *msg, int writer_ready)
{
    h265_snapshot_frame_t frame;
    h265_byte_buffer_t out;
    int used_cached_gop = 0;
    k_u32 i;
    int ret;

    if (msg == NULL) {
        return -1;
    }

    if ((msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT) == 0) {
        return 0;
    }

    printf("[snapshot] seq=%llu begin flags=0x%x packs=%u total=%u writer=%d\n",
           (unsigned long long)msg->seq,
           msg->reserved,
           msg->pack_cnt,
           msg->total_len,
           writer_ready);

    memset(&frame, 0, sizeof(frame));
    memset(&out, 0, sizeof(out));
    frame.copy_au = 1;

    for (i = 0; i < msg->pack_cnt; i++) {
        void *virt_addr = nalu_datafifo_mmap_pack(&msg->packs[i]);
        if (virt_addr == NULL) {
            printf("[snapshot] seq=%llu pack[%u] mmap failed phys=0x%llx len=%u\n",
                   (unsigned long long)msg->seq,
                   i,
                   (unsigned long long)msg->packs[i].phys_addr,
                   msg->packs[i].len);
            free(frame.au.data);
            printf("[snapshot] seq=%llu done ret=-1\n",
                   (unsigned long long)msg->seq);
            return -1;
        }

        printf("[snapshot] seq=%llu pack[%u] mmap ok len=%u\n",
               (unsigned long long)msg->seq,
               i,
               msg->packs[i].len);

        if (h265_snapshot_feed_buffer(&frame,
                                      (const uint8_t *)virt_addr,
                                      msg->packs[i].len) != 0) {
            printf("[snapshot] seq=%llu pack[%u] parse failed len=%u\n",
                   (unsigned long long)msg->seq,
                   i,
                   msg->packs[i].len);
        }

        if (nalu_datafifo_munmap_pack(&msg->packs[i], virt_addr) != 0) {
            printf("[snapshot] seq=%llu pack[%u] munmap failed\n",
                   (unsigned long long)msg->seq,
                   i);
        } else {
            printf("[snapshot] seq=%llu pack[%u] munmap ok\n",
                   (unsigned long long)msg->seq,
                   i);
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

    if (!writer_ready) {
        printf("[snapshot] drop datafifo snapshot seq=%llu flags=0x%x: writer not ready\n",
               (unsigned long long)msg->seq,
               msg->reserved);
        free(frame.au.data);
        printf("[snapshot] seq=%llu done ret=-1\n",
               (unsigned long long)msg->seq);
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
        printf("[snapshot] seq=%llu done ret=-1\n",
               (unsigned long long)msg->seq);
        return -1;
    }

    {
        size_t queued_len = out.len;

    ret = snapshot_writer_enqueue_h265_take(out.data,
                                            out.len,
                                            used_cached_gop ?
                                            g_h265_snapshot_cache.current_gop_pts :
                                            msg->frame_pts,
                                            used_cached_gop ?
                                            "datafifo-cached-gop" :
                                            "datafifo-irap");
    out.data = NULL;
    out.len = 0;
    if (ret == 0) {
        printf("[snapshot] captured datafifo seq=%llu flags=0x%x len=%lu frame_irap=%d cached_gop=%d gop_seq=%llu-%llu params=%d/%d/%d\n",
               (unsigned long long)msg->seq,
               msg->reserved,
               (unsigned long)queued_len,
               frame.has_irap,
               used_cached_gop,
               (unsigned long long)g_h265_snapshot_cache.current_gop_start_seq,
               (unsigned long long)g_h265_snapshot_cache.current_gop_last_seq,
               g_h265_snapshot_cache.vps.len > 0,
               g_h265_snapshot_cache.sps.len > 0,
               g_h265_snapshot_cache.pps.len > 0);
    }
    }

    free(out.data);
    free(frame.au.data);
    printf("[snapshot] seq=%llu done ret=%d\n",
           (unsigned long long)msg->seq,
           ret);
    return ret;
}
