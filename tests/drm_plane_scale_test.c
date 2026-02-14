/*
 * DRM plane scaling test for tg5050
 *
 * Tests whether the Allwinner display engine can hardware-scale
 * source buffers (320x240, 640x240, 640x480) to the full 1280x720 display
 * using DRM overlay planes with drmModeSetPlane().
 *
 * If this works, gopher64 can render at N64 native resolution and let
 * the display controller upscale — zero CPU overhead for scaling.
 *
 * Cross-compile:
 *   clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT \
 *     -fuse-ld=lld -o drm_plane_scale_test drm_plane_scale_test.c -ldrm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile int g_running = 1;
static int g_force_msync = 0;
static int g_use_addfb2 = 0;
static int g_force_dmabuf_sync = 0;
static int g_use_atomic_plane = 0;
static int g_format_sweep = 0;
static int g_test_padded_pitch = 0;
static void sighandler(int sig) { (void)sig; g_running = 0; }

enum pack_mode {
    PACK_XRGB8888 = 0, /* XR24 */
    PACK_XBGR8888,     /* XB24 */
    PACK_RGBX8888,     /* RX24 */
    PACK_BGRX8888      /* BX24 */
};

struct pixel_format_desc {
    const char *name;
    uint32_t fourcc;
    enum pack_mode pack;
};

static const struct pixel_format_desc k_formats[] = {
    { "xr24", DRM_FORMAT_XRGB8888, PACK_XRGB8888 },
    { "xb24", DRM_FORMAT_XBGR8888, PACK_XBGR8888 },
    { "rx24", DRM_FORMAT_RGBX8888, PACK_RGBX8888 },
    { "bx24", DRM_FORMAT_BGRX8888, PACK_BGRX8888 },
};

static int g_format_index = 0; /* default xr24 */

static uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b) {
    switch (k_formats[g_format_index].pack) {
    case PACK_XBGR8888:
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    case PACK_RGBX8888:
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8);
    case PACK_BGRX8888:
        return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8);
    case PACK_XRGB8888:
    default:
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

static int find_format_index(const char *name) {
    for (int i = 0; i < (int)(sizeof(k_formats) / sizeof(k_formats[0])); i++) {
        if (strcmp(name, k_formats[i].name) == 0) return i;
    }
    return -1;
}

struct fb {
    uint32_t w, h, stride, size, handle, id;
    uint8_t *map;
    int dma_fd;
};

static int fb_create(int fd, struct fb *f, uint32_t w, uint32_t h) {
    memset(f, 0, sizeof(*f));
    f->dma_fd = -1;
    f->w = w; f->h = h;

    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) {
        fprintf(stderr, "  [FAIL] create_dumb %ux%u: %s\n", w, h, strerror(errno));
        return -1;
    }
    f->handle = c.handle; f->stride = c.pitch; f->size = c.size;

    if (g_use_addfb2) {
        uint32_t handles[4] = { f->handle, 0, 0, 0 };
        uint32_t pitches[4] = { f->stride, 0, 0, 0 };
        uint32_t offsets[4] = { 0, 0, 0, 0 };
        uint32_t fmt = k_formats[g_format_index].fourcc;
        if (drmModeAddFB2(fd, w, h, fmt, handles, pitches, offsets, &f->id, 0) < 0) {
            fprintf(stderr, "  [FAIL] addFB2(%s) %ux%u: %s\n", k_formats[g_format_index].name, w, h, strerror(errno));
            return -1;
        }
    } else {
        if (drmModeAddFB(fd, w, h, 24, 32, f->stride, f->handle, &f->id) < 0) {
            fprintf(stderr, "  [FAIL] addFB %ux%u: %s\n", w, h, strerror(errno));
            return -1;
        }
    }

    struct drm_mode_map_dumb m = { .handle = f->handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) {
        fprintf(stderr, "  [FAIL] map_dumb %ux%u: %s\n", w, h, strerror(errno));
        return -1;
    }
    f->map = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if (f->map == MAP_FAILED) { f->map = NULL; return -1; }

    if (drmPrimeHandleToFD(fd, f->handle, DRM_CLOEXEC | DRM_RDWR, &f->dma_fd) < 0)
        f->dma_fd = -1;

    return 0;
}

