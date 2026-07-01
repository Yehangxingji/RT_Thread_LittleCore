#ifndef DATAFIFO_SNAPSHOT_H
#define DATAFIFO_SNAPSHOT_H

#include "nalu_datafifo.h"

typedef struct {
    const uint8_t *data;
    size_t len;
    k_u64 pts;
    k_u32 type;
} datafifo_copied_pack_t;

typedef struct {
    k_u32 chn;
    k_u32 pack_cnt;
    k_u64 seq;
    k_u64 frame_pts;
    k_u64 submit_time_ms;
    k_u32 total_len;
    k_u32 reserved;
    datafifo_copied_pack_t packs[MPP_NALU_IPC_MAX_PACKS];
} datafifo_copied_frame_t;

int datafifo_snapshot_process(const mpp_nalu_ipc_msg *msg, int writer_ready);
int datafifo_snapshot_process_copied(const datafifo_copied_frame_t *frame, int writer_ready);
void datafifo_snapshot_deinit(void);

#endif /* DATAFIFO_SNAPSHOT_H */
