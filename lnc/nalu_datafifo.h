#ifndef NALU_DATAFIFO_H
#define NALU_DATAFIFO_H

#include <stddef.h>
#include <stdint.h>

#include "k_type.h"
#include "k_datafifo.h"

#define NALU_DATAFIFO_EXPECTED_ITEM_SIZE 512U

#if defined(__has_include)
#if __has_include("mpp_nalu_ipc.h")
#include "mpp_nalu_ipc.h"
#endif
#endif

#ifndef MPP_NALU_IPC_H
#define MPP_NALU_IPC_H

#define MPP_NALU_IPC_MAGIC      0x4E414C55U  /* "NALU" */
#define MPP_NALU_IPC_VERSION    1U
#define MPP_NALU_IPC_MAX_PACKS  8U
#define MPP_NALU_IPC_ITEM_SIZE  NALU_DATAFIFO_EXPECTED_ITEM_SIZE

typedef struct {
    k_u64 phys_addr;
    k_u64 pts;
    k_u32 len;
    k_u32 type;
} mpp_nalu_ipc_pack;

typedef struct {
    k_u32 magic;
    k_u32 version;
    k_u32 chn;
    k_u32 pack_cnt;
    k_u64 seq;
    k_u64 frame_pts;
    k_u64 submit_time_ms;
    k_u32 total_len;
    k_u32 reserved;
    mpp_nalu_ipc_pack packs[MPP_NALU_IPC_MAX_PACKS];
} mpp_nalu_ipc_msg;

#endif /* MPP_NALU_IPC_H */

#ifndef MPP_NALU_IPC_FLAG_SNAPSHOT
#define MPP_NALU_IPC_FLAG_SNAPSHOT  (1U << 0)
#endif

#if MPP_NALU_IPC_ITEM_SIZE != NALU_DATAFIFO_EXPECTED_ITEM_SIZE
#error "MPP_NALU_IPC_ITEM_SIZE must match the big-core DATAFIFO item size: 512"
#endif

typedef char nalu_datafifo_msg_must_fit_item[
    (sizeof(mpp_nalu_ipc_msg) <= MPP_NALU_IPC_ITEM_SIZE) ? 1 : -1
];

#define NALU_DATAFIFO_FIFO_ENTRIES 3U
#define NALU_DATAFIFO_READ_IDLE_US 1000U

typedef struct {
    k_datafifo_handle handle;
    int opened;
} nalu_datafifo_reader_t;

int nalu_datafifo_open(nalu_datafifo_reader_t *reader, k_u64 fifo_phy_addr);
void nalu_datafifo_close(nalu_datafifo_reader_t *reader);

int nalu_datafifo_read(nalu_datafifo_reader_t *reader,
                       const mpp_nalu_ipc_msg **out_msg,
                       void **out_item);

int nalu_datafifo_read_done(nalu_datafifo_reader_t *reader, void *item);
int nalu_datafifo_validate_msg(const mpp_nalu_ipc_msg *msg);

void *nalu_datafifo_mmap_pack(const mpp_nalu_ipc_pack *pack);
int nalu_datafifo_munmap_pack(const mpp_nalu_ipc_pack *pack, void *virt_addr);

#endif /* NALU_DATAFIFO_H */