static void fb_flush(struct fb *f) {
    if (!g_force_msync || !f->map || !f->size) return;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;

    uintptr_t start = (uintptr_t)f->map;
    uintptr_t end = start + f->size;
    uintptr_t aligned_start = start & ~((uintptr_t)page - 1);
    uintptr_t aligned_end = (end + (uintptr_t)page - 1) & ~((uintptr_t)page - 1);
    size_t aligned_size = (size_t)(aligned_end - aligned_start);

    if (msync((void *)aligned_start, aligned_size, MS_SYNC) < 0) {
        fprintf(stderr, "  [WARN] msync failed: %s\n", strerror(errno));
    }
}

static void fb_dmabuf_sync(struct fb *f, unsigned flags) {
    if (!g_force_dmabuf_sync || f->dma_fd < 0) return;
    struct dma_buf_sync sync = { .flags = flags };
    if (ioctl(f->dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "  [WARN] DMA_BUF_IOCTL_SYNC failed: %s\n", strerror(errno));
            warned = 1;
        }
    }
}

static void fb_destroy(int fd, struct fb *f) {
    if (f->map) { munmap(f->map, f->size); f->map = NULL; }
    if (f->dma_fd >= 0) { close(f->dma_fd); f->dma_fd = -1; }
    if (f->id) { drmModeRmFB(fd, f->id); f->id = 0; }
    if (f->handle) {
        struct drm_mode_destroy_dumb d = { .handle = f->handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        f->handle = 0;
    }
}

static void fb_fill_color(struct fb *f, uint8_t r, uint8_t g, uint8_t b) {
    fb_dmabuf_sync(f, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    uint32_t pixel = pack_pixel(r, g, b);
    for (uint32_t y = 0; y < f->h; y++) {
        uint32_t *row = (uint32_t *)(f->map + y * f->stride);
        for (uint32_t x = 0; x < f->w; x++) row[x] = pixel;
    }
    fb_dmabuf_sync(f, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

static void fb_fill_pattern(struct fb *f) {
    fb_dmabuf_sync(f, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    for (uint32_t y = 0; y < f->h; y++) {
        uint32_t *row = (uint32_t *)(f->map + y * f->stride);
        for (uint32_t x = 0; x < f->w; x++) {
            uint8_t r = 0, g = 0, b = 0;
            /* Colored quadrants */
            int left = x < f->w / 2, top = y < f->h / 2;
            if (top && left)       { r = 255; }              /* Red TL */
            else if (top && !left) { g = 255; }              /* Green TR */
            else if (!top && left) { b = 255; }              /* Blue BL */
            else                   { r = 255; g = 255; }     /* Yellow BR */

            /* Gradient within each quadrant */
            uint8_t lum = ((x % (f->w/2)) * 255) / (f->w/2);
            r = (r * lum) >> 8; g = (g * lum) >> 8; b = (b * lum) >> 8;

            /* Grid lines every 32 pixels */
            if (x % 32 == 0 || y % 32 == 0) { r = g = b = 80; }
            /* Center cross */
            if (x == f->w/2 || y == f->h/2) { r = g = b = 255; }
            /* Border */
            if (x < 2 || x >= f->w-2 || y < 2 || y >= f->h-2) { r = g = b = 255; }
            /* Corner markers (8x8 white squares) */
            if ((x < 8 || x >= f->w-8) && (y < 8 || y >= f->h-8)) { r = g = b = 255; }

            row[x] = pack_pixel(r, g, b);
        }
    }
    fb_dmabuf_sync(f, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

static void msleep(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;

    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) continue;
        if (strcmp(prop->name, name) == 0) {
            id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return id;
}

static int set_plane_atomic(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id,
                            uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
                            uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h) {
    uint32_t prop_fb_id   = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t prop_crtc_id = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t prop_crtc_x  = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t prop_crtc_y  = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t prop_crtc_w  = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t prop_crtc_h  = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t prop_src_x   = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t prop_src_y   = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t prop_src_w   = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t prop_src_h   = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");

    if (!prop_fb_id || !prop_crtc_id || !prop_crtc_x || !prop_crtc_y ||
        !prop_crtc_w || !prop_crtc_h || !prop_src_x || !prop_src_y ||
        !prop_src_w || !prop_src_h) {
        errno = EINVAL;
        return -1;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -1;

    if (drmModeAtomicAddProperty(req, plane_id, prop_fb_id, fb_id) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, dst_x) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, dst_y) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, dst_w) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, dst_h) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_src_x, src_x) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_src_y, src_y) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_src_w, src_w) < 0 ||
        drmModeAtomicAddProperty(req, plane_id, prop_src_h, src_h) < 0) {
        drmModeAtomicFree(req);
        errno = EINVAL;
        return -1;
    }

    int err = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return err;
}

/* Find a plane usable with a given CRTC */
static uint32_t find_plane(int fd, uint32_t crtc_id, uint32_t crtc_index, int want_overlay) {
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) return 0;

    uint32_t result = 0;
    for (uint32_t i = 0; i < planes->count_planes && !result; i++) {
        drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p) continue;

        /* Check if plane can be used with our CRTC */
        if (!(p->possible_crtcs & (1 << crtc_index))) {
            drmModeFreePlane(p);
            continue;
        }

        /* Check plane type via properties */
        drmModeObjectProperties *props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
                if (!prop) continue;
                if (strcmp(prop->name, "type") == 0) {
                    uint64_t val = props->prop_values[j];
                    /* DRM_PLANE_TYPE_OVERLAY=0, PRIMARY=1, CURSOR=2 */
                    const char *names[] = {"Overlay", "Primary", "Cursor"};
                    const char *tname = val < 3 ? names[val] : "Unknown";
                    fprintf(stderr, "    Plane %u: type=%s crtcs=0x%x formats=%u",
                        p->plane_id, tname, p->possible_crtcs, p->count_formats);
                    if (p->fb_id)
                        fprintf(stderr, " (active: fb=%u %ux%u+%d+%d)",
                            p->fb_id, p->crtc_x, p->crtc_y, /* actually src dims but ok */
                            p->crtc_x, p->crtc_y);
                    fprintf(stderr, "\n");

                    if (want_overlay && val == 0 /* overlay */) result = p->plane_id;
                    if (!want_overlay && val == 1 /* primary */) result = p->plane_id;
                }
                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    return result;
}

