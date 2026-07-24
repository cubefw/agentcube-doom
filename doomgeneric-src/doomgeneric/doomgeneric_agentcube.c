// doomgeneric platform: SDL window + stream frames to AgentCube
// Based on doomgeneric_sdl.c (ozkl/doomgeneric)
//
// Same path as scripts/stream_video_to_cube.py:
//   scale Doom → 80×80 RGB565 → ACSP TCP :81
//   Cube keeps previous LCD image until the full frame is in RAM, then SPI 3×→240.
// HTTP strip fallback is legacy only (-http).

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <SDL.h>
#include <curl/curl.h>

#define KEYQUEUE_SIZE 16
#define AC_W 80
#define AC_H 80

/* HTTP fallback strip limits (ESP8266 heap) */
#define AC_HTTP_STRIP_H_DEFAULT 8
#define AC_HTTP_STRIP_H_MAX 12

/* ACSP protocol — must match firmware StreamProtocol.h */
#define ACSP_MAGIC 0x50534341u /* 'ACSP' LE */
#define ACSP_VERSION 1
#define ACSP_CMD_FRAME 1
#define ACSP_CMD_CLEAR 2
#define ACSP_CMD_PING 3
#define ACSP_CMD_END 4
#define ACSP_HEADER_SIZE 20
#define ACSP_DEFAULT_PORT 81

#pragma pack(push, 1)
typedef struct AcspHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t cmd;
    uint16_t seq;
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint32_t payload_len;
} AcspHeader;
#pragma pack(pop)

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static char g_host[256] = "127.0.0.1:8765";
static char g_host_only[256] = "127.0.0.1";
static int g_http_port = 8765;
static int g_stream_port = ACSP_DEFAULT_PORT;
static int g_stream = 1;
static int g_show_window = 1;
static int g_frame_skip = 1;
static int g_frame_i = 0;
static int g_http_strip_h = AC_HTTP_STRIP_H_DEFAULT;
static int g_use_tcp = 1;     /* prefer ACSP TCP atomic present */
static int g_force_http = 0;
static int g_wait_ack = 1;    /* wait cube ACK after each frame (one frame in flight) */
static int g_sock = -1;
static uint16_t g_seq = 0;
static int g_fail_streak = 0;

static CURL *g_curl = NULL;
static char g_url[512];

static uint16_t g_rgb565[AC_W * AC_H];

/* stats */
static uint32_t g_stat_frames = 0;
static uint32_t g_stat_bytes = 0;
static double g_stat_t0 = 0;

static double now_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static unsigned char convertToDoomKey(unsigned int key)
{
    switch (key) {
    case SDLK_RETURN: key = KEY_ENTER; break;
    case SDLK_ESCAPE: key = KEY_ESCAPE; break;
    case SDLK_LEFT: key = KEY_LEFTARROW; break;
    case SDLK_RIGHT: key = KEY_RIGHTARROW; break;
    case SDLK_UP: key = KEY_UPARROW; break;
    case SDLK_DOWN: key = KEY_DOWNARROW; break;
    case SDLK_LCTRL:
    case SDLK_RCTRL: key = KEY_FIRE; break;
    case SDLK_SPACE: key = KEY_USE; break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: key = KEY_RSHIFT; break;
    case SDLK_LALT:
    case SDLK_RALT: key = KEY_LALT; break;
    case SDLK_F2: key = KEY_F2; break;
    case SDLK_F3: key = KEY_F3; break;
    case SDLK_F4: key = KEY_F4; break;
    case SDLK_F5: key = KEY_F5; break;
    case SDLK_F6: key = KEY_F6; break;
    case SDLK_F7: key = KEY_F7; break;
    case SDLK_F8: key = KEY_F8; break;
    case SDLK_F9: key = KEY_F9; break;
    case SDLK_F10: key = KEY_F10; break;
    case SDLK_F11: key = KEY_F11; break;
    case SDLK_EQUALS:
    case SDLK_PLUS: key = KEY_EQUALS; break;
    case SDLK_MINUS: key = KEY_MINUS; break;
    default: key = tolower((int)key); break;
    }
    return (unsigned char)key;
}

static void addKeyToQueue(int pressed, unsigned int keyCode)
{
    unsigned char key = convertToDoomKey(keyCode);
    unsigned short keyData = (unsigned short)((pressed << 8) | key);
    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}

