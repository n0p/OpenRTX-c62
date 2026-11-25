/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "core/state.h"
#include "sdl_engine.h"
#include "emulator.h"

#ifdef PLATFORM_LINUX_SIM
#include "sim_png.h"
#endif

chan_t fb_sync;              // Shared channel to receive frame buffer updates
Uint32 SDL_Screenshot_Event; // Shared custom SDL event to request a screenshot
Uint32 SDL_Backlight_Event;  // Shared custom SDL event to change backlight

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *displayTexture;

static bool ready = false;  // Signal if the main loop is ready
static keyboard_t sdl_keys; // Store the keyboard status

/*
 * One capture in flight.
 *
 * The framebuffer is a texture owned by the SDL thread, so a screenshot has to
 * be taken from that thread. sdlEngine_capture() pushes the request and waits
 * here for the answer; the main loop takes the lock only to publish the result,
 * so it is never blocked by a client that is not waiting.
 */
static pthread_mutex_t capLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  capCond;
static bool capPending = false;
static bool capDone    = false;
static bool capResult  = false;
static bool capPng     = false;
static char capPath[512];
static bool capCondReady = false;

static bool sdk_key_code_to_key(SDL_Keycode sym, keyboard_t *key)
{
    switch (sym) {
        case SDLK_0:
            *key = KEY_0;
            return true;

        case SDLK_1:
            *key = KEY_1;
            return true;

        case SDLK_2:
            *key = KEY_2;
            return true;

        case SDLK_3:
            *key = KEY_3;
            return true;

        case SDLK_4:
            *key = KEY_4;
            return true;

        case SDLK_5:
            *key = KEY_5;
            return true;

        case SDLK_6:
            *key = KEY_6;
            return true;

        case SDLK_7:
            *key = KEY_7;
            return true;

        case SDLK_8:
            *key = KEY_8;
            return true;

        case SDLK_9:
            *key = KEY_9;
            return true;

        case SDLK_ASTERISK:
            *key = KEY_STAR;
            return true;

        case SDLK_ESCAPE:
            *key = KEY_ESC;
            return true;

        case SDLK_LEFT:
            *key = KEY_LEFT;
            return true;

        case SDLK_RIGHT:
            *key = KEY_RIGHT;
            return true;

        case SDLK_RETURN:
            *key = KEY_ENTER;
            return true;

        case SDLK_HASH:
            *key = KEY_HASH;
            return true;

        case SDLK_n:
            *key = KEY_F1;
            return true;

        case SDLK_m:
            *key = KEY_MONI;
            return true;

        case SDLK_PAGEUP:
            *key = KNOB_LEFT;
            return true;

        case SDLK_PAGEDOWN:
            *key = KNOB_RIGHT;
            return true;

        case SDLK_UP:
            *key = KEY_UP;
            return true;

        case SDLK_DOWN:
            *key = KEY_DOWN;
            return true;

        default:
            return false;
    }
}

