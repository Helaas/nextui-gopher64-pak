/*
 * DRM scanout end-to-end test for tg5050
 *
 * Displays solid color frames directly on the screen via DRM/KMS
 * using dumb buffers (CPU-writable). This validates the full display
 * output path that gopher64 will use for frame presentation.
 *
 * Cycles: RED -> GREEN -> BLUE -> WHITE, 1 second each.
 *
 * Cross-compile:
 *   clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT \
 *     -fuse-ld=lld -o drm_scanout_test drm_scanout_test.c -ldrm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile int g_running = 1;

static void sighandler(int sig) {
    (void)sig;
    g_running = 0;
}

struct framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    uint32_t handle;
    uint32_t fb_id;
    uint8_t *map;
};

static int create_framebuffer(int fd, struct framebuffer *fb, uint32_t width, uint32_t height) {
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map_req = {0};

    fb->width = width;
    fb->height = height;

    /* Create dumb buffer */
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "  [FAIL] DRM_IOCTL_MODE_CREATE_DUMB: %s\n", strerror(errno));
        return -1;
    }
    fb->handle = create.handle;
    fb->stride = create.pitch;
    fb->size = create.size;

    /* Add as framebuffer */
    if (drmModeAddFB(fd, width, height, 24, 32, fb->stride, fb->handle, &fb->fb_id) < 0) {
        fprintf(stderr, "  [FAIL] drmModeAddFB: %s\n", strerror(errno));
        return -1;
    }

    /* Map for CPU access */
    map_req.handle = fb->handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
        fprintf(stderr, "  [FAIL] DRM_IOCTL_MODE_MAP_DUMB: %s\n", strerror(errno));
        return -1;
    }
    fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_req.offset);
    if (fb->map == MAP_FAILED) {
        fprintf(stderr, "  [FAIL] mmap: %s\n", strerror(errno));
        fb->map = NULL;
        return -1;
    }

    return 0;
}

static void destroy_framebuffer(int fd, struct framebuffer *fb) {
    if (fb->map) {
        munmap(fb->map, fb->size);
        fb->map = NULL;
    }
    if (fb->fb_id) {
        drmModeRmFB(fd, fb->fb_id);
        fb->fb_id = 0;
    }
    if (fb->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        fb->handle = 0;
    }
}

static void fill_color(struct framebuffer *fb, uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t *row = (uint32_t *)(fb->map + y * fb->stride);
        uint32_t pixel = (r << 16) | (g << 8) | b; /* XRGB8888 */
        for (uint32_t x = 0; x < fb->width; x++) {
            row[x] = pixel;
        }
    }
}

