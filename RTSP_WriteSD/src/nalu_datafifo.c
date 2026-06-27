#include "nalu_datafifo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_RT_SMART
#include "mpi_sys_api.h"
#else
#include <sys/mman.h>

static int g_datafifo_mem_fd = -1;

static void *little_sys_mmap(k_u64 phys_addr, k_u32 size)
{
    void *mmap_addr;
    void *virt_addr;
    long page_size;
    k_u64 page_mask;
    size_t mmap_size;
    off_t mmap_offset;
    k_u64 page_offset;

    if (size == 0) {
        return NULL;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        printf("[datafifo] sysconf(_SC_PAGESIZE) failed\n");
        return NULL;
    }

    page_mask = (k_u64)page_size - 1U;
    page_offset = phys_addr & page_mask;
    mmap_size = (size_t)(((k_u64)size + page_offset + page_mask) & ~page_mask);
    mmap_offset = (off_t)(phys_addr & ~page_mask);

    if (g_datafifo_mem_fd < 0) {
        g_datafifo_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (g_datafifo_mem_fd < 0) {
            printf("[datafifo] open(/dev/mem) failed: %s\n", strerror(errno));
            return NULL;
        }
    }

    mmap_addr = mmap(NULL,
                     mmap_size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     g_datafifo_mem_fd,
                     mmap_offset);
    if (mmap_addr == MAP_FAILED) {
        printf("[datafifo] mmap failed: phys=0x%llx len=%u err=%s\n",
               (unsigned long long)phys_addr,
               (unsigned int)size,
               strerror(errno));
        return NULL;
    }

    virt_addr = (void *)((char *)mmap_addr + (size_t)page_offset);
    return virt_addr;
}

static k_s32 little_sys_munmap(k_u64 phys_addr, void *virt_addr, k_u32 size)
{
    long page_size;
    k_u64 page_mask;
    size_t mmap_size;
    void *mmap_addr;
    k_u64 page_offset;

    if (virt_addr == NULL || size == 0) {
        return -1;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return -1;
    }

    page_mask = (k_u64)page_size - 1U;
    page_offset = phys_addr & page_mask;
    mmap_size = (size_t)(((k_u64)size + page_offset + page_mask) & ~page_mask);
    mmap_addr = (void *)((char *)virt_addr - (size_t)page_offset);

    if (munmap(mmap_addr, mmap_size) != 0) {
        printf("[datafifo] munmap failed: virt=%p len=%u err=%s\n",
               virt_addr,
               (unsigned int)size,
               strerror(errno));
        return -1;
    }

    return 0;
}

static void little_sys_close_mmap_fd(void)
{
    if (g_datafifo_mem_fd >= 0) {
        close(g_datafifo_mem_fd);
        g_datafifo_mem_fd = -1;
    }
}
#endif

int nalu_datafifo_open(nalu_datafifo_reader_t *reader, k_u64 fifo_phy_addr)
{
    k_s32 ret;
    k_datafifo_params_s params;

    if (reader == NULL || fifo_phy_addr == 0) {
        return -1;
    }

    memset(reader, 0, sizeof(*reader));
    reader->handle = K_DATAFIFO_INVALID_HANDLE;

    memset(&params, 0, sizeof(params));
    params.u32EntriesNum = NALU_DATAFIFO_FIFO_ENTRIES;
    params.u32CacheLineSize = MPP_NALU_IPC_ITEM_SIZE;
    params.bDataReleaseByWriter = K_TRUE;
    params.enOpenMode = DATAFIFO_READER;

    ret = kd_datafifo_open_by_addr(&reader->handle, &params, fifo_phy_addr);
    if (ret != 0) {
        printf("[datafifo] kd_datafifo_open_by_addr failed, phy=0x%llx ret=0x%x\n",
               (unsigned long long)fifo_phy_addr,
               ret);
        reader->handle = K_DATAFIFO_INVALID_HANDLE;
        return -1;
    }

    reader->opened = 1;
    printf("[datafifo] opened reader, fifo phy=0x%llx entries=%u item_size=%u\n",
           (unsigned long long)fifo_phy_addr,
           (unsigned int)NALU_DATAFIFO_FIFO_ENTRIES,
           (unsigned int)MPP_NALU_IPC_ITEM_SIZE);

    return 0;
}

void nalu_datafifo_close(nalu_datafifo_reader_t *reader)
{
    if (reader != NULL && reader->opened) {
        kd_datafifo_close(reader->handle);
        reader->handle = K_DATAFIFO_INVALID_HANDLE;
        reader->opened = 0;
#ifndef USE_RT_SMART
        little_sys_close_mmap_fd();
#endif
    }
}