static int screenshot_display(const char *filename, bool png)
{
    /*
     * https://stackoverflow.com/a/48176678
     * user1902824
     * modified to keep renderer and display texture references in the body
     * rather than as a parameter
     */
    SDL_Renderer *ren = renderer;
    SDL_Texture *tex = displayTexture;
    int err = 0;

    SDL_Texture *ren_tex;
    SDL_Surface *surf;
    int st;
    int w;
    int h;
    int format;
    void *pixels;

    pixels = NULL;
    surf = NULL;
    ren_tex = NULL;
    format = SDL_PIXELFORMAT_RGBA32;

    /* Get information about texture we want to save */
    st = SDL_QueryTexture(tex, NULL, NULL, &w, &h);

    if (st != 0) {
        SDL_Log("Failed querying texture: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    ren_tex = SDL_CreateTexture(ren, format, SDL_TEXTUREACCESS_TARGET, w, h);

    if (!ren_tex) {
        SDL_Log("Failed creating render texture: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    /*
     * Initialize our canvas, then copy texture to a target whose pixel data we
     * can access
     */
    st = SDL_SetRenderTarget(ren, ren_tex);

    if (st != 0) {
        SDL_Log("Failed setting render target: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    SDL_SetRenderDrawColor(ren, 0x00, 0x00, 0x00, 0x00);
    SDL_RenderClear(ren);
    st = SDL_RenderCopy(ren, tex, NULL, NULL);

    if (st != 0) {
        SDL_Log("Failed copying texture data: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    /* Create buffer to hold texture data and load it */
    pixels = malloc(w * h * SDL_BYTESPERPIXEL(format));

    if (!pixels) {
        SDL_Log("Failed allocating memory\n");
        err++;
        goto cleanup;
    }

    st = SDL_RenderReadPixels(ren, NULL, format, pixels,
                              w * SDL_BYTESPERPIXEL(format));
    if (st != 0) {
        SDL_Log("Failed reading pixel data: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    /* Copy pixel data over to surface */
    surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h,
                                              SDL_BITSPERPIXEL(format),
                                              w * SDL_BYTESPERPIXEL(format),
                                              format);
    if (!surf) {
        SDL_Log("Failed creating new surface: %s\n", SDL_GetError());
        err++;
        goto cleanup;
    }

    /* Save result to an image */
    if (png) {
#ifdef PLATFORM_LINUX_SIM
        /* The read pixels are RGBA32 in memory order R,G,B,A: pack them as RGB
           for the PNG encoder. */
        uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);

        if (rgb == NULL) {
            err++;
            goto cleanup;
        }

        const uint8_t *src = (const uint8_t *)pixels;
        for (int i = 0; i < w * h; i++) {
            rgb[i * 3 + 0] = src[i * 4 + 0];
            rgb[i * 3 + 1] = src[i * 4 + 1];
            rgb[i * 3 + 2] = src[i * 4 + 2];
        }

        st = sim_png_write(filename, rgb, w, h) ? 0 : -1;
        free(rgb);
#else
        st = -1;
        (void)surf;
#endif
    } else {
        st = SDL_SaveBMP(surf, filename);
    }

    if (st != 0) {
        SDL_Log("Failed saving image: %s\n", filename);
        err++;
        goto cleanup;
    }

    SDL_Log("Saved texture as %s to \"%s\"\n", png ? "PNG" : "BMP", filename);

cleanup:
    SDL_FreeSurface(surf);
    free(pixels);
    SDL_DestroyTexture(ren_tex);
    return err;
}

static bool set_brightness(uint8_t brightness)
{
    /*
     * When this texture is rendered, during the copy operation each source
     * color channel is modulated by the appropriate color value according to
     * the following formula:
     * srcC = srcC * (color / 255)
     *
     * Color modulation is not always supported by the renderer;
     * it will return -1 if color modulation is not supported.
     */
    int colMod = SDL_SetTextureColorMod(displayTexture, brightness, brightness,
                                        brightness);

    SDL_RenderCopy(renderer, displayTexture, NULL, NULL);
    SDL_RenderPresent(renderer);

    return colMod == 0;
}

void sdlEngine_init()
{
    // Set audio stream name hints before any SDL subsystem init so they take
    // effect when the first audio device is opened. SDL_HINT_APP_NAME works on
    // macOS (Core Audio); the DEVICE hints are Linux-specific but harmless.
    SDL_SetHint(SDL_HINT_APP_NAME, "OpenRTX");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "OpenRTX");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "Communications");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) < 0) {
        printf("SDL video init error!!\n");
        exit(1);
    }

    // Register SDL custom events to handle screenshot requests and backlight
    SDL_Screenshot_Event = SDL_RegisterEvents(2);
    SDL_Backlight_Event = SDL_Screenshot_Event + 1;

    // The capture handshake needs its own condition variable
    if (!capCondReady) {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&capCond, &attr);
        pthread_condattr_destroy(&attr);
        capCondReady = true;
    }

    chan_init(&fb_sync);

    window = SDL_CreateWindow("OpenRTX", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, CONFIG_SCREEN_WIDTH * 3,
                              CONFIG_SCREEN_HEIGHT * 3, SDL_WINDOW_SHOWN);

    renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_RenderSetLogicalSize(renderer, CONFIG_SCREEN_WIDTH,
                             CONFIG_SCREEN_HEIGHT);
    displayTexture =
        SDL_CreateTexture(renderer, PIXEL_FORMAT, SDL_TEXTUREACCESS_STREAMING,
                          CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT);
    SDL_RenderClear(renderer);

    // Setting brightness also triggers a render
    set_brightness(state.settings.brightness);
}

/*
 * SDL main loop. Due to macOS restrictions, this must run on the Main Thread.
 */
void sdlEngine_run()
{
    ready = true;

    SDL_Event ev = { 0 };

    while (!emulator_state.powerOff) {
        keyboard_t key = 0;

        if (SDL_PollEvent(&ev) == 1) {
            switch (ev.type) {
                case SDL_QUIT:
                    emulator_state.powerOff = true;
                    break;

                case SDL_KEYDOWN:
                    if (sdk_key_code_to_key(ev.key.keysym.sym, &key)) {
                        sdl_keys |= key;
                    }
                    break;

                case SDL_KEYUP:
                    if (sdk_key_code_to_key(ev.key.keysym.sym, &key)) {
                        sdl_keys ^= key;
                    }
                    break;
            }

            if (ev.type == SDL_Screenshot_Event) {
                char path[sizeof(capPath)];
                bool png, ok;

                pthread_mutex_lock(&capLock);
                memcpy(path, capPath, sizeof(path));
                png = capPng;
                pthread_mutex_unlock(&capLock);

                ok = screenshot_display(path, png) == 0;

                pthread_mutex_lock(&capLock);
                capResult = ok;
                capDone   = true;
                pthread_cond_broadcast(&capCond);
                pthread_mutex_unlock(&capLock);
            } else if (ev.type == SDL_Backlight_Event) {
                set_brightness(*((uint8_t *)ev.user.data1));
                free(ev.user.data1);
            }
        }

        // we update the window only if there is a something ready to render
        if (chan_can_send(&fb_sync)) {
            PIXEL_SIZE *pixels;
            int pitch = 0;

            if (SDL_LockTexture(displayTexture, NULL, (void **)&pixels, &pitch)
                < 0) {
                SDL_Log("SDL_lock failed: %s", SDL_GetError());
            }

            chan_send(&fb_sync, pixels);
            chan_recv(&fb_sync, NULL);

            SDL_UnlockTexture(displayTexture);
            SDL_RenderCopy(renderer, displayTexture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }
    }

    printf("Terminating SDL display emulator, goodbye!\n");

    SDL_DestroyTexture(displayTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

bool sdlEngine_ready()
{
    /*
     * bool is an atomic data type for x86 and it can be returned safely without
     * incurring in data races between threads.
     */
    return ready;
}

bool sdlEngine_capture(const char *path, bool png, int timeoutMs)
{
    if (path == NULL || !ready)
        return false;

    pthread_mutex_lock(&capLock);

    if (capPending) {
        pthread_mutex_unlock(&capLock);
        return false;
    }

    snprintf(capPath, sizeof(capPath), "%s", path);
    capPng     = png;
    capDone    = false;
    capResult  = false;
    capPending = true;

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_Screenshot_Event;
    SDL_PushEvent(&e);

    bool ok = false;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += timeoutMs / 1000;
    deadline.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (!capDone && emulator_state.powerOff == false) {
        if (pthread_cond_timedwait(&capCond, &capLock, &deadline)
            == ETIMEDOUT)
            break;
    }

    ok = capDone && capResult;
    capPending = false;

    pthread_mutex_unlock(&capLock);

    return ok;
}

keyboard_t sdlEngine_getKeys()
{
    /*
     * keyboard_t is an atomic data type for x86 and it can be returned safely
     * without incurring in data races between threads.
     */
    return sdl_keys;
}