static int try_plane_scale(int fd, uint32_t plane_id, uint32_t crtc_id,
                           struct fb *src, uint32_t dst_w, uint32_t dst_h,
                           const char *label) {
    /*
     * drmModeSetPlane(fd, plane_id, crtc_id, fb_id, flags,
     *     crtc_x, crtc_y, crtc_w, crtc_h,     -- destination rect on screen
     *     src_x, src_y, src_w, src_h)           -- source rect in fb (16.16 fixed point)
     *
     * If src size != dst size, the display controller must scale.
     */
    int err;
    if (g_use_atomic_plane) {
        err = set_plane_atomic(fd, plane_id, crtc_id, src->id,
                               0, 0, dst_w, dst_h,
                               0, 0, src->w << 16, src->h << 16);
    } else {
        err = drmModeSetPlane(fd, plane_id, crtc_id, src->id, 0,
            0, 0, dst_w, dst_h,                              /* dest: full screen */
            0, 0, src->w << 16, src->h << 16);               /* src: full fb, 16.16 fp */
    }

    if (err < 0) {
        fprintf(stderr, "  [FAIL] %s (%ux%u -> %ux%u): %s\n",
            label, src->w, src->h, dst_w, dst_h, strerror(errno));
        return -1;
    }
    fprintf(stderr, "  [PASS] %s (%ux%u -> %ux%u): plane %u OK\n",
        label, src->w, src->h, dst_w, dst_h, plane_id);
    return 0;
}

static int try_plane_scale_rect(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id,
                                uint32_t src_w, uint32_t src_h,
                                uint32_t dst_w, uint32_t dst_h,
                                const char *label) {
    int err;
    if (g_use_atomic_plane) {
        err = set_plane_atomic(fd, plane_id, crtc_id, fb_id,
                               0, 0, dst_w, dst_h,
                               0, 0, src_w << 16, src_h << 16);
    } else {
        err = drmModeSetPlane(fd, plane_id, crtc_id, fb_id, 0,
                              0, 0, dst_w, dst_h,
                              0, 0, src_w << 16, src_h << 16);
    }
    if (err < 0) {
        fprintf(stderr, "  [FAIL] %s (%ux%u -> %ux%u): %s\n",
                label, src_w, src_h, dst_w, dst_h, strerror(errno));
        return -1;
    }
    fprintf(stderr, "  [PASS] %s (%ux%u -> %ux%u): plane %u OK\n",
            label, src_w, src_h, dst_w, dst_h, plane_id);
    return 0;
}