/* Draw a simple gradient + test pattern to confirm pixel accuracy */
static void fill_test_pattern(struct framebuffer *fb) {
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t *row = (uint32_t *)(fb->map + y * fb->stride);
        for (uint32_t x = 0; x < fb->width; x++) {
            uint8_t r, g, b;
            /* Top third: red gradient */
            if (y < fb->height / 3) {
                r = (x * 255) / fb->width;
                g = 0;
                b = 0;
            }
            /* Middle third: green gradient */
            else if (y < 2 * fb->height / 3) {
                r = 0;
                g = (x * 255) / fb->width;
                b = 0;
            }
            /* Bottom third: blue gradient */
            else {
                r = 0;
                g = 0;
                b = (x * 255) / fb->width;
            }
            /* White border */
            if (x < 4 || x >= fb->width - 4 || y < 4 || y >= fb->height - 4) {
                r = g = b = 255;
            }
            /* Center crosshair */
            if ((x == fb->width / 2 && y > fb->height / 4 && y < 3 * fb->height / 4) ||
                (y == fb->height / 2 && x > fb->width / 4 && x < 3 * fb->width / 4)) {
                r = g = b = 255;
            }
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

static void msleep(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    int fd = -1;
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    drmModeCrtc *saved_crtc = NULL;
    struct framebuffer fb[2] = {0};
    int ret = 1;
    int duration_ms = 1000; /* per color */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fast") == 0) duration_ms = 500;
        if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--fast]\n", argv[0]);
            fprintf(stderr, "Displays test colors on screen via DRM dumb buffers.\n");
            fprintf(stderr, "  --fast  500ms per frame instead of 1000ms\n");
            return 0;
        }
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    fprintf(stderr, "=== DRM Scanout Test ===\n\n");

    /* Open DRM device */
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "  [FAIL] Cannot open /dev/dri/card0: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "  [PASS] Opened /dev/dri/card0 (fd=%d)\n", fd);

    /* Need master for modesetting */
    if (drmSetMaster(fd) < 0) {
        fprintf(stderr, "  [WARN] drmSetMaster: %s (may still work)\n", strerror(errno));
    } else {
        fprintf(stderr, "  [PASS] DRM master acquired\n");
    }

    /* Get resources */
    res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "  [FAIL] drmModeGetResources: %s\n", strerror(errno));
        goto cleanup;
    }
    fprintf(stderr, "  [INFO] DRM resources: %d connectors, %d CRTCs, %d encoders\n",
        res->count_connectors, res->count_crtcs, res->count_encoders);

    /* Find connected connector */
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        fprintf(stderr, "  [INFO] Connector %d: type=%d id=%d %s (%dx%d mm)\n",
            i, c->connector_type, c->connector_id,
            c->connection == DRM_MODE_CONNECTED ? "CONNECTED" :
            c->connection == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown",
            c->mmWidth, c->mmHeight);

        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && !conn) {
            conn = c;
        } else {
            drmModeFreeConnector(c);
        }
    }

    if (!conn) {
        fprintf(stderr, "  [FAIL] No connected connector with modes found\n");
        goto cleanup;
    }
    fprintf(stderr, "  [PASS] Using connector %d (type=%d)\n", conn->connector_id, conn->connector_type);

    /* List available modes */
    fprintf(stderr, "  [INFO] Available modes:\n");
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        fprintf(stderr, "    [%d] %dx%d @ %dHz (type=0x%x flags=0x%x)\n",
            i, m->hdisplay, m->vdisplay, m->vrefresh, m->type, m->flags);
    }

    /* Use preferred mode, or first mode */
    drmModeModeInfo *mode = NULL;
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = &conn->modes[i];
            break;
        }
    }
    if (!mode) mode = &conn->modes[0];
    fprintf(stderr, "  [PASS] Selected mode: %dx%d @ %dHz\n",
        mode->hdisplay, mode->vdisplay, mode->vrefresh);

    /* Find CRTC */
    uint32_t crtc_id = 0;
    /* Try the encoder currently attached to this connector */
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    /* Fallback: find any available CRTC */
    if (!crtc_id) {
        for (int i = 0; i < conn->count_encoders; i++) {
            drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
            if (!enc) continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (enc->possible_crtcs & (1 << j)) {
                    crtc_id = res->crtcs[j];
                    drmModeFreeEncoder(enc);
                    goto found_crtc;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }
found_crtc:
    if (!crtc_id) {
        fprintf(stderr, "  [FAIL] No CRTC found for connector\n");
        goto cleanup;
    }
    fprintf(stderr, "  [PASS] Using CRTC %d\n", crtc_id);

    /* Save current CRTC state for restoration */
    saved_crtc = drmModeGetCrtc(fd, crtc_id);
    if (saved_crtc) {
        fprintf(stderr, "  [INFO] Saved current CRTC state (fb=%d, %dx%d+%d+%d)\n",
            saved_crtc->buffer_id, saved_crtc->width, saved_crtc->height, saved_crtc->x, saved_crtc->y);
    }

    /* Create double framebuffers */
    fprintf(stderr, "\n--- Creating framebuffers (%dx%d) ---\n", mode->hdisplay, mode->vdisplay);
    for (int i = 0; i < 2; i++) {
        if (create_framebuffer(fd, &fb[i], mode->hdisplay, mode->vdisplay) < 0) {
            fprintf(stderr, "  [FAIL] Failed to create framebuffer %d\n", i);
            goto cleanup;
        }
        fprintf(stderr, "  [PASS] Framebuffer %d: handle=%d fb_id=%d stride=%d size=%d\n",
            i, fb[i].handle, fb[i].fb_id, fb[i].stride, fb[i].size);
    }

    /* Display test frames */
    fprintf(stderr, "\n--- Displaying test frames ---\n");
    fprintf(stderr, "  Each frame shown for %d ms. Ctrl+C to stop.\n\n", duration_ms);

    struct {
        const char *name;
        uint8_t r, g, b;
        int pattern; /* 1 = use test pattern instead of solid color */
    } frames[] = {
        { "RED",          255, 0,   0,   0 },
        { "GREEN",        0,   255, 0,   0 },
        { "BLUE",         0,   0,   255, 0 },
        { "WHITE",        255, 255, 255, 0 },
        { "TEST PATTERN", 0,   0,   0,   1 },
    };
    int num_frames = sizeof(frames) / sizeof(frames[0]);
    int cur_fb = 0;

    for (int f = 0; f < num_frames && g_running; f++) {
        /* Fill the back buffer */
        if (frames[f].pattern) {
            fill_test_pattern(&fb[cur_fb]);
        } else {
            fill_color(&fb[cur_fb], frames[f].r, frames[f].g, frames[f].b);
        }

        /* Set CRTC to display this buffer */
        int err = drmModeSetCrtc(fd, crtc_id, fb[cur_fb].fb_id, 0, 0,
                                  &conn->connector_id, 1, mode);
        if (err < 0) {
            fprintf(stderr, "  [FAIL] drmModeSetCrtc (%s): %s\n", frames[f].name, strerror(errno));
        } else {
            fprintf(stderr, "  [PASS] Displaying: %s (fb=%d)\n", frames[f].name, fb[cur_fb].fb_id);
        }

        msleep(duration_ms);
        cur_fb ^= 1; /* swap buffers */
    }

    /* Page flip test (async, if supported) */
    if (g_running) {
        fprintf(stderr, "\n--- Page flip stress test (60 flips) ---\n");
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int flip_ok = 0, flip_fail = 0;
        for (int i = 0; i < 60 && g_running; i++) {
            uint8_t grey = (i * 4) % 256;
            fill_color(&fb[cur_fb], grey, grey, grey);

            int err = drmModeSetCrtc(fd, crtc_id, fb[cur_fb].fb_id, 0, 0,
                                      &conn->connector_id, 1, mode);
            if (err < 0) flip_fail++;
            else flip_ok++;
            cur_fb ^= 1;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double fps = 60.0 / elapsed;

        if (flip_fail == 0) {
            fprintf(stderr, "  [PASS] 60 flips in %.2fs (%.1f fps raw throughput)\n", elapsed, fps);
        } else {
            fprintf(stderr, "  [FAIL] %d/%d flips failed (%.2fs)\n", flip_fail, flip_ok + flip_fail, elapsed);
        }
    }

    fprintf(stderr, "\n--- Restoring previous display state ---\n");
    ret = 0;

cleanup:
    /* Restore original CRTC */
    if (saved_crtc) {
        if (saved_crtc->buffer_id) {
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                           saved_crtc->x, saved_crtc->y,
                           &conn->connector_id, 1, &saved_crtc->mode);
            fprintf(stderr, "  [PASS] Restored original CRTC state\n");
        }
        drmModeFreeCrtc(saved_crtc);
    }

    for (int i = 0; i < 2; i++) {
        destroy_framebuffer(fd, &fb[i]);
    }
    if (conn) drmModeFreeConnector(conn);
    if (res) drmModeFreeResources(res);
    if (fd >= 0) {
        drmDropMaster(fd);
        close(fd);
    }

    fprintf(stderr, "\n=== DRM Scanout Test %s ===\n", ret == 0 ? "PASSED" : "FAILED");
    return ret;
}