static void handleKeyInput(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            puts("Quit requested");
            atexit(SDL_Quit);
            exit(0);
        }
        if (e.type == SDL_KEYDOWN) {
            addKeyToQueue(1, (unsigned int)e.key.keysym.sym);
        } else if (e.type == SDL_KEYUP) {
            addKeyToQueue(0, (unsigned int)e.key.keysym.sym);
        }
    }
}

static void parse_host_port(const char *in)
{
    /* Accept host, host:port, http://host:port */
    const char *p = in;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    }
    snprintf(g_host, sizeof(g_host), "%s", p);

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", p);
    char *colon = strrchr(tmp, ':');
    if (colon && strchr(tmp, ']') == NULL) {
        /* simple host:port (not IPv6) */
        *colon = '\0';
        snprintf(g_host_only, sizeof(g_host_only), "%s", tmp);
        int port = atoi(colon + 1);
        if (port > 0) {
            g_http_port = port;
        }
    } else {
        snprintf(g_host_only, sizeof(g_host_only), "%s", tmp);
        /* real cube default HTTP 80; sim often 8765 already in default host */
        if (strcmp(g_host_only, "127.0.0.1") == 0 || strcmp(g_host_only, "localhost") == 0) {
            g_http_port = 8765;
        } else {
            g_http_port = 80;
        }
    }
}

static void parse_args(void)
{
    const char *env = getenv("AGENTCUBE_HOST");
    if (env && env[0]) {
        parse_host_port(env);
    } else {
        parse_host_port(g_host);
    }
    env = getenv("AGENTCUBE_STREAM");
    if (env && env[0] == '0') {
        g_stream = 0;
    }
    env = getenv("AGENTCUBE_FRAME_SKIP");
    if (env && atoi(env) > 0) {
        g_frame_skip = atoi(env);
    }
    env = getenv("AGENTCUBE_STRIP_H");
    if (env && atoi(env) > 0) {
        g_http_strip_h = atoi(env);
        if (g_http_strip_h > AC_HTTP_STRIP_H_MAX) {
            g_http_strip_h = AC_HTTP_STRIP_H_MAX;
        }
    }
    env = getenv("AGENTCUBE_STREAM_PORT");
    if (env && atoi(env) > 0) {
        g_stream_port = atoi(env);
    }
    env = getenv("AGENTCUBE_HTTP");
    if (env && env[0] == '1') {
        g_force_http = 1;
        g_use_tcp = 0;
    }
    env = getenv("AGENTCUBE_NOACK");
    if (env && env[0] == '1') {
        g_wait_ack = 0;
    }

    int p = M_CheckParmWithArgs("-agentcube", 1);
    if (p > 0 && p + 1 < myargc) {
        parse_host_port(myargv[p + 1]);
        g_stream = 1;
    }
    if (M_CheckParm("-nostream") > 0) {
        g_stream = 0;
    }
    if (M_CheckParm("-nowindow") > 0) {
        g_show_window = 0;
    }
    if (M_CheckParm("-http") > 0) {
        g_force_http = 1;
        g_use_tcp = 0;
    }
    if (M_CheckParm("-noack") > 0) {
        g_wait_ack = 0;
    }
    p = M_CheckParmWithArgs("-frameskip", 1);
    if (p > 0 && p + 1 < myargc) {
        g_frame_skip = atoi(myargv[p + 1]);
        if (g_frame_skip < 1) {
            g_frame_skip = 1;
        }
    }
    p = M_CheckParmWithArgs("-striph", 1);
    if (p > 0 && p + 1 < myargc) {
        g_http_strip_h = atoi(myargv[p + 1]);
        if (g_http_strip_h > AC_HTTP_STRIP_H_MAX) {
            g_http_strip_h = AC_HTTP_STRIP_H_MAX;
        }
        if (g_http_strip_h < 1) {
            g_http_strip_h = 1;
        }
    }
    p = M_CheckParmWithArgs("-streamport", 1);
    if (p > 0 && p + 1 < myargc) {
        g_stream_port = atoi(myargv[p + 1]);
    }
}

