#include "snapshot_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define SNAPSHOT_QUEUE_DEPTH 4U
#define SNAPSHOT_PATH_MAX    256U
#define SNAPSHOT_CMD_MAX     1024U
#define SNAPSHOT_IMAGE_EXT   "jpg"
#ifndef SNAPSHOT_FFMPEG_BIN
#define SNAPSHOT_FFMPEG_BIN  "ffmpeg"
#endif

typedef struct {
    uint8_t *data;
    size_t len;
    uint64_t pts;
    char reason[32];
} snapshot_job_t;

static pthread_mutex_t g_snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_snapshot_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_snapshot_thread;
static int g_snapshot_started;
static int g_snapshot_stop;
static snapshot_job_t g_snapshot_queue[SNAPSHOT_QUEUE_DEPTH];
static unsigned int g_snapshot_head;
static unsigned int g_snapshot_tail;
static unsigned int g_snapshot_count;
static unsigned int g_snapshot_seq;
static char g_snapshot_dir[SNAPSHOT_PATH_MAX] = SNAPSHOT_DEFAULT_DIR;

static int snapshot_mkdir_one(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0775);
#endif
}


// 递归创建目录
static int snapshot_mkdir_p(const char *dir)
{
    char path[SNAPSHOT_PATH_MAX];
    size_t len;
    size_t i;

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }

    len = strlen(dir);
    if (len >= sizeof(path)) {
        return -1;
    }

    memcpy(path, dir, len + 1U);
    for (i = 1; i < len; i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            if (path[0] != '\0' && snapshot_mkdir_one(path) != 0 && errno != EEXIST) {
                printf("[snapshot] mkdir %s failed: %s\n", path, strerror(errno));
                return -1;
            }
            path[i] = '/';
        }
    }

    if (snapshot_mkdir_one(path) != 0 && errno != EEXIST) {
        printf("[snapshot] mkdir %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

// 释放快照任务
static void snapshot_free_job(snapshot_job_t *job)
{
    if (job == NULL) {
        return;
    }

    free(job->data);
    memset(job, 0, sizeof(*job));
}

// 从队列中弹出快照任务
static int snapshot_pop_job(snapshot_job_t *job)
{
    if (g_snapshot_count == 0) {
        return -1;
    }

    *job = g_snapshot_queue[g_snapshot_head];
    memset(&g_snapshot_queue[g_snapshot_head], 0, sizeof(g_snapshot_queue[g_snapshot_head]));
    g_snapshot_head = (g_snapshot_head + 1U) % SNAPSHOT_QUEUE_DEPTH;
    g_snapshot_count--;
    return 0;
}

static int snapshot_open_unique_h265(char *h265_path,
                                     size_t h265_path_size,
                                     char *image_path,
                                     size_t image_path_size)
{
    int fd;
    int attempt;

    for (attempt = 0; attempt < 1000000; attempt++) {
        int h265_ret;
        int image_ret;
        unsigned int seq;

        pthread_mutex_lock(&g_snapshot_mutex);
        seq = ++g_snapshot_seq;
        pthread_mutex_unlock(&g_snapshot_mutex);

        h265_ret = snprintf(h265_path,
                            h265_path_size,
                            "%s/snapshot_%06u.h265",
                            g_snapshot_dir,
                            seq);
        image_ret = snprintf(image_path,
                             image_path_size,
                             "%s/snapshot_%06u.%s",
                             g_snapshot_dir,
                             seq,
                             SNAPSHOT_IMAGE_EXT);
        if (h265_ret < 0 || (size_t)h265_ret >= h265_path_size ||
            image_ret < 0 || (size_t)image_ret >= image_path_size) {
            printf("[snapshot] path too long: dir=%s seq=%u\n", g_snapshot_dir, seq);
            return -1;
        }

        if (access(image_path, F_OK) == 0) {
            continue;
        }

        fd = open(h265_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd >= 0) {
            return fd;
        }

        if (errno != EEXIST) {
            printf("[snapshot] open %s failed: %s\n", h265_path, strerror(errno));
            return -1;
        }
    }

    printf("[snapshot] cannot allocate unique snapshot file in %s\n", g_snapshot_dir);
    return -1;
}

static int snapshot_write_all(int fd, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        offset += (size_t)written;
    }

    return 0;
}
static int snapshot_append_text(char *dst, size_t dst_size, size_t *offset, const char *src)
{
    size_t src_len;

    if (dst == NULL || offset == NULL || src == NULL) {
        return -1;
    }

    src_len = strlen(src);
    if (*offset + src_len >= dst_size) {
        return -1;
    }

    memcpy(dst + *offset, src, src_len);
    *offset += src_len;
    dst[*offset] = '\0';
    return 0;
}

static int snapshot_shell_quote(char *dst, size_t dst_size, const char *src)
{
    size_t offset = 0;
    const char *p;

    if (snapshot_append_text(dst, dst_size, &offset, "'") != 0) {
        return -1;
    }

    for (p = src; p != NULL && *p != '\0'; p++) {
        if (*p == '\'') {
            if (snapshot_append_text(dst, dst_size, &offset, "'\\''") != 0) {
                return -1;
            }
        } else {
            if (offset + 1U >= dst_size) {
                return -1;
            }
            dst[offset++] = *p;
            dst[offset] = '\0';
        }
    }

    return snapshot_append_text(dst, dst_size, &offset, "'");
}

static int snapshot_convert_h265_to_image(const char *h265_path, const char *image_path)
{
    char quoted_h265[SNAPSHOT_CMD_MAX];
    char quoted_image[SNAPSHOT_CMD_MAX];
    char cmd[SNAPSHOT_CMD_MAX];
    int ret;

    if (snapshot_shell_quote(quoted_h265, sizeof(quoted_h265), h265_path) != 0 ||
        snapshot_shell_quote(quoted_image, sizeof(quoted_image), image_path) != 0) {
        printf("[snapshot] build ffmpeg command failed: path too long\n");
        return -1;
    }

    ret = snprintf(cmd,
                   sizeof(cmd),
                   "%s -hide_banner -loglevel error -y -f hevc -i %s -frames:v 1 %s",
                   SNAPSHOT_FFMPEG_BIN,
                   quoted_h265,
                   quoted_image);
    if (ret < 0 || (size_t)ret >= sizeof(cmd)) {
        printf("[snapshot] ffmpeg command too long\n");
        return -1;
    }

    ret = system(cmd);
    if (ret != 0) {
        printf("[snapshot] convert %s to %s failed, ret=%d\n",
               h265_path,
               image_path,
               ret);
        return -1;
    }

    printf("[snapshot] converted %s\n", image_path);
    return 0;
}


static int snapshot_write_file(const snapshot_job_t *job)
{
    char h265_path[SNAPSHOT_PATH_MAX];
    char image_path[SNAPSHOT_PATH_MAX];
    int fd;

    fd = snapshot_open_unique_h265(h265_path,
                                   sizeof(h265_path),
                                   image_path,
                                   sizeof(image_path));
    if (fd < 0) {
        return -1;
    }

    if (snapshot_write_all(fd, job->data, job->len) != 0) {
        printf("[snapshot] write %s failed: %s\n", h265_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (close(fd) != 0) {
        printf("[snapshot] close %s failed: %s\n", h265_path, strerror(errno));
        return -1;
    }

    printf("[snapshot] saved stream %s len=%lu pts=%llu reason=%s\n",
           h265_path,
           (unsigned long)job->len,
           (unsigned long long)job->pts,
           job->reason);

    if (snapshot_convert_h265_to_image(h265_path, image_path) == 0) {
        if (unlink(h265_path) != 0) {
            printf("[snapshot] unlink temp %s failed: %s\n", h265_path, strerror(errno));
        }
        printf("[snapshot] saved image %s pts=%llu reason=%s\n",
               image_path,
               (unsigned long long)job->pts,
               job->reason);
        return 0;
    }

    printf("[snapshot] kept stream %s because image conversion failed\n", h265_path);
    return -1;
}

// 快照写入线程
static void *snapshot_writer_loop(void *arg)
{
    (void)arg;

#ifdef __linux__
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5) != 0) {
        printf("[snapshot] set low priority failed: %s\n", strerror(errno));
    }
#endif

    while (1) {
        snapshot_job_t job;

        memset(&job, 0, sizeof(job));
        pthread_mutex_lock(&g_snapshot_mutex);
        while (!g_snapshot_stop && g_snapshot_count == 0) {
            pthread_cond_wait(&g_snapshot_cond, &g_snapshot_mutex);
        }

        if (g_snapshot_stop && g_snapshot_count == 0) {
            pthread_mutex_unlock(&g_snapshot_mutex);
            break;
        }

        snapshot_pop_job(&job);
        pthread_mutex_unlock(&g_snapshot_mutex);

        if (job.data != NULL && job.len > 0) {
            snapshot_write_file(&job);
        }
        snapshot_free_job(&job);
    }

    return NULL;
}

// 初始化快照写入线程
int snapshot_writer_init(const char *dir)
{
    const char *target_dir = dir;

    if (target_dir == NULL || target_dir[0] == '\0') {
        target_dir = SNAPSHOT_DEFAULT_DIR;
    }

    if (strlen(target_dir) >= sizeof(g_snapshot_dir)) {
        printf("[snapshot] dir too long: %s\n", target_dir);
        return -1;
    }

    if (snapshot_mkdir_p(target_dir) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_snapshot_mutex);
    if (g_snapshot_started) {
        pthread_mutex_unlock(&g_snapshot_mutex);
        return 0;
    }

    strncpy(g_snapshot_dir, target_dir, sizeof(g_snapshot_dir) - 1U);
    g_snapshot_dir[sizeof(g_snapshot_dir) - 1U] = '\0';
    g_snapshot_stop = 0;
    g_snapshot_head = 0;
    g_snapshot_tail = 0;
    g_snapshot_count = 0;
    g_snapshot_seq = 0;
    pthread_mutex_unlock(&g_snapshot_mutex);

    if (pthread_create(&g_snapshot_thread, NULL, snapshot_writer_loop, NULL) != 0) {
        printf("[snapshot] pthread_create failed\n");
        return -1;
    }

    pthread_mutex_lock(&g_snapshot_mutex);
    g_snapshot_started = 1;
    pthread_mutex_unlock(&g_snapshot_mutex);

    printf("[snapshot] writer started dir=%s queue=%u\n",
           g_snapshot_dir,
           SNAPSHOT_QUEUE_DEPTH);
    return 0;
}

// 入队快照任务
int snapshot_writer_enqueue_h265(const uint8_t *data,
                                 size_t len,
                                 uint64_t pts,
                                 const char *reason)
{
    uint8_t *copy;

    if (data == NULL || len == 0) {
        return -1;
    }

    copy = (uint8_t *)malloc(len);
    if (copy == NULL) {
        printf("[snapshot] malloc failed len=%lu\n", (unsigned long)len);
        return -1;
    }

    memcpy(copy, data, len);
    return snapshot_writer_enqueue_h265_take(copy, len, pts, reason);
}

int snapshot_writer_enqueue_h265_take(uint8_t *data,
                                      size_t len,
                                      uint64_t pts,
                                      const char *reason)
{
    snapshot_job_t job;

    if (data == NULL || len == 0) {
        free(data);
        return -1;
    }

    memset(&job, 0, sizeof(job));
    job.data = data;
    job.len = len;
    job.pts = pts;
    snprintf(job.reason, sizeof(job.reason), "%s", reason ? reason : "manual");

    pthread_mutex_lock(&g_snapshot_mutex);
    if (!g_snapshot_started || g_snapshot_stop) {
        pthread_mutex_unlock(&g_snapshot_mutex);
        snapshot_free_job(&job);
        return -1;
    }

    if (g_snapshot_count >= SNAPSHOT_QUEUE_DEPTH) {
        pthread_mutex_unlock(&g_snapshot_mutex);
        printf("[snapshot] queue full, drop len=%lu reason=%s\n",
               (unsigned long)len,
               job.reason);
        snapshot_free_job(&job);
        return -1;
    }

    g_snapshot_queue[g_snapshot_tail] = job;
    g_snapshot_tail = (g_snapshot_tail + 1U) % SNAPSHOT_QUEUE_DEPTH;
    g_snapshot_count++;
    pthread_cond_signal(&g_snapshot_cond);
    pthread_mutex_unlock(&g_snapshot_mutex);

    printf("[snapshot] queued len=%lu pts=%llu reason=%s\n",
           (unsigned long)len,
           (unsigned long long)pts,
           job.reason);
    return 0;
}

// 初始化快照写入线程
void snapshot_writer_deinit(void)
{
    unsigned int i;

    pthread_mutex_lock(&g_snapshot_mutex);
    if (!g_snapshot_started) {
        pthread_mutex_unlock(&g_snapshot_mutex);
        return;
    }

    g_snapshot_stop = 1;
    pthread_cond_signal(&g_snapshot_cond);
    pthread_mutex_unlock(&g_snapshot_mutex);

    pthread_join(g_snapshot_thread, NULL);

    pthread_mutex_lock(&g_snapshot_mutex);
    for (i = 0; i < SNAPSHOT_QUEUE_DEPTH; i++) {
        snapshot_free_job(&g_snapshot_queue[i]);
    }
    g_snapshot_started = 0;
    g_snapshot_stop = 0;
    g_snapshot_head = 0;
    g_snapshot_tail = 0;
    g_snapshot_count = 0;
    pthread_mutex_unlock(&g_snapshot_mutex);

    printf("[snapshot] writer stopped\n");
}
