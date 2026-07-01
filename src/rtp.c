#include "rtp.h"


int create_udp_socket_only(void)
{
    int sock;
    int opt = 1;
    struct sockaddr_in local_addr;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        printf("udp socket() failed, errno=%d\n", errno);
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(RTP_SERVER_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        printf("udp bind port %d failed, errno=%d\n", RTP_SERVER_PORT, errno);
        close(sock);
        return -1;
    }

    return sock;
}



//构建RTP头
static void build_rtp_header(uint8_t *packet,
                             uint16_t seq,
                             uint32_t timestamp,
                             uint32_t ssrc,
                             uint8_t marker)
{
    packet[0] = 0x80;
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | (RTP_PAYLOAD_TYPE & 0x7f));

    packet[2] = (uint8_t)((seq >> 8) & 0xff);
    packet[3] = (uint8_t)(seq & 0xff);

    packet[4] = (uint8_t)((timestamp >> 24) & 0xff);
    packet[5] = (uint8_t)((timestamp >> 16) & 0xff);
    packet[6] = (uint8_t)((timestamp >> 8) & 0xff);
    packet[7] = (uint8_t)(timestamp & 0xff);

    packet[8] = (uint8_t)((ssrc >> 24) & 0xff);
    packet[9] = (uint8_t)((ssrc >> 16) & 0xff);
    packet[10] = (uint8_t)((ssrc >> 8) & 0xff);
    packet[11] = (uint8_t)(ssrc & 0xff);
}

//通过查找起始码找到RTP包的位置和长度
static long find_start_code(const uint8_t *buf,
                            size_t size,
                            size_t offset,
                            size_t *start_code_len)
{
    size_t i;

    for (i = offset; i + 3 <= size; i++)
    {
        if (i + 3 <= size &&
            buf[i] == 0x00 &&
            buf[i + 1] == 0x00 &&
            buf[i + 2] == 0x01)
        {
            *start_code_len = 3;
            return (long)i;
        }

        if (i + 4 <= size &&
            buf[i] == 0x00 &&
            buf[i + 1] == 0x00 &&
            buf[i + 2] == 0x00 &&
            buf[i + 3] == 0x01)
        {
            *start_code_len = 4;
            return (long)i;
        }
    }

    return -1;
}

static int buffer_starts_with_start_code(const uint8_t *buf,
                                         size_t len,
                                         size_t *start_code_len)
{
    if (buf == NULL || len < 3 || start_code_len == NULL)
    {
        return 0;
    }

    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01)
    {
        *start_code_len = 3;
        return 1;
    }

    if (len >= 4 &&
        buf[0] == 0x00 &&
        buf[1] == 0x00 &&
        buf[2] == 0x00 &&
        buf[3] == 0x01)
    {
        *start_code_len = 4;
        return 1;
    }

    return 0;
}

//把NALU从文件中读取出来并保存到数组中
int load_annexb_nalus(const uint8_t *buf,
                             size_t size,
                             nalu_t **out_nalus,
                             size_t *out_count)
{
    nalu_t *nalus = NULL;
    size_t capacity = 0;
    size_t count = 0;
    size_t search_offset = 0;

    while (1)
    {
        size_t sc_len = 0;
        size_t next_sc_len = 0;
        long sc_pos;
        long next_sc_pos;
        size_t nalu_start;
        size_t nalu_end;

        sc_pos = find_start_code(buf, size, search_offset, &sc_len);
        if (sc_pos < 0)
        {
            break;
        }

        nalu_start = (size_t)sc_pos + sc_len;
        next_sc_pos = find_start_code(buf, size, nalu_start, &next_sc_len);
        nalu_end = (next_sc_pos < 0) ? size : (size_t)next_sc_pos;

        while (nalu_end > nalu_start && buf[nalu_end - 1] == 0x00)
        {
            nalu_end--;
        }

        if (nalu_end > nalu_start)
        {
            if (count == capacity)
            {
                size_t new_capacity = (capacity == 0) ? 128 : capacity * 2;
                nalu_t *new_nalus = (nalu_t *)realloc(nalus, new_capacity * sizeof(nalu_t));
                if (new_nalus == NULL)
                {
                    free(nalus);
                    return -1;
                }
                nalus = new_nalus;
                capacity = new_capacity;
            }

            nalus[count].data = buf + nalu_start;
            nalus[count].len = nalu_end - nalu_start;
            count++;
        }

        if (next_sc_pos < 0)
        {
            break;
        }

        search_offset = (size_t)next_sc_pos;
    }

    *out_nalus = nalus;
    *out_count = count;
    return 0;
}

