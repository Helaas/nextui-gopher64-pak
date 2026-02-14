/*
 * DRM plane scaling test using GBM buffers for tg5050
 *
 * Tests whether the Allwinner display engine corruption with drmModeSetPlane
 * is caused by dumb buffer allocation (vs GBM/CMA-allocated buffers).
 *
 * Theory: the display engine's hw scaler requires buffers allocated through
 * the GPU driver's allocator (GBM → libmali → CMA), not the generic dumb
 * buffer path. Dumb buffers may have wrong tiling/alignment for the scaler.
 *
 * Uses dlopen() for libgbm.so — no link-time dependency needed.
 *
 * Cross-compile:
 *   clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT \
 *     -fuse-ld=lld -o drm_gbm_plane_test drm_gbm_plane_test.c -ldrm -ldl
 *
 * Run on device:
 *   adb push drm_gbm_plane_test /tmp/
 *   adb shell '/tmp/drm_gbm_plane_test'
 *   adb shell '/tmp/drm_gbm_plane_test --dumb'   # side-by-side comparison
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
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* ------------------------------------------------------------------ */
/* Minimal GBM type/function declarations (loaded via dlopen)         */
/* ------------------------------------------------------------------ */

struct gbm_device;
struct gbm_bo;

/* GBM flags */
#define GBM_BO_USE_SCANOUT      (1 << 0)
#define GBM_BO_USE_RENDERING    (1 << 2)
#define GBM_BO_USE_WRITE        (1 << 3)
#define GBM_BO_USE_LINEAR       (1 << 4)

/* GBM map transfer flags */
#define GBM_BO_TRANSFER_WRITE       (1 << 1)
#define GBM_BO_TRANSFER_READ_WRITE  (3)

union gbm_bo_handle {
	void *ptr;
	int32_t s32;
	uint32_t u32;
	int64_t s64;
	uint64_t u64;
};

/* Function pointers loaded at runtime */
static struct gbm_device *(*p_gbm_create_device)(int fd);
static void (*p_gbm_device_destroy)(struct gbm_device *gbm);
static struct gbm_bo *(*p_gbm_bo_create)(struct gbm_device *gbm,
	uint32_t width, uint32_t height, uint32_t format, uint32_t flags);
static void (*p_gbm_bo_destroy)(struct gbm_bo *bo);
static uint32_t (*p_gbm_bo_get_width)(struct gbm_bo *bo);
static uint32_t (*p_gbm_bo_get_height)(struct gbm_bo *bo);
static uint32_t (*p_gbm_bo_get_stride)(struct gbm_bo *bo);
static uint32_t (*p_gbm_bo_get_format)(struct gbm_bo *bo);
static union gbm_bo_handle (*p_gbm_bo_get_handle)(struct gbm_bo *bo);
static void *(*p_gbm_bo_map)(struct gbm_bo *bo, uint32_t x, uint32_t y,
	uint32_t width, uint32_t height, uint32_t flags,
	uint32_t *stride, void **map_data);
static void (*p_gbm_bo_unmap)(struct gbm_bo *bo, void *map_data);

static void *g_gbm_lib = NULL;

