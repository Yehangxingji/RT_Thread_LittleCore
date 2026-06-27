#include "rtsp.h"


static int send_all(int fd, const char *data)
{
    size_t total = strlen(data);
    size_t sent = 0;

    while (sent < total)
    {
        int n = send(fd, data + sent, total - sent, 0);
        if (n <= 0)
        {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

static int parse_rtsp_request(const char *req, rtsp_request_t *out)
{
    const char *cseq_pos;

    memset(out, 0, sizeof(*out));
    out->cseq = 0;

    if (sscanf(req, "%31s %255s", out->method, out->url) != 2)
    {
        return -1;
    }

    cseq_pos = strstr(req, "CSeq:");
    if (cseq_pos == NULL)
    {
        cseq_pos = strstr(req, "Cseq:");
    }
    if (cseq_pos != NULL)
    {
        out->cseq = atoi(cseq_pos + 5);
    }

    return 0;
}

static int parse_client_port(const char *req)
{
    const char *p = strstr(req, "client_port=");
    int port;

    if (p == NULL)
    {
        return 0;
    }

    port = atoi(p + strlen("client_port="));
    if (port <= 0 || port > 65535)
    {
        return 0;
    }

    return port;
}

static void make_sdp(char *sdp, size_t maxlen, const char *control_url)
{
    snprintf(sdp,
             maxlen,
             "v=0\r\n"
             "o=- 0 0 IN IP4 0.0.0.0\r\n"
             "s=H265 Stream\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H265/90000\r\n"
             "a=control:%s/trackID=0\r\n",
             RTP_PAYLOAD_TYPE,
             RTP_PAYLOAD_TYPE,
             control_url);
}

static void send_options_response(int fd, int cseq)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
             "\r\n",
             cseq);

    send_all(fd, response);
}

static void send_describe_response(int fd, const rtsp_request_t *req)
{
    char sdp[1024];
    char response[2048];

    make_sdp(sdp, sizeof(sdp), req->url);

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %lu\r\n"
             "\r\n"
             "%s",
             req->cseq,
             (unsigned long)strlen(sdp),
             sdp);

    send_all(fd, response);
}

static void send_setup_response(int fd, int cseq, int client_rtp_port)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
             "Session: %s\r\n"
             "\r\n",
             cseq,
             client_rtp_port,
             client_rtp_port + 1,
             RTP_SERVER_PORT,
             RTCP_SERVER_PORT,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_play_response(int fd, int cseq)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %s\r\n"
             "Range: npt=0.000-\r\n"
             "\r\n",
             cseq,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_teardown_response(int fd, int cseq)
{
    char response[256];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %s\r\n"
             "\r\n",
             cseq,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_error_response(int fd, int cseq, int code, const char *reason)
{
    char response[256];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 %d %s\r\n"
             "CSeq: %d\r\n"
             "\r\n",
             code,
             reason,
             cseq);

    send_all(fd, response);
}

void handle_rtsp_client(int client_fd, const struct sockaddr_in *cli_addr)
{
    char req_buf[2048];

    while (1)
    {
        int n;
        rtsp_request_t req;

        n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (n <= 0)
        {
            break;
        }

        req_buf[n] = '\0';
        printf("[rtsp] request from %s:%d\n%s\n",
               inet_ntoa(cli_addr->sin_addr),
               ntohs(cli_addr->sin_port),
               req_buf);

        if (parse_rtsp_request(req_buf, &req) != 0)
        {
            send_error_response(client_fd, 0, 400, "Bad Request");
            continue;
        }

        if (strcmp(req.method, "OPTIONS") == 0)
        {
            send_options_response(client_fd, req.cseq);
        }
        else if (strcmp(req.method, "DESCRIBE") == 0)
        {
            send_describe_response(client_fd, &req);
        }
        else if (strcmp(req.method, "SETUP") == 0)
        {
            int client_port = parse_client_port(req_buf);
            if (client_port == 0)
            {
                send_error_response(client_fd, req.cseq, 461, "Unsupported Transport");
                continue;
            }

            pthread_mutex_lock(&g_mutex);
            g_client_addr = *cli_addr;
            g_client_addr.sin_port = htons(client_port);
            g_client_addr_set = 1;
            g_play = 0;
            pthread_mutex_unlock(&g_mutex);

            printf("[rtsp] SETUP client RTP target %s:%d\n",
                   inet_ntoa(g_client_addr.sin_addr),
                   ntohs(g_client_addr.sin_port));

            send_setup_response(client_fd, req.cseq, client_port);
        }
        else if (strcmp(req.method, "PLAY") == 0)
        {
            pthread_mutex_lock(&g_mutex);
            g_play = 1;
            pthread_cond_signal(&g_cond);
            pthread_mutex_unlock(&g_mutex);

            send_play_response(client_fd, req.cseq);
        }
        else if (strcmp(req.method, "TEARDOWN") == 0)
        {
            pthread_mutex_lock(&g_mutex);
            g_play = 0;
            pthread_mutex_unlock(&g_mutex);

            send_teardown_response(client_fd, req.cseq);
            break;
        }
        else
        {
            send_error_response(client_fd, req.cseq, 405, "Method Not Allowed");
        }
    }

    pthread_mutex_lock(&g_mutex);
    g_play = 0;
    pthread_mutex_unlock(&g_mutex);
}

// 启动RTSP服务器
int start_rtsp_server(void)
{
    int listen_fd;
    int opt = 1;
    struct sockaddr_in listen_addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("RTSP socket failed, errno=%d\n", errno);
        return -1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(RTSP_PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        printf("RTSP bind port %d failed, errno=%d\n", RTSP_PORT, errno);
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 5) < 0) {
        printf("RTSP listen failed, errno=%d\n", errno);
        close(listen_fd);
        return -1;
    }

    printf("RTSP server started on port %d\n", RTSP_PORT);
    printf("Open VLC URL: rtsp://<board-ip>/stream\n");
    return listen_fd;
}