//获取NALU类型
int h265_nalu_type(const uint8_t *nalu, size_t len)
{
    if (len < 2)
    {
        return -1;
    }

    return (nalu[0] >> 1) & 0x3f;
}

//发送RTP包（未分片）
static int send_rtp_packet(int sock,
                           const struct sockaddr_in *dest,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint16_t *seq,
                           uint32_t timestamp,
                           uint32_t ssrc,
                           uint8_t marker)
{
    uint8_t packet[RTP_PACKET_MAX_SIZE];
    size_t packet_len;
    int sent_len;

    packet_len = RTP_HEADER_SIZE + payload_len;
    if (packet_len > sizeof(packet))
    {
        printf("RTP packet too large: %lu\n", (unsigned long)packet_len);
        return -1;
    }

    build_rtp_header(packet, *seq, timestamp, ssrc, marker);
    memcpy(packet + RTP_HEADER_SIZE, payload, payload_len);

    sent_len = sendto(sock,
                      packet,
                      packet_len,
                      0,
                      (const struct sockaddr *)dest,
                      sizeof(*dest));
    if (sent_len < 0)
    {
        printf("sendto failed, seq=%u errno=%d\n",
               (unsigned int)*seq,
               errno);
        return -1;
    }

    (*seq)++;
    return 0;
}

//
int send_h265_nalu_rtp(int sock,
                              const struct sockaddr_in *dest,
                              const uint8_t *nalu,
                              size_t nalu_len,
                              uint16_t *seq,
                              uint32_t timestamp,
                              uint32_t ssrc,
                              uint8_t marker)
{
    int nal_type;

    if (nalu == NULL || nalu_len < 2)
    {
        return -1;
    }

    nal_type = h265_nalu_type(nalu, nalu_len);

    if (nalu_len <= RTP_MAX_PAYLOAD)//不用分片，直接发送
    {
        return send_rtp_packet(sock, dest, nalu, nalu_len, seq, timestamp, ssrc, marker);
    }
    //需要分片
    {
        uint8_t fu_payload[RTP_MAX_PAYLOAD];
        size_t max_fragment_data = RTP_MAX_PAYLOAD - H265_FU_HEADER_SIZE;//分片数据最大容量
        size_t offset = 2;//头部两字节为头部
        int first = 1;

        while (offset < nalu_len)
        {
            size_t remaining = nalu_len - offset;
            size_t fragment_size = remaining > max_fragment_data ? max_fragment_data : remaining;
            int last = (offset + fragment_size) >= nalu_len;//判断是否为最后一次

            //设置NALU为分片类型
            fu_payload[0] = (uint8_t)((nalu[0] & 0x81) | (H265_FU_TYPE << 1));
            fu_payload[1] = nalu[1];
            fu_payload[2] = (uint8_t)((first ? 0x80 : 0x00) |
                                      (last ? 0x40 : 0x00) |
                                      (nal_type & 0x3f));
            
            memcpy(fu_payload + H265_FU_HEADER_SIZE, nalu + offset, fragment_size);

            if (send_rtp_packet(sock,
                                dest,
                                fu_payload,
                                H265_FU_HEADER_SIZE + fragment_size,
                                seq,
                                timestamp,
                                ssrc,
                                last ? marker : 0) != 0)
            {
                return -1;
            }

            first = 0;
            offset += fragment_size;
        }
    }

    return 0;
}

