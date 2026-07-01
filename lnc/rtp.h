#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "nalu_datafifo.h"

#define RTP_PAYLOAD_TYPE     96
#define RTP_CLOCK_RATE       90000
#define VIDEO_FPS            15
#define RTP_TS_STEP          (RTP_CLOCK_RATE / VIDEO_FPS)
#define SEND_INTERVAL_US     (1000000 / VIDEO_FPS)

#define RTP_SERVER_PORT      5004
#define RTCP_SERVER_PORT     5005

#define RTP_HEADER_SIZE      12
#define RTP_PACKET_MAX_SIZE  1500
#define RTP_MAX_PAYLOAD      1200

#define H265_FU_HEADER_SIZE  3
#define H265_FU_TYPE         49

typedef struct
{
    const uint8_t *data;
    size_t len;
} nalu_t;

int create_udp_socket_only(void);

int load_annexb_nalus(const uint8_t *buf,
                      size_t size,
                      nalu_t **out_nalus,
                      size_t *out_count);

int h265_nalu_type(const uint8_t *nalu, size_t len);

int send_h265_nalu_rtp(int sock,
                       const struct sockaddr_in *dest,
                       const uint8_t *nalu,
                       size_t nalu_len,
                       uint16_t *seq,
                       uint32_t timestamp,
                       uint32_t ssrc,
                       uint8_t marker);

int send_h265_buffer_rtp(int sock,
                         const struct sockaddr_in *dest,
                         const uint8_t *buf,
                         size_t len,
                         uint16_t *seq,
                         uint32_t timestamp,
                         uint32_t ssrc,
                         uint8_t last_marker);

int send_datafifo_pack(int sock,
                       const struct sockaddr_in *dest,
                       uint64_t datafifo_seq,
                       k_u32 pack_index,
                       const mpp_nalu_ipc_pack *pack,
                       uint16_t *seq,
                       uint32_t timestamp,
                       uint32_t ssrc,
                       uint8_t marker);

#endif