static void run_format_sweep(int fd, uint32_t plane_id, uint32_t crtc_id,
                             uint32_t dst_w, uint32_t dst_h, int duration_ms) {
    int saved_format = g_format_index;
    int saved_addfb2 = g_use_addfb2;
    g_use_addfb2 = 1;

    fprintf(stderr, "\n--- Format sweep (640x240 -> %ux%u) ---\n", dst_w, dst_h);
    for (int i = 0; i < (int)(sizeof(k_formats) / sizeof(k_formats[0])); i++) {
        struct fb tmp = {0};
        g_format_index = i;
        fprintf(stderr, "  Testing format %s...\n", k_formats[g_format_index].name);

        if (fb_create(fd, &tmp, 640, 240) < 0) {
            fprintf(stderr, "  [FAIL] create tmp fb for %s\n", k_formats[g_format_index].name);
            continue;
        }
        fb_fill_pattern(&tmp);
        fb_flush(&tmp);

        if (try_plane_scale(fd, plane_id, crtc_id, &tmp, dst_w, dst_h, k_formats[g_format_index].name) == 0) {
            fprintf(stderr, "  [INFO] showing %s for %d ms\n", k_formats[g_format_index].name, duration_ms);
            msleep(duration_ms);
        }
        fb_destroy(fd, &tmp);
    }

    if (g_use_atomic_plane)
        set_plane_atomic(fd, plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    else
        drmModeSetPlane(fd, plane_id, crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    g_format_index = saved_format;
    g_use_addfb2 = saved_addfb2;
}

int main(int argc, char **argv) {
    int fd = -1;
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    drmModeCrtc *saved_crtc = NULL;
    struct fb bg = {0};       /* background / primary plane */
    struct fb small = {0};    /* 320x240 test */
    struct fb wide = {0};     /* 640x240 test (matches gopher64 scanout shape) */
    struct fb medium = {0};   /* 640x480 test */
    int ret = 1;
    int duration_ms = 2000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fast") == 0) duration_ms = 1000;
        else if (strcmp(argv[i], "--msync") == 0) g_force_msync = 1;
        else if (strcmp(argv[i], "--addfb2") == 0) g_use_addfb2 = 1;
        else if (strcmp(argv[i], "--dmabuf-sync") == 0) g_force_dmabuf_sync = 1;
        else if (strcmp(argv[i], "--atomic-plane") == 0) g_use_atomic_plane = 1;
        else if (strcmp(argv[i], "--format-sweep") == 0) g_format_sweep = 1;
        else if (strcmp(argv[i], "--test-padded-pitch") == 0) g_test_padded_pitch = 1;
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            int idx = find_format_index(argv[++i]);
            if (idx < 0) {
                fprintf(stderr, "Unknown format '%s' (expected: xr24|xb24|rx24|bx24)\n", argv[i]);
                return 1;
            }
            g_format_index = idx;
        }
        if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--fast] [--msync] [--addfb2] [--dmabuf-sync] [--atomic-plane] [--format <xr24|xb24|rx24|bx24>] [--format-sweep] [--test-padded-pitch]\n", argv[0]);
            fprintf(stderr, "Tests DRM plane scaling (320x240/640x240/640x480 -> 1280x720).\n");
            fprintf(stderr, "  --msync  force msync() after CPU writes to dumb buffers\n");
            fprintf(stderr, "  --addfb2 use drmModeAddFB2 with selected format instead of legacy AddFB\n");
            fprintf(stderr, "  --dmabuf-sync use DMA_BUF_IOCTL_SYNC around CPU writes\n");
            fprintf(stderr, "  --atomic-plane use drmModeAtomicCommit for plane updates\n");
            fprintf(stderr, "  --format select AddFB2 format (xr24 default)\n");
            fprintf(stderr, "  --format-sweep cycle through xr24/xb24/rx24/bx24 on-plane\n");
            fprintf(stderr, "  --test-padded-pitch add focused test: FB 1280x240, SRC rect 640x240\n");
            return 0;
        }
    }

    if ((g_format_index != 0 || g_format_sweep) && !g_use_addfb2) {
        fprintf(stderr, "  [INFO] forcing --addfb2 for selected/sweep format mode\n");
        g_use_addfb2 = 1;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    fprintf(stderr, "=== DRM Plane Scaling Test ===\n\n");
    fprintf(stderr, "Config: addfb=%s, format=%s, msync=%s, dmabuf_sync=%s, atomic_plane=%s, format_sweep=%s, test_padded_pitch=%s\n\n",
        g_use_addfb2 ? "AddFB2" : "AddFB(legacy)",
        k_formats[g_format_index].name,
        g_force_msync ? "on" : "off",
        g_force_dmabuf_sync ? "on" : "off",
        g_use_atomic_plane ? "on" : "off",
        g_format_sweep ? "on" : "off",
        g_test_padded_pitch ? "on" : "off");

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "  [FAIL] open card0: %s\n", strerror(errno)); return 1; }
    fprintf(stderr, "  [PASS] Opened /dev/dri/card0\n");

    /* Must enable universal planes to see overlay/cursor planes */
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
        fprintf(stderr, "  [WARN] DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s\n", strerror(errno));
    else
        fprintf(stderr, "  [PASS] Universal planes enabled\n");
    if (g_use_atomic_plane) {
        if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0)
            fprintf(stderr, "  [WARN] DRM_CLIENT_CAP_ATOMIC: %s\n", strerror(errno));
        else
            fprintf(stderr, "  [PASS] Atomic modesetting enabled\n");
    }

    if (drmSetMaster(fd) < 0)
        fprintf(stderr, "  [WARN] drmSetMaster: %s\n", strerror(errno));

    res = drmModeGetResources(fd);
    if (!res) { fprintf(stderr, "  [FAIL] getResources: %s\n", strerror(errno)); goto cleanup; }

    /* Find connected connector */
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && !conn) conn = c;
        else drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "  [FAIL] No connected connector\n"); goto cleanup; }

    drmModeModeInfo *mode = NULL;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = &conn->modes[i]; break; }
    if (!mode) mode = &conn->modes[0];
    fprintf(stderr, "  [PASS] Display: %ux%u @ %uHz\n", mode->hdisplay, mode->vdisplay, mode->vrefresh);

    /* Find CRTC */
    uint32_t crtc_id = 0, crtc_index = 0;
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
    }
    if (!crtc_id) {
        for (int i = 0; i < conn->count_encoders && !crtc_id; i++) {
            drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
            if (!enc) continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (enc->possible_crtcs & (1 << j)) { crtc_id = res->crtcs[j]; crtc_index = j; break; }
            }
            drmModeFreeEncoder(enc);
        }
    }
    /* Get CRTC index */
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == crtc_id) { crtc_index = i; break; }
    if (!crtc_id) { fprintf(stderr, "  [FAIL] No CRTC\n"); goto cleanup; }
    fprintf(stderr, "  [PASS] CRTC %u (index %u)\n", crtc_id, crtc_index);

    saved_crtc = drmModeGetCrtc(fd, crtc_id);

    /* Enumerate planes */
    fprintf(stderr, "\n--- Available planes ---\n");
    uint32_t primary_plane = find_plane(fd, crtc_id, crtc_index, 0);
    uint32_t overlay_plane = find_plane(fd, crtc_id, crtc_index, 1);

    if (primary_plane)
        fprintf(stderr, "  [PASS] Primary plane: %u\n", primary_plane);
    else
        fprintf(stderr, "  [FAIL] No primary plane found\n");

    if (overlay_plane)
        fprintf(stderr, "  [PASS] Overlay plane: %u\n", overlay_plane);
    else
        fprintf(stderr, "  [INFO] No overlay plane found (will test primary only)\n");

    /* Create framebuffers */
    fprintf(stderr, "\n--- Creating framebuffers ---\n");
    if (fb_create(fd, &bg, mode->hdisplay, mode->vdisplay) < 0) goto cleanup;
    fprintf(stderr, "  [PASS] Background: %ux%u (fb=%u)\n", bg.w, bg.h, bg.id);

    if (fb_create(fd, &small, 320, 240) < 0) goto cleanup;
    fprintf(stderr, "  [PASS] Small: 320x240 (fb=%u)\n", small.id);

    if (fb_create(fd, &wide, 640, 240) < 0) goto cleanup;
    fprintf(stderr, "  [PASS] Wide: 640x240 (fb=%u)\n", wide.id);

    if (fb_create(fd, &medium, 640, 480) < 0) goto cleanup;
    fprintf(stderr, "  [PASS] Medium: 640x480 (fb=%u)\n", medium.id);

    /* Fill test patterns */
    fb_fill_color(&bg, 32, 32, 32);  /* dark grey background */
    fb_fill_pattern(&small);
    fb_fill_pattern(&wide);
    fb_fill_pattern(&medium);
    fb_flush(&bg);
    fb_flush(&small);
    fb_flush(&wide);
    fb_flush(&medium);

    /* Set up the primary plane with our background */
    fprintf(stderr, "\n--- Setting up display ---\n");
    int err = drmModeSetCrtc(fd, crtc_id, bg.id, 0, 0, &conn->connector_id, 1, mode);
    if (err < 0) {
        fprintf(stderr, "  [FAIL] drmModeSetCrtc (background): %s\n", strerror(errno));
        goto cleanup;
    }
    fprintf(stderr, "  [PASS] Background displayed\n");
    msleep(500);

    if (g_format_sweep) {
        uint32_t sweep_plane = primary_plane ? primary_plane : overlay_plane;
        if (!sweep_plane) {
            fprintf(stderr, "  [FAIL] No plane available for format sweep\n");
        } else {
            run_format_sweep(fd, sweep_plane, crtc_id, mode->hdisplay, mode->vdisplay, duration_ms);
        }
        ret = 0;
        goto cleanup;
    }

    if (g_test_padded_pitch) {
        uint32_t test_plane = primary_plane ? primary_plane : overlay_plane;
        if (!test_plane) {
            fprintf(stderr, "  [FAIL] No plane available for padded-pitch test\n");
            ret = 1;
            goto cleanup;
        }

        fprintf(stderr, "\n--- Padded-pitch test (FB=1280x240, SRC=640x240) ---\n");
        struct fb padded = {0};
        if (fb_create(fd, &padded, 1280, 240) < 0) {
            fprintf(stderr, "  [FAIL] Failed to create padded FB\n");
            ret = 1;
            goto cleanup;
        }

        // Fill full row with black, then draw pattern only in left 640x240 region.
        for (uint32_t y = 0; y < padded.h; y++) {
            uint32_t *row = (uint32_t *)(padded.map + y * padded.stride);
            for (uint32_t x = 0; x < padded.w; x++)
                row[x] = pack_pixel(0, 0, 0);
        }
        struct fb left_view = padded;
        left_view.w = 640;
        left_view.h = 240;
        fb_fill_pattern(&left_view);
        fb_flush(&padded);

        if (try_plane_scale_rect(fd, test_plane, crtc_id, padded.id,
                                 640, 240, mode->hdisplay, mode->vdisplay,
                                 "PaddedPitch 640x240") == 0) {
            msleep(duration_ms);
        }

        if (g_use_atomic_plane)
            set_plane_atomic(fd, test_plane, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        else
            drmModeSetPlane(fd, test_plane, crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

        fb_destroy(fd, &padded);
        ret = 0;
        goto cleanup;
    }

    /*
     * Test 1: Scale on primary plane
     * Replace the primary plane FB with the small one, scaling up.
     */
    fprintf(stderr, "\n--- Test 1: Primary plane scaling ---\n");
    if (primary_plane) {
        fprintf(stderr, "  Testing 320x240 -> %ux%u on primary plane %u...\n",
            mode->hdisplay, mode->vdisplay, primary_plane);
        if (try_plane_scale(fd, primary_plane, crtc_id, &small,
                            mode->hdisplay, mode->vdisplay, "Primary 320x240") == 0) {
            msleep(duration_ms);
        }

        fprintf(stderr, "  Testing 640x240 -> %ux%u on primary plane %u...\n",
            mode->hdisplay, mode->vdisplay, primary_plane);
        if (try_plane_scale(fd, primary_plane, crtc_id, &wide,
                            mode->hdisplay, mode->vdisplay, "Primary 640x240") == 0) {
            msleep(duration_ms);
        }

        fprintf(stderr, "  Testing 640x480 -> %ux%u on primary plane %u...\n",
            mode->hdisplay, mode->vdisplay, primary_plane);
        if (try_plane_scale(fd, primary_plane, crtc_id, &medium,
                            mode->hdisplay, mode->vdisplay, "Primary 640x480") == 0) {
            msleep(duration_ms);
        }

        /* Restore background */
        drmModeSetCrtc(fd, crtc_id, bg.id, 0, 0, &conn->connector_id, 1, mode);
    }

    /*
     * Test 2: Scale on overlay plane
     * Keep the background on primary, overlay the small buffer scaled up.
     */
    if (overlay_plane) {
        fprintf(stderr, "\n--- Test 2: Overlay plane scaling ---\n");

        fprintf(stderr, "  Testing 320x240 -> %ux%u on overlay plane %u...\n",
            mode->hdisplay, mode->vdisplay, overlay_plane);
        if (try_plane_scale(fd, overlay_plane, crtc_id, &small,
                            mode->hdisplay, mode->vdisplay, "Overlay 320x240") == 0) {
            msleep(duration_ms);
        }

        fprintf(stderr, "  Testing 640x240 -> %ux%u on overlay plane %u...\n",
            mode->hdisplay, mode->vdisplay, overlay_plane);
        if (try_plane_scale(fd, overlay_plane, crtc_id, &wide,
                            mode->hdisplay, mode->vdisplay, "Overlay 640x240") == 0) {
            msleep(duration_ms);
        }

        fprintf(stderr, "  Testing 640x480 -> %ux%u on overlay plane %u...\n",
            mode->hdisplay, mode->vdisplay, overlay_plane);
        if (try_plane_scale(fd, overlay_plane, crtc_id, &medium,
                            mode->hdisplay, mode->vdisplay, "Overlay 640x480") == 0) {
            msleep(duration_ms);
        }

        /* Centered with integer scale: 320x240 * 3 = 960x720 */
        fprintf(stderr, "  Testing 320x240 -> 960x720 centered on overlay...\n");
        int cx = (mode->hdisplay - 960) / 2;
        int cy = (mode->vdisplay - 720) / 2;
        if (g_use_atomic_plane) {
            err = set_plane_atomic(fd, overlay_plane, crtc_id, small.id,
                                   cx, cy, 960, 720,
                                   0, 0, 320 << 16, 240 << 16);
        } else {
            err = drmModeSetPlane(fd, overlay_plane, crtc_id, small.id, 0,
                cx, cy, 960, 720,
                0, 0, 320 << 16, 240 << 16);
        }
        if (err < 0)
            fprintf(stderr, "  [FAIL] Overlay centered 320x240->960x720: %s\n", strerror(errno));
        else {
            fprintf(stderr, "  [PASS] Overlay centered 320x240 -> 960x720 at (%d,%d)\n", cx, cy);
            msleep(duration_ms);
        }

        /* Disable overlay */
        if (g_use_atomic_plane)
            set_plane_atomic(fd, overlay_plane, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        else
            drmModeSetPlane(fd, overlay_plane, crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    /*
     * Test 3: Page flip throughput with scaled plane
     * Measure how fast we can flip a 320x240 buffer scaled to fullscreen.
     */
    uint32_t test_plane = overlay_plane ? overlay_plane : primary_plane;
    if (test_plane) {
        fprintf(stderr, "\n--- Test 3: Scaled page flip throughput (320x240 -> %ux%u) ---\n",
            mode->hdisplay, mode->vdisplay);

        struct fb flip_a = {0}, flip_b = {0};
        if (fb_create(fd, &flip_a, 320, 240) < 0) goto skip_flip;
        if (fb_create(fd, &flip_b, 320, 240) < 0) { fb_destroy(fd, &flip_a); goto skip_flip; }

        struct timespec t0, t1;
        int flips = 120;
        int ok = 0, fail = 0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < flips && g_running; i++) {
            struct fb *cur = (i & 1) ? &flip_b : &flip_a;
            uint8_t v = (i * 4) % 256;
            fb_fill_color(cur, v, 255 - v, 128);
            fb_flush(cur);

            if (g_use_atomic_plane) {
                err = set_plane_atomic(fd, test_plane, crtc_id, cur->id,
                                       0, 0, mode->hdisplay, mode->vdisplay,
                                       0, 0, 320 << 16, 240 << 16);
            } else {
                err = drmModeSetPlane(fd, test_plane, crtc_id, cur->id, 0,
                    0, 0, mode->hdisplay, mode->vdisplay,
                    0, 0, 320 << 16, 240 << 16);
            }
            if (err < 0) fail++; else ok++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double fps = flips / elapsed;
        fprintf(stderr, "  [%s] %d flips in %.2fs = %.1f fps (320x240 fill + scaled flip)\n",
            fail == 0 ? "PASS" : "FAIL", flips, elapsed, fps);

        /* Disable plane */
        if (test_plane == overlay_plane) {
            if (g_use_atomic_plane)
                set_plane_atomic(fd, test_plane, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            else
                drmModeSetPlane(fd, test_plane, crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        fb_destroy(fd, &flip_b);
        fb_destroy(fd, &flip_a);
    }
skip_flip:

    /*
     * Test 4: Compare throughput — 320x240 scaled vs 1280x720 unscaled
     */
    if (primary_plane) {
        fprintf(stderr, "\n--- Test 4: Throughput comparison ---\n");

        /* 1280x720 unscaled */
        struct fb full_a = {0}, full_b = {0};
        if (fb_create(fd, &full_a, mode->hdisplay, mode->vdisplay) == 0 &&
            fb_create(fd, &full_b, mode->hdisplay, mode->vdisplay) == 0) {

            struct timespec t0, t1;
            int flips = 60;

            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int i = 0; i < flips && g_running; i++) {
                struct fb *cur = (i & 1) ? &full_b : &full_a;
                uint8_t v = (i * 4) % 256;
                fb_fill_color(cur, v, v, v);
                fb_flush(cur);
                drmModeSetCrtc(fd, crtc_id, cur->id, 0, 0, &conn->connector_id, 1, mode);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed_full = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            double fps_full = flips / elapsed_full;

            /* 320x240 scaled (on primary via SetPlane) */
            struct fb sm_a = {0}, sm_b = {0};
            double elapsed_small = 0, fps_small = 0;
            if (fb_create(fd, &sm_a, 320, 240) == 0 && fb_create(fd, &sm_b, 320, 240) == 0) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (int i = 0; i < flips && g_running; i++) {
                    struct fb *cur = (i & 1) ? &sm_b : &sm_a;
                    uint8_t v = (i * 4) % 256;
                    fb_fill_color(cur, v, v, v);
                    fb_flush(cur);
                    if (g_use_atomic_plane) {
                        set_plane_atomic(fd, primary_plane, crtc_id, cur->id,
                                         0, 0, mode->hdisplay, mode->vdisplay,
                                         0, 0, 320 << 16, 240 << 16);
                    } else {
                        drmModeSetPlane(fd, primary_plane, crtc_id, cur->id, 0,
                            0, 0, mode->hdisplay, mode->vdisplay,
                            0, 0, 320 << 16, 240 << 16);
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                elapsed_small = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
                fps_small = flips / elapsed_small;
            }

            fprintf(stderr, "  1280x720 unscaled:     %d flips in %.2fs = %.1f fps (fill=%.1f MB/frame)\n",
                flips, elapsed_full, fps_full, (double)(mode->hdisplay * mode->vdisplay * 4) / 1e6);
            fprintf(stderr, "  320x240 hw-scaled:     %d flips in %.2fs = %.1f fps (fill=%.1f MB/frame)\n",
                flips, elapsed_small, fps_small, (320.0 * 240 * 4) / 1e6);
            if (fps_small > fps_full * 1.1)
                fprintf(stderr, "  [PASS] HW scaling is %.1fx faster than full-res fill\n", fps_small / fps_full);
            else if (fps_small > 0)
                fprintf(stderr, "  [INFO] HW scaling ~same speed as full-res (%.1fx)\n", fps_small / fps_full);

            fb_destroy(fd, &sm_b);
            fb_destroy(fd, &sm_a);
        }
        fb_destroy(fd, &full_b);
        fb_destroy(fd, &full_a);
    }

    ret = 0;
    fprintf(stderr, "\n--- Restoring display ---\n");

cleanup:
    if (saved_crtc) {
        if (saved_crtc->buffer_id)
            drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                saved_crtc->x, saved_crtc->y, &conn->connector_id, 1, &saved_crtc->mode);
        fprintf(stderr, "  [PASS] Restored original CRTC\n");
        drmModeFreeCrtc(saved_crtc);
    }
    fb_destroy(fd, &medium);
    fb_destroy(fd, &wide);
    fb_destroy(fd, &small);
    fb_destroy(fd, &bg);
    if (conn) drmModeFreeConnector(conn);
    if (res) drmModeFreeResources(res);
    if (fd >= 0) { drmDropMaster(fd); close(fd); }

    fprintf(stderr, "\n=== DRM Plane Scaling Test %s ===\n", ret == 0 ? "DONE" : "FAILED");
    return ret;
}