static int load_gbm(void) {
	g_gbm_lib = dlopen("libgbm.so", RTLD_LAZY);
	if (!g_gbm_lib) g_gbm_lib = dlopen("libgbm.so.1", RTLD_LAZY);
	if (!g_gbm_lib) {
		fprintf(stderr, "  [FAIL] Cannot load libgbm.so: %s\n", dlerror());
		return -1;
	}

#define LOAD_SYM(name) \
	p_##name = dlsym(g_gbm_lib, #name); \
	if (!p_##name) { fprintf(stderr, "  [FAIL] Missing symbol: %s\n", #name); return -1; }

	LOAD_SYM(gbm_create_device)
	LOAD_SYM(gbm_device_destroy)
	LOAD_SYM(gbm_bo_create)
	LOAD_SYM(gbm_bo_destroy)
	LOAD_SYM(gbm_bo_get_width)
	LOAD_SYM(gbm_bo_get_height)
	LOAD_SYM(gbm_bo_get_stride)
	LOAD_SYM(gbm_bo_get_format)
	LOAD_SYM(gbm_bo_get_handle)
	LOAD_SYM(gbm_bo_map)
	LOAD_SYM(gbm_bo_unmap)
#undef LOAD_SYM

	fprintf(stderr, "  [PASS] Loaded libgbm.so\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;
static int g_also_test_dumb = 0;  /* --dumb: run dumb buffer test for comparison */
static int g_use_linear = 0;      /* --linear: add GBM_BO_USE_LINEAR flag */
static int g_use_write = 0;       /* --write: add GBM_BO_USE_WRITE flag */
static void sighandler(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* GBM-backed framebuffer                                             */
/* ------------------------------------------------------------------ */

struct gbm_fb {
	struct gbm_bo *bo;
	uint32_t w, h, stride;
	uint32_t handle;  /* GEM handle */
	uint32_t fb_id;   /* DRM framebuffer ID */
	uint8_t *map;
	void *map_data;   /* opaque handle for gbm_bo_unmap */
};

static int gbm_fb_create(int drm_fd, struct gbm_device *gbm,
                          struct gbm_fb *f, uint32_t w, uint32_t h) {
	memset(f, 0, sizeof(*f));
	f->w = w;
	f->h = h;

	uint32_t flags = GBM_BO_USE_SCANOUT;
	if (g_use_linear) flags |= GBM_BO_USE_LINEAR;
	if (g_use_write)  flags |= GBM_BO_USE_WRITE;

	f->bo = p_gbm_bo_create(gbm, w, h, DRM_FORMAT_XRGB8888, flags);
	if (!f->bo) {
		fprintf(stderr, "  [FAIL] gbm_bo_create %ux%u: %s\n", w, h, strerror(errno));
		return -1;
	}

	f->stride = p_gbm_bo_get_stride(f->bo);
	union gbm_bo_handle bh = p_gbm_bo_get_handle(f->bo);
	f->handle = bh.u32;

	fprintf(stderr, "  [INFO] GBM BO %ux%u: handle=%u stride=%u (%.1f bytes/pixel)\n",
		w, h, f->handle, f->stride, (float)f->stride / w);

	/* Create DRM framebuffer from GBM BO handle */
	uint32_t handles[4] = { f->handle, 0, 0, 0 };
	uint32_t pitches[4] = { f->stride, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	if (drmModeAddFB2(drm_fd, w, h, DRM_FORMAT_XRGB8888,
	                   handles, pitches, offsets, &f->fb_id, 0) < 0) {
		fprintf(stderr, "  [FAIL] drmModeAddFB2 for GBM BO %ux%u: %s\n", w, h, strerror(errno));
		p_gbm_bo_destroy(f->bo);
		f->bo = NULL;
		return -1;
	}

	/* Map for CPU access */
	uint32_t map_stride = 0;
	f->map = (uint8_t *)p_gbm_bo_map(f->bo, 0, 0, w, h,
		GBM_BO_TRANSFER_WRITE, &map_stride, &f->map_data);
	if (!f->map) {
		fprintf(stderr, "  [FAIL] gbm_bo_map %ux%u: %s\n", w, h, strerror(errno));
		drmModeRmFB(drm_fd, f->fb_id);
		p_gbm_bo_destroy(f->bo);
		memset(f, 0, sizeof(*f));
		return -1;
	}

	if (map_stride != f->stride) {
		fprintf(stderr, "  [INFO] gbm_bo_map stride=%u (differs from bo stride=%u)\n",
			map_stride, f->stride);
		f->stride = map_stride;  /* use the mapped stride for CPU writes */
	}

	fprintf(stderr, "  [PASS] GBM FB %ux%u: fb_id=%u handle=%u stride=%u\n",
		w, h, f->fb_id, f->handle, f->stride);
	return 0;
}

static void gbm_fb_destroy(int drm_fd, struct gbm_fb *f) {
	if (f->map && f->bo) {
		p_gbm_bo_unmap(f->bo, f->map_data);
		f->map = NULL;
	}
	if (f->fb_id) { drmModeRmFB(drm_fd, f->fb_id); f->fb_id = 0; }
	if (f->bo) { p_gbm_bo_destroy(f->bo); f->bo = NULL; }
}

/* ------------------------------------------------------------------ */
/* Dumb buffer framebuffer (for comparison)                           */
/* ------------------------------------------------------------------ */

struct dumb_fb {
	uint32_t w, h, stride, size, handle, fb_id;
	uint8_t *map;
};

static int dumb_fb_create(int fd, struct dumb_fb *f, uint32_t w, uint32_t h) {
	memset(f, 0, sizeof(*f));
	f->w = w; f->h = h;

	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) {
		fprintf(stderr, "  [FAIL] create_dumb %ux%u: %s\n", w, h, strerror(errno));
		return -1;
	}
	f->handle = c.handle; f->stride = c.pitch; f->size = c.size;

	if (drmModeAddFB(fd, w, h, 24, 32, f->stride, f->handle, &f->fb_id) < 0) {
		fprintf(stderr, "  [FAIL] addFB %ux%u: %s\n", w, h, strerror(errno));
		return -1;
	}

	struct drm_mode_map_dumb m = { .handle = f->handle };
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) return -1;
	f->map = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (f->map == MAP_FAILED) { f->map = NULL; return -1; }

	fprintf(stderr, "  [PASS] Dumb FB %ux%u: fb_id=%u stride=%u\n",
		w, h, f->fb_id, f->stride);
	return 0;
}

static void dumb_fb_destroy(int fd, struct dumb_fb *f) {
	if (f->map) { munmap(f->map, f->size); f->map = NULL; }
	if (f->fb_id) { drmModeRmFB(fd, f->fb_id); f->fb_id = 0; }
	if (f->handle) {
		struct drm_mode_destroy_dumb d = { .handle = f->handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		f->handle = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Pattern fill (same as drm_plane_scale_test.c)                      */
/* ------------------------------------------------------------------ */

static uint32_t pack_xrgb(uint8_t r, uint8_t g, uint8_t b) {
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fill_pattern(uint8_t *map, uint32_t w, uint32_t h, uint32_t stride) {
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(map + y * stride);
		for (uint32_t x = 0; x < w; x++) {
			uint8_t r = 0, g = 0, b = 0;
			int left = x < w / 2, top = y < h / 2;
			if (top && left)       { r = 255; }
			else if (top && !left) { g = 255; }
			else if (!top && left) { b = 255; }
			else                   { r = 255; g = 255; }

			uint8_t lum = ((x % (w/2)) * 255) / (w/2);
			r = (r * lum) >> 8; g = (g * lum) >> 8; b = (b * lum) >> 8;

			if (x % 32 == 0 || y % 32 == 0) { r = g = b = 80; }
			if (x == w/2 || y == h/2) { r = g = b = 255; }
			if (x < 2 || x >= w-2 || y < 2 || y >= h-2) { r = g = b = 255; }
			if ((x < 8 || x >= w-8) && (y < 8 || y >= h-8)) { r = g = b = 255; }

			row[x] = pack_xrgb(r, g, b);
		}
	}
}

static void fill_color(uint8_t *map, uint32_t w, uint32_t h, uint32_t stride,
                        uint8_t r, uint8_t g, uint8_t b) {
	uint32_t pixel = pack_xrgb(r, g, b);
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(map + y * stride);
		for (uint32_t x = 0; x < w; x++) row[x] = pixel;
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void msleep(int ms) {
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static uint32_t find_plane(int fd, uint32_t crtc_index, int want_overlay) {
	drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
	if (!planes) return 0;

	uint32_t result = 0;
	for (uint32_t i = 0; i < planes->count_planes && !result; i++) {
		drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
		if (!p) continue;
		if (!(p->possible_crtcs & (1 << crtc_index))) {
			drmModeFreePlane(p);
			continue;
		}

		drmModeObjectProperties *props = drmModeObjectGetProperties(
			fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		if (props) {
			for (uint32_t j = 0; j < props->count_props; j++) {
				drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
				if (!prop) continue;
				if (strcmp(prop->name, "type") == 0) {
					uint64_t val = props->prop_values[j];
					const char *names[] = {"Overlay", "Primary", "Cursor"};
					fprintf(stderr, "    Plane %u: type=%s\n",
						p->plane_id, val < 3 ? names[val] : "Unknown");
					if (want_overlay && val == 0) result = p->plane_id;
					if (!want_overlay && val == 1) result = p->plane_id;
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

/* ------------------------------------------------------------------ */
/* Main test                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
	int duration_ms = 3000;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dumb") == 0) g_also_test_dumb = 1;
		else if (strcmp(argv[i], "--linear") == 0) g_use_linear = 1;
		else if (strcmp(argv[i], "--write") == 0) g_use_write = 1;
		else if (strcmp(argv[i], "--fast") == 0) duration_ms = 1000;
		else if (strcmp(argv[i], "--help") == 0) {
			fprintf(stderr,
				"Usage: %s [--dumb] [--linear] [--write] [--fast]\n"
				"\n"
				"Tests DRM plane scaling with GBM-allocated buffers.\n"
				"  --dumb     Also test dumb buffers for side-by-side comparison\n"
				"  --linear   Add GBM_BO_USE_LINEAR flag (force linear/non-tiled)\n"
				"  --write    Add GBM_BO_USE_WRITE flag\n"
				"  --fast     Shorter display duration (1s instead of 3s)\n",
				argv[0]);
			return 0;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	fprintf(stderr, "=== DRM GBM Plane Scaling Test ===\n\n");
	fprintf(stderr, "Config: linear=%s, write=%s, also_dumb=%s\n\n",
		g_use_linear ? "on" : "off",
		g_use_write ? "on" : "off",
		g_also_test_dumb ? "on" : "off");

	/* Load GBM */
	if (load_gbm() < 0) return 1;

	/* Open DRM */
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { fprintf(stderr, "  [FAIL] open card0: %s\n", strerror(errno)); return 1; }

	/* Also try the render node for GBM if card0 doesn't work */
	struct gbm_device *gbm = p_gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "  [FAIL] gbm_create_device(card0): %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	fprintf(stderr, "  [PASS] GBM device created on card0\n");

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
		fprintf(stderr, "  [WARN] universal planes: %s\n", strerror(errno));
	if (drmSetMaster(fd) < 0)
		fprintf(stderr, "  [WARN] drmSetMaster: %s\n", strerror(errno));

	int ret = 1;
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { fprintf(stderr, "  [FAIL] getResources\n"); goto done; }

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && !conn) conn = c;
		else drmModeFreeConnector(c);
	}
	if (!conn) { fprintf(stderr, "  [FAIL] no connected connector\n"); goto done; }

	drmModeModeInfo *mode = NULL;
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = &conn->modes[i]; break; }
	if (!mode) mode = &conn->modes[0];
	fprintf(stderr, "  [PASS] Display: %ux%u @ %uHz\n", mode->hdisplay, mode->vdisplay, mode->vrefresh);

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
	for (int i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == crtc_id) { crtc_index = i; break; }
	if (!crtc_id) { fprintf(stderr, "  [FAIL] no CRTC\n"); goto done; }
	fprintf(stderr, "  [PASS] CRTC %u (index %u)\n", crtc_id, crtc_index);

	drmModeCrtc *saved_crtc = drmModeGetCrtc(fd, crtc_id);

	fprintf(stderr, "\n--- Available planes ---\n");
	uint32_t primary_plane = find_plane(fd, crtc_index, 0);
	uint32_t overlay_plane = find_plane(fd, crtc_index, 1);
	if (primary_plane) fprintf(stderr, "  Primary: %u\n", primary_plane);
	if (overlay_plane) fprintf(stderr, "  Overlay: %u\n", overlay_plane);

	uint32_t test_plane = primary_plane;
	if (!test_plane) test_plane = overlay_plane;
	if (!test_plane) { fprintf(stderr, "  [FAIL] no plane\n"); goto restore; }

	/* ---- Set up display with a GBM background buffer ---- */
	fprintf(stderr, "\n--- Setting up display (GBM background) ---\n");
	struct gbm_fb bg_gbm = {0};
	if (gbm_fb_create(fd, gbm, &bg_gbm, mode->hdisplay, mode->vdisplay) < 0) {
		fprintf(stderr, "  [FAIL] Cannot create GBM background buffer\n");
		goto restore;
	}
	fill_color(bg_gbm.map, bg_gbm.w, bg_gbm.h, bg_gbm.stride, 32, 32, 32);

	/* Unmap before scanout — some drivers require this */
	p_gbm_bo_unmap(bg_gbm.bo, bg_gbm.map_data);
	bg_gbm.map = NULL;
	bg_gbm.map_data = NULL;

	if (drmModeSetCrtc(fd, crtc_id, bg_gbm.fb_id, 0, 0,
	                   &conn->connector_id, 1, mode) < 0) {
		fprintf(stderr, "  [FAIL] SetCrtc background: %s\n", strerror(errno));
		goto restore;
	}
	fprintf(stderr, "  [PASS] GBM background displayed\n");
	msleep(500);

	/* ---- Test resolutions with GBM buffers ---- */
	struct { uint32_t w, h; const char *name; } test_sizes[] = {
		{ 320, 240, "320x240" },
		{ 640, 240, "640x240" },
		{ 640, 480, "640x480" },
	};
	int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

	fprintf(stderr, "\n=== Test: GBM buffer + drmModeSetPlane ===\n");
	for (int t = 0; t < num_sizes && g_running; t++) {
		uint32_t tw = test_sizes[t].w, th = test_sizes[t].h;
		fprintf(stderr, "\n--- GBM %s -> %ux%u (plane %u) ---\n",
			test_sizes[t].name, mode->hdisplay, mode->vdisplay, test_plane);

		struct gbm_fb test_fb = {0};
		if (gbm_fb_create(fd, gbm, &test_fb, tw, th) < 0) {
			fprintf(stderr, "  [FAIL] Cannot create GBM test buffer %s\n", test_sizes[t].name);
			continue;
		}

		fill_pattern(test_fb.map, tw, th, test_fb.stride);

		/* Unmap before presenting */
		p_gbm_bo_unmap(test_fb.bo, test_fb.map_data);
		test_fb.map = NULL;
		test_fb.map_data = NULL;

		int err = drmModeSetPlane(fd, test_plane, crtc_id, test_fb.fb_id, 0,
			0, 0, mode->hdisplay, mode->vdisplay,
			0, 0, tw << 16, th << 16);

		if (err < 0) {
			fprintf(stderr, "  [FAIL] SetPlane GBM %s: %s\n",
				test_sizes[t].name, strerror(errno));
		} else {
			fprintf(stderr, "  [SHOW] GBM %s -> fullscreen. CHECK FOR CORRUPTION!\n",
				test_sizes[t].name);
			fprintf(stderr, "  Expected: 4-color quadrants (R/G/B/Y) with gradients,\n");
			fprintf(stderr, "            grid lines every 32px, white border/cross.\n");
			msleep(duration_ms);
		}

		/* Restore background before next test */
		drmModeSetCrtc(fd, crtc_id, bg_gbm.fb_id, 0, 0,
		               &conn->connector_id, 1, mode);
		msleep(300);

		gbm_fb_destroy(fd, &test_fb);
	}

	/* ---- Optional: test dumb buffers for comparison ---- */
	if (g_also_test_dumb && g_running) {
		fprintf(stderr, "\n=== Test: Dumb buffer + drmModeSetPlane (comparison) ===\n");

		/* Need a dumb background for SetCrtc */
		struct dumb_fb bg_dumb = {0};
		if (dumb_fb_create(fd, &bg_dumb, mode->hdisplay, mode->vdisplay) < 0) {
			fprintf(stderr, "  [FAIL] Cannot create dumb background\n");
			goto skip_dumb;
		}
		fill_color(bg_dumb.map, bg_dumb.w, bg_dumb.h, bg_dumb.stride, 32, 32, 32);

		drmModeSetCrtc(fd, crtc_id, bg_dumb.fb_id, 0, 0,
		               &conn->connector_id, 1, mode);
		msleep(300);

		for (int t = 0; t < num_sizes && g_running; t++) {
			uint32_t tw = test_sizes[t].w, th = test_sizes[t].h;
			fprintf(stderr, "\n--- DUMB %s -> %ux%u (plane %u) ---\n",
				test_sizes[t].name, mode->hdisplay, mode->vdisplay, test_plane);

			struct dumb_fb test_dumb = {0};
			if (dumb_fb_create(fd, &test_dumb, tw, th) < 0) continue;

			fill_pattern(test_dumb.map, tw, th, test_dumb.stride);

			int err = drmModeSetPlane(fd, test_plane, crtc_id, test_dumb.fb_id, 0,
				0, 0, mode->hdisplay, mode->vdisplay,
				0, 0, tw << 16, th << 16);

			if (err < 0) {
				fprintf(stderr, "  [FAIL] SetPlane dumb %s: %s\n",
					test_sizes[t].name, strerror(errno));
			} else {
				fprintf(stderr, "  [SHOW] DUMB %s -> fullscreen. Compare with GBM above!\n",
					test_sizes[t].name);
				msleep(duration_ms);
			}

			drmModeSetCrtc(fd, crtc_id, bg_dumb.fb_id, 0, 0,
			               &conn->connector_id, 1, mode);
			msleep(300);

			dumb_fb_destroy(fd, &test_dumb);
		}

		dumb_fb_destroy(fd, &bg_dumb);
	}
skip_dumb:

	/* ---- Throughput test: GBM + SetPlane ---- */
	if (g_running) {
		fprintf(stderr, "\n=== Test: GBM throughput (640x240 -> %ux%u) ===\n",
			mode->hdisplay, mode->vdisplay);

		struct gbm_fb flip_a = {0}, flip_b = {0};
		if (gbm_fb_create(fd, gbm, &flip_a, 640, 240) == 0 &&
		    gbm_fb_create(fd, gbm, &flip_b, 640, 240) == 0) {

			struct timespec t0, t1;
			int flips = 120, ok = 0, fail = 0;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (int i = 0; i < flips && g_running; i++) {
				struct gbm_fb *cur = (i & 1) ? &flip_b : &flip_a;

				/* Remap for writing */
				uint32_t map_stride = 0;
				void *map_data = NULL;
				uint8_t *map = (uint8_t *)p_gbm_bo_map(cur->bo, 0, 0, cur->w, cur->h,
					GBM_BO_TRANSFER_WRITE, &map_stride, &map_data);
				if (map) {
					uint8_t v = (i * 4) % 256;
					fill_color(map, cur->w, cur->h, map_stride, v, 255 - v, 128);
					p_gbm_bo_unmap(cur->bo, map_data);
				}

				int err = drmModeSetPlane(fd, test_plane, crtc_id, cur->fb_id, 0,
					0, 0, mode->hdisplay, mode->vdisplay,
					0, 0, 640 << 16, 240 << 16);
				if (err < 0) fail++; else ok++;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);

			double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
			fprintf(stderr, "  [%s] %d flips in %.2fs = %.1f fps (GBM 640x240 fill + SetPlane)\n",
				fail == 0 ? "PASS" : "FAIL", flips, elapsed, flips / elapsed);
		}
		gbm_fb_destroy(fd, &flip_b);
		gbm_fb_destroy(fd, &flip_a);
	}

	ret = 0;

restore:
	fprintf(stderr, "\n--- Restoring display ---\n");
	if (saved_crtc && saved_crtc->buffer_id) {
		drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
			saved_crtc->x, saved_crtc->y, &conn->connector_id, 1, &saved_crtc->mode);
		fprintf(stderr, "  [PASS] Restored original CRTC\n");
	}
	if (saved_crtc) drmModeFreeCrtc(saved_crtc);

	gbm_fb_destroy(fd, &bg_gbm);

done:
	if (conn) drmModeFreeConnector(conn);
	if (res) drmModeFreeResources(res);
	if (gbm) p_gbm_device_destroy(gbm);
	if (fd >= 0) { drmDropMaster(fd); close(fd); }
	if (g_gbm_lib) dlclose(g_gbm_lib);

	fprintf(stderr, "\n=== DRM GBM Plane Test %s ===\n", ret == 0 ? "DONE" : "FAILED");
	return ret;
}