static size_t curl_discard(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static int tcp_connect(void)
{
    if (g_sock >= 0) {
        return 0;
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", g_stream_port);

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int ga = getaddrinfo(g_host_only, portstr, &hints, &res);
    if (ga != 0) {
        fprintf(stderr, "agentcube: getaddrinfo %s: %s\n", g_host_only, gai_strerror(ga));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; /* 100ms — ACK poll */
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* send timeout a bit longer for full frames */
        struct timeval tvs;
        tvs.tv_sec = 3;
        tvs.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvs, sizeof(tvs));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "agentcube: TCP connect %s:%d failed (%s)\n", g_host_only, g_stream_port,
                strerror(errno));
        return -1;
    }
    g_sock = fd;
    fprintf(stderr, "agentcube: ACSP TCP connected %s:%d\n", g_host_only, g_stream_port);
    return 0;
}

static void tcp_close(void)
{
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

static int tcp_write_all(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(g_sock, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

/* Wait for cube ACK (0x06) after FRAME/CLEAR — one frame in flight */
static int acsp_wait_ack(void)
{
    if (!g_wait_ack || g_sock < 0) {
        return 0;
    }
    uint8_t b = 0;
    for (int attempt = 0; attempt < 200; attempt++) {
        ssize_t n = recv(g_sock, &b, 1, 0);
        if (n == 1) {
            if (b == 0x06) {
                return 0;
            }
            /* ignore stray bytes */
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(2000);
                continue;
            }
            /* macOS: SO_RCVTIMEO → EWOULDBLOCK; may also get ETIMEDOUT */
#ifdef ETIMEDOUT
            if (errno == ETIMEDOUT) {
                usleep(2000);
                continue;
            }
#endif
            return -1;
        }
        usleep(2000);
    }
    fprintf(stderr, "agentcube: ACK timeout (cube busy?)\n");
    return -1;
}

/* One full 80×80 frame — same as stream_video_to_cube.py. Cube ACKs after present. */
static int acsp_send_frame(const uint16_t *pixels)
{
    if (g_sock < 0 && tcp_connect() != 0) {
        return -1;
    }

    AcspHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ACSP_MAGIC;
    hdr.version = ACSP_VERSION;
    hdr.cmd = ACSP_CMD_FRAME;
    hdr.seq = g_seq++;
    hdr.x = 0;
    hdr.y = 0;
    hdr.w = AC_W;
    hdr.h = AC_H;
    hdr.payload_len = (uint32_t)AC_W * (uint32_t)AC_H * 2u;

    size_t nbytes = (size_t)hdr.payload_len;
    if (tcp_write_all(&hdr, sizeof(hdr)) != 0 || tcp_write_all(pixels, nbytes) != 0) {
        tcp_close();
        return -1;
    }
    g_stat_bytes += (uint32_t)(sizeof(hdr) + nbytes);

    if (g_wait_ack && acsp_wait_ack() != 0) {
        tcp_close();
        return -1;
    }
    g_fail_streak = 0;
    return 0;
}

static void http_init(void)
{
    if (g_curl) {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (!g_curl) {
        fprintf(stderr, "agentcube: curl_easy_init failed\n");
        return;
    }
    snprintf(g_url, sizeof(g_url), "http://%s:%d/api/v1/draw/frame", g_host_only, g_http_port);
    curl_easy_setopt(g_curl, CURLOPT_URL, g_url);
    curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(g_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    fprintf(stderr, "agentcube: HTTP fallback %s (strips H=%d)\n", g_url, g_http_strip_h);
}

static int http_post_strip(int y0, int h)
{
    if (!g_curl) {
        http_init();
        if (!g_curl) {
            return -1;
        }
    }
    char hdr_y[48], hdr_h[48];
    snprintf(hdr_y, sizeof(hdr_y), "X-Frame-Y: %d", y0);
    snprintf(hdr_h, sizeof(hdr_h), "X-Frame-H: %d", h);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    char hdr_w[48];
    snprintf(hdr_w, sizeof(hdr_w), "X-Frame-W: %d", AC_W);
    headers = curl_slist_append(headers, "X-Frame-X: 0");
    headers = curl_slist_append(headers, hdr_w);
    headers = curl_slist_append(headers, hdr_y);
    headers = curl_slist_append(headers, hdr_h);

    size_t nbytes = (size_t)AC_W * (size_t)h * 2U;
    const char *body = (const char *)&g_rgb565[y0 * AC_W];
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE, (long)nbytes);
    CURLcode rc = curl_easy_perform(g_curl);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        return -1;
    }
    g_stat_bytes += (uint32_t)nbytes;
    return 0;
}

static void scale_frame_to_rgb565(void)
{
    const int sw = DOOMGENERIC_RESX;
    const int sh = DOOMGENERIC_RESY;
    const pixel_t *src = DG_ScreenBuffer;

    for (int y = 0; y < AC_H; y++) {
        int sy = (y * sh) / AC_H;
        for (int x = 0; x < AC_W; x++) {
            int sx = (x * sw) / AC_W;
            uint32_t px = (uint32_t)src[sy * sw + sx];
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t b = (uint8_t)(px & 0xFF);
            uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            g_rgb565[y * AC_W + x] = c;
        }
    }
}

static void stream_frame_tcp(void)
{
    /* Atomic present: identical pipeline to stream_video_to_cube.py */
    if (acsp_send_frame(g_rgb565) != 0) {
        g_fail_streak++;
        if ((g_fail_streak % 30) == 1) {
            fprintf(stderr, "agentcube: TCP frame failed (reconnect… streak=%d)\n", g_fail_streak);
        }
        tcp_close();
        return;
    }
}

static void stream_frame_http(void)
{
    /* Legacy: only useful for old FW without ACSP */
    for (int y = 0; y < AC_H; y += g_http_strip_h) {
        int h = g_http_strip_h;
        if (y + h > AC_H) {
            h = AC_H - y;
        }
        if (http_post_strip(y, h) != 0) {
            break;
        }
        usleep(2000);
    }
}

static void stream_frame(void)
{
    if (!g_stream || !DG_ScreenBuffer) {
        return;
    }
    g_frame_i++;
    if ((g_frame_i % g_frame_skip) != 0) {
        return;
    }

    scale_frame_to_rgb565();

    if (g_use_tcp && !g_force_http) {
        stream_frame_tcp();
    } else {
        stream_frame_http();
    }

    g_stat_frames++;
    if (g_stat_t0 == 0) {
        g_stat_t0 = now_sec();
    } else if ((g_stat_frames % 30) == 0) {
        double dt = now_sec() - g_stat_t0;
        if (dt > 0.5) {
            double fps = g_stat_frames / dt;
            double mbps = (g_stat_bytes * 8.0 / dt) / 1e6;
            fprintf(stderr, "agentcube: ~%.1f FPS stream, %.2f Mbit/s (%dx%d ACSP)\n", fps, mbps,
                    AC_W, AC_H);
            g_stat_frames = 0;
            g_stat_bytes = 0;
            g_stat_t0 = now_sec();
        }
    }
}

static void stream_init(void)
{
    if (!g_stream) {
        return;
    }
    if (g_force_http) {
        http_init();
        return;
    }
    if (tcp_connect() != 0) {
        fprintf(stderr, "agentcube: falling back to HTTP strips\n");
        g_use_tcp = 0;
        http_init();
    } else {
        /* No CLEAR — first frame paints over startup logo; later frames patch dirty rows only. */
        fprintf(stderr,
                "agentcube: ACSP atomic 80x80→240 (3x) (ACK after present).\n"
                "  Panel keeps previous frame until next is fully received.\n"
                "  Video test: python3 ../../scripts/stream_video_to_cube.py -H %s --test\n",
                g_host_only);
    }
}

void DG_Init(void)
{
    parse_args();
    stream_init();

    Uint32 flags = SDL_INIT_VIDEO;
#ifdef FEATURE_SOUND
    flags |= SDL_INIT_AUDIO;
#endif
    if (SDL_Init(flags) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }

    if (g_show_window) {
        window = SDL_CreateWindow(
            "DOOM → AgentCube",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            DOOMGENERIC_RESX,
            DOOMGENERIC_RESY,
            SDL_WINDOW_SHOWN);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGB888,
            SDL_TEXTUREACCESS_STREAMING,
            DOOMGENERIC_RESX,
            DOOMGENERIC_RESY);
    } else {
        window = SDL_CreateWindow(
            "DOOM",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            64,
            64,
            SDL_WINDOW_HIDDEN);
    }
}

void DG_DrawFrame(void)
{
    if (g_show_window && texture && renderer) {
        SDL_UpdateTexture(
            texture,
            NULL,
            DG_ScreenBuffer,
            DOOMGENERIC_RESX * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    stream_frame();
    handleKeyInput();
}

void DG_SleepMs(uint32_t ms)
{
    SDL_Delay(ms);
}

uint32_t DG_GetTicksMs(void)
{
    return SDL_GetTicks();
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
        return 0;
    }
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = keyData >> 8;
    *doomKey = (unsigned char)(keyData & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    if (window != NULL) {
        SDL_SetWindowTitle(window, title);
    }
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
