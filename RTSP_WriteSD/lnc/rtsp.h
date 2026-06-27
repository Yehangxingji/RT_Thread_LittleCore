#ifndef RTSP_H
#define RTSP_H

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h> 
#include <sys/socket.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rtp.h"

#define RTSP_PORT        554
#define RTSP_SESSION_ID  "12345678"

typedef struct
{
    char method[32];
    char url[256];
    int cseq;
} rtsp_request_t;

extern pthread_mutex_t g_mutex;
extern pthread_cond_t g_cond;
extern int g_play;
extern int g_client_addr_set;
extern struct sockaddr_in g_client_addr;

// 处理RTSP客户端
void handle_rtsp_client(int client_fd, const struct sockaddr_in *cli_addr);

int start_rtsp_server(void);

#endif