int send_h265_buffer_rtp(int sock,
                         const struct sockaddr_in *dest,
                         const uint8_t *buf,
                         size_t len,
                         uint16_t *seq,
                         uint32_t timestamp,
                         uint32_t ssrc,
                         uint8_t last_marker)
{
    size_t first_sc_len = 0;

    if (buf == NULL || len < 2)
    {
        return -1;
    }

    /*
     * VENC pack may be one raw NALU, one Annex-B NALU, or several Annex-B
     * NALUs in the same pack. RTP/H.265 payload must not include Annex-B
     * start codes, so split and strip them when present.
     */
    if (!buffer_starts_with_start_code(buf, len, &first_sc_len))
    {
        return send_h265_nalu_rtp(sock, dest, buf, len, seq, timestamp, ssrc, last_marker);
    }

    {
        size_t search_offset = 0;
        int sent_count = 0;

        while (1)
        {
            size_t sc_len = 0;
            size_t next_sc_len = 0;
            long sc_pos;
            long next_sc_pos;
            size_t nalu_start;
            size_t nalu_end;
            uint8_t packet_marker;

            sc_pos = find_start_code(buf, len, search_offset, &sc_len);
            if (sc_pos < 0)
            {
                break;
            }

            nalu_start = (size_t)sc_pos + sc_len;
            next_sc_pos = find_start_code(buf, len, nalu_start, &next_sc_len);
            nalu_end = (next_sc_pos < 0) ? len : (size_t)next_sc_pos;

            while (nalu_end > nalu_start && buf[nalu_end - 1] == 0x00)
            {
                nalu_end--;
            }

            if (nalu_end > nalu_start)
            {
                packet_marker = (next_sc_pos < 0) ? last_marker : 0;
                if (send_h265_nalu_rtp(sock,
                                       dest,
                                       buf + nalu_start,
                                       nalu_end - nalu_start,
                                       seq,
                                       timestamp,
                                       ssrc,
                                       packet_marker) != 0)
                {
                    return -1;
                }
                sent_count++;
            }

            if (next_sc_pos < 0)
            {
                break;
            }

            search_offset = (size_t)next_sc_pos;
        }

        return (sent_count > 0) ? 0 : -1;
    }
}

// 发送DATAFIFO包
int send_datafifo_pack(int sock,
                       const struct sockaddr_in *dest,
                       uint64_t datafifo_seq,
                       k_u32 pack_index,
                       const mpp_nalu_ipc_pack *pack,
                       uint16_t *seq,
                       uint32_t timestamp,
                       uint32_t ssrc,
                       uint8_t marker)
{
    void *virt_addr;
    int ret;
    int munmap_ret;

    virt_addr = nalu_datafifo_mmap_pack(pack);
    if (virt_addr == NULL) {
        printf("[datafifo] seq=%llu pack[%u] mmap failed phys=0x%llx len=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               pack ? (unsigned long long)pack->phys_addr : 0ULL,
               pack ? pack->len : 0U);
        return -1;
    }

    if (NALU_DATAFIFO_VERBOSE_LOG) {
        printf("[datafifo] seq=%llu pack[%u] mmap ok virt=%p phys=0x%llx len=%u marker=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               virt_addr,
               (unsigned long long)pack->phys_addr,
               pack->len,
               (unsigned int)marker);
    }

    ret = send_h265_buffer_rtp(sock,
                               dest,
                               (const uint8_t *)virt_addr,
                               pack->len,
                               seq,
                               timestamp,
                               ssrc,
                               marker);

    if (NALU_DATAFIFO_VERBOSE_LOG || ret != 0) {
        printf("[datafifo] seq=%llu pack[%u] rtp ret=%d next_rtp_seq=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               ret,
               seq ? (unsigned int)*seq : 0U);
    }

    munmap_ret = nalu_datafifo_munmap_pack(pack, virt_addr);
    if (NALU_DATAFIFO_VERBOSE_LOG || munmap_ret != 0) {
        printf("[datafifo] seq=%llu pack[%u] munmap ret=%d\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               munmap_ret);
    }
    if (munmap_ret != 0 && ret == 0) {
        ret = -1;
    }

    return ret;
}

