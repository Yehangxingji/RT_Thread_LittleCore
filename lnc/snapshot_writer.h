#ifndef SNAPSHOT_WRITER_H
#define SNAPSHOT_WRITER_H

#include <stddef.h>
#include <stdint.h>

#define SNAPSHOT_DEFAULT_DIR "/sdcard/snapshots"

int snapshot_writer_init(const char *dir);
int snapshot_writer_enqueue_h265(const uint8_t *data,
                                 size_t len,
                                 uint64_t pts,
                                 const char *reason);
int snapshot_writer_enqueue_h265_take(uint8_t *data,
                                      size_t len,
                                      uint64_t pts,
                                      const char *reason);
void snapshot_writer_deinit(void);

#endif /* SNAPSHOT_WRITER_H */
