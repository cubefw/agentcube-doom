// doomgeneric platform: SDL window + stream frames to AgentCube (sim or cube)
// Based on doomgeneric_sdl.c (ozkl/doomgeneric)
//
// ESP8266 heap cannot hold a full 240x240 RGB565 buffer (~115KB).
// Stream in horizontal strips (default H=20 → 9600 bytes/POST).

#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>
#include <curl/curl.h>

#define KEYQUEUE_SIZE 16
#define AC_W 240
#define AC_H 240
/* Max strip height: DRAW_FRAME_MAX_BYTES=12KB → 12000/(240*2)=25 lines */
#define AC_STRIP_H_DEFAULT 16
#define AC_STRIP_H_MAX 24

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

// AgentCube stream config
static char g_host[256] = "127.0.0.1:8765";
static int g_stream = 1;
static int g_show_window = 1;
static int g_frame_skip = 1; // send every Nth frame (1 = all)
static int g_frame_i = 0;
static int g_strip_h = AC_STRIP_H_DEFAULT;
static CURL *g_curl = NULL;
static char g_url[512];
/* Full scaled frame in RGB565 LE (host has RAM; cube only gets strips) */
static uint16_t g_rgb565[AC_W * AC_H];
static struct curl_slist *g_base_headers = NULL;

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

static void parse_args(void)
{
    const char *env = getenv("AGENTCUBE_HOST");
    if (env && env[0]) {
        snprintf(g_host, sizeof(g_host), "%s", env);
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
        g_strip_h = atoi(env);
        if (g_strip_h > AC_STRIP_H_MAX) {
            g_strip_h = AC_STRIP_H_MAX;
        }
        if (g_strip_h < 1) {
            g_strip_h = 1;
        }
    }
    // -agentcube host:port
    int p = M_CheckParmWithArgs("-agentcube", 1);
    if (p > 0 && p + 1 < myargc) {
        snprintf(g_host, sizeof(g_host), "%s", myargv[p + 1]);
        g_stream = 1;
    }
    if (M_CheckParm("-nostream") > 0) {
        g_stream = 0;
    }
    if (M_CheckParm("-nowindow") > 0) {
        g_show_window = 0;
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
        g_strip_h = atoi(myargv[p + 1]);
        if (g_strip_h > AC_STRIP_H_MAX) {
            g_strip_h = AC_STRIP_H_MAX;
        }
        if (g_strip_h < 1) {
            g_strip_h = 1;
        }
    }
}

static size_t curl_discard(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static void stream_init(void)
{
    if (!g_stream) {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (!g_curl) {
        fprintf(stderr, "agentcube: curl_easy_init failed\n");
        g_stream = 0;
        return;
    }
    // host may be "127.0.0.1:8765" or full URL
    if (strncmp(g_host, "http://", 7) == 0 || strncmp(g_host, "https://", 8) == 0) {
        snprintf(g_url, sizeof(g_url), "%s/api/v1/draw/frame", g_host);
    } else {
        snprintf(g_url, sizeof(g_url), "http://%s/api/v1/draw/frame", g_host);
    }
    g_base_headers = curl_slist_append(NULL, "Content-Type: application/octet-stream");
    g_base_headers = curl_slist_append(g_base_headers, "X-Frame-X: 0");
    g_base_headers = curl_slist_append(g_base_headers, "X-Frame-W: 240");

    curl_easy_setopt(g_curl, CURLOPT_URL, g_url);
    curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_discard);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_NODELAY, 1L);
    fprintf(stderr, "agentcube: streaming to %s (strips H=%d, ~%d bytes each)\n",
            g_url, g_strip_h, AC_W * g_strip_h * 2);
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
            // doomgeneric packing: typically 0x00RRGGBB in lower bits
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >> 8) & 0xFF);
            uint8_t b = (uint8_t)(px & 0xFF);
            uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            g_rgb565[y * AC_W + x] = c;
        }
    }
}

static int post_strip(int y0, int h)
{
    char hdr_y[48];
    char hdr_h[48];
    snprintf(hdr_y, sizeof(hdr_y), "X-Frame-Y: %d", y0);
    snprintf(hdr_h, sizeof(hdr_h), "X-Frame-H: %d", h);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "X-Frame-X: 0");
    headers = curl_slist_append(headers, "X-Frame-W: 240");
    headers = curl_slist_append(headers, hdr_y);
    headers = curl_slist_append(headers, hdr_h);

    size_t nbytes = (size_t)AC_W * (size_t)h * 2U;
    const char *body = (const char *)&g_rgb565[y0 * AC_W];

    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE, (long)nbytes);

    CURLcode rc = curl_easy_perform(g_curl);
    long http = 0;
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        if ((g_frame_i % 60) == 0) {
            fprintf(stderr, "agentcube: curl %s (y=%d h=%d)\n", curl_easy_strerror(rc), y0, h);
        }
        return -1;
    }
    if (http != 200 && (g_frame_i % 60) == 0) {
        fprintf(stderr, "agentcube: HTTP %ld (y=%d h=%d)\n", http, y0, h);
        return -1;
    }
    return 0;
}

static void stream_frame(void)
{
    if (!g_stream || !g_curl || !DG_ScreenBuffer) {
        return;
    }
    g_frame_i++;
    if ((g_frame_i % g_frame_skip) != 0) {
        return;
    }

    scale_frame_to_rgb565();

    for (int y = 0; y < AC_H; y += g_strip_h) {
        int h = g_strip_h;
        if (y + h > AC_H) {
            h = AC_H - y;
        }
        if (post_strip(y, h) != 0) {
            break;
        }
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
        // headless-ish: still need video subsystem for events on some platforms
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
