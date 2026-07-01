#ifndef DATAFIFO_SNAPSHOT_H
#define DATAFIFO_SNAPSHOT_H

#include "nalu_datafifo.h"

int datafifo_snapshot_process(const mpp_nalu_ipc_msg *msg, int writer_ready);
void datafifo_snapshot_deinit(void);

#endif /* DATAFIFO_SNAPSHOT_H */