int nalu_datafifo_read(nalu_datafifo_reader_t *reader,
                       const mpp_nalu_ipc_msg **out_msg,
                       void **out_item)
{
    k_s32 ret;
    k_u32 read_len = 0;
    void *item = NULL;

    if (reader == NULL || !reader->opened || out_msg == NULL || out_item == NULL) {
        return -1;
    }

    ret = kd_datafifo_cmd(reader->handle, DATAFIFO_CMD_GET_AVAIL_READ_LEN, &read_len);
    if (ret != 0) {
        printf("[datafifo] DATAFIFO_CMD_GET_AVAIL_READ_LEN failed, ret=0x%x\n", ret);
        return -1;
    }

    if (read_len < MPP_NALU_IPC_ITEM_SIZE) {
        return -1;
    }

    if (read_len > NALU_DATAFIFO_FIFO_ENTRIES * MPP_NALU_IPC_ITEM_SIZE) {
        printf("[datafifo] bad avail read len=%u, max=%u\n",
               (unsigned int)read_len,
               (unsigned int)(NALU_DATAFIFO_FIFO_ENTRIES * MPP_NALU_IPC_ITEM_SIZE));
        return -1;
    }

    ret = kd_datafifo_read(reader->handle, &item);
    if (ret != 0 || item == NULL) {
        return -1;
    }

    *out_item = item;
    *out_msg = (const mpp_nalu_ipc_msg *)item;
    return 0;
}

int nalu_datafifo_read_done(nalu_datafifo_reader_t *reader, void *item)
{
    k_s32 ret;

    if (reader == NULL || !reader->opened || item == NULL) {
        return -1;
    }

    ret = kd_datafifo_cmd(reader->handle, DATAFIFO_CMD_READ_DONE, item);
    if (ret != 0) {
        printf("[datafifo] DATAFIFO_CMD_READ_DONE failed, ret=0x%x\n", ret);
        return -1;
    }

    return 0;
}

int nalu_datafifo_validate_msg(const mpp_nalu_ipc_msg *msg)
{
    k_u32 i;
    k_u32 sum_len = 0;

    if (msg == NULL) {
        return -1;
    }

    if (msg->magic != MPP_NALU_IPC_MAGIC) {
        printf("[datafifo] bad magic=0x%x\n", msg->magic);
        return -1;
    }

    if (msg->version != MPP_NALU_IPC_VERSION) {
        printf("[datafifo] bad version=%u\n", msg->version);
        return -1;
    }

    if (msg->pack_cnt == 0 || msg->pack_cnt > MPP_NALU_IPC_MAX_PACKS) {
        printf("[datafifo] bad pack_cnt=%u\n", msg->pack_cnt);
        return -1;
    }

    for (i = 0; i < msg->pack_cnt; i++) {
        if (msg->packs[i].phys_addr == 0 || msg->packs[i].len == 0) {
            printf("[datafifo] bad pack[%u]: phys=0x%llx len=%u\n",
                   i,
                   (unsigned long long)msg->packs[i].phys_addr,
                   msg->packs[i].len);
            return -1;
        }
        if (msg->packs[i].len > 8U * 1024U * 1024U ||
            sum_len > 0xffffffffU - msg->packs[i].len) {
            printf("[datafifo] unreasonable pack[%u] len=%u\n",
                   i,
                   msg->packs[i].len);
            return -1;
        }
        sum_len += msg->packs[i].len;
    }

    if (msg->total_len != 0 && msg->total_len != sum_len) {
        printf("[datafifo] total_len mismatch: total=%u sum=%u\n",
               msg->total_len,
               sum_len);
        return -1;
    }

    return 0;
}

void *nalu_datafifo_mmap_pack(const mpp_nalu_ipc_pack *pack)
{
    void *virt_addr;

    if (pack == NULL || pack->phys_addr == 0 || pack->len == 0) {
        return NULL;
    }

#ifdef USE_RT_SMART
    virt_addr = kd_mpi_sys_mmap(pack->phys_addr, pack->len);
    if (virt_addr == NULL) {
        printf("[datafifo] kd_mpi_sys_mmap failed: phys=0x%llx len=%u\n",
               (unsigned long long)pack->phys_addr,
               pack->len);
    }
#else
    virt_addr = little_sys_mmap(pack->phys_addr, pack->len);
    if (virt_addr == NULL) {
        printf("[datafifo] little_sys_mmap failed: phys=0x%llx len=%u\n",
               (unsigned long long)pack->phys_addr,
               pack->len);
    }
#endif

    return virt_addr;
}

int nalu_datafifo_munmap_pack(const mpp_nalu_ipc_pack *pack, void *virt_addr)
{
    k_s32 ret;

    if (pack == NULL || virt_addr == NULL || pack->len == 0) {
        return -1;
    }

#ifdef USE_RT_SMART
    ret = kd_mpi_sys_munmap(virt_addr, pack->len);
    if (ret != 0) {
        printf("[datafifo] kd_mpi_sys_munmap failed: virt=%p len=%u ret=0x%x\n",
               virt_addr,
               pack->len,
               ret);
        return -1;
    }
#else
    ret = little_sys_munmap(pack->phys_addr, virt_addr, pack->len);
    if (ret != 0) {
        return -1;
    }
#endif

    return 0;
}
