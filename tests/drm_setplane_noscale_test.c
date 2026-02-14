/*
 * DRM SetPlane test WITHOUT hardware scaling
 *
 * Key diagnostic: is the corruption caused by the hw scaler, or by
 * SetPlane itself (cache coherency, bus master visibility)?
 *
 * Test A: 1280x720 pattern -> SetPlane 1280x720 (NO scaling)
 * Test B: 640x240 CPU-scaled to 1280x720 -> SetPlane 1280x720 (NO scaling)
 * Test C: 640x240 pattern -> SetPlane hw-scaled to 1280x720 (SCALING, expect corrupt)
 * Test D: Throughput of CPU-scale + SetPlane 1:1 (potential production path)
 *
 * If A+B are clean but C is corrupt: scaler is broken, use CPU-scale + SetPlane.
 * If A is also corrupt: SetPlane itself has a cache coherency issue.
 *
 * Cross-compile:
 *   clang --target=aarch64-unknown-linux-gnu --sysroot=$SYSROOT \
 *     -fuse-ld=lld -o drm_setplane_noscale_test drm_setplane_noscale_test.c -ldrm
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

#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile int g_running = 1;
static void sighandler(int sig) { (void)sig; g_running = 0; }

/* ------------------------------------------------------------------ */
/* Dumb buffer helpers                                                */
/* ------------------------------------------------------------------ */

struct fb {
	uint32_t w, h, stride, size, handle, fb_id;
	uint8_t *map;
};

static int fb_create(int fd, struct fb *f, uint32_t w, uint32_t h) {
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

	return 0;
}

static void fb_destroy(int fd, struct fb *f) {
	if (f->map) { munmap(f->map, f->size); f->map = NULL; }
	if (f->fb_id) { drmModeRmFB(fd, f->fb_id); f->fb_id = 0; }
	if (f->handle) {
		struct drm_mode_destroy_dumb d = { .handle = f->handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		f->handle = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Pattern fills                                                      */
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

/* CPU nearest-neighbor upscale from src into dst buffer */
static void cpu_upscale(const uint8_t *src_map, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
                         uint8_t *dst_map, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride) {
	for (uint32_t y = 0; y < dst_h; y++) {
		uint32_t sy = (y * src_h) / dst_h;
		const uint32_t *src_row = (const uint32_t *)(src_map + sy * src_stride);
		uint32_t *dst_row = (uint32_t *)(dst_map + y * dst_stride);
		for (uint32_t x = 0; x < dst_w; x++) {
			uint32_t sx = (x * src_w) / dst_w;
			dst_row[x] = src_row[sx];
		}
	}
}

/* Fast integer CPU upscale (memcpy row duplication) */
static void cpu_upscale_integer(const uint8_t *src_map, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
                                 uint8_t *dst_map, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride,
                                 uint32_t scale_x, uint32_t scale_y) {
	uint32_t *expanded = (uint32_t *)malloc(dst_w * sizeof(uint32_t));
	if (!expanded) return;

	size_t row_bytes = dst_w * sizeof(uint32_t);

	for (uint32_t sy = 0; sy < src_h; sy++) {
		const uint32_t *src_row = (const uint32_t *)(src_map + sy * src_stride);
		uint32_t *out = expanded;
		for (uint32_t sx = 0; sx < src_w; sx++) {
			uint32_t pixel = src_row[sx];
			for (uint32_t rx = 0; rx < scale_x; rx++)
				*out++ = pixel;
		}
		uint32_t dy_base = sy * scale_y;
		for (uint32_t ry = 0; ry < scale_y; ry++) {
			uint8_t *dst = dst_map + (dy_base + ry) * dst_stride;
			memcpy(dst, expanded, row_bytes);
		}
	}

	free(expanded);
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
		if (!(p->possible_crtcs & (1 << crtc_index))) { drmModeFreePlane(p); continue; }

		drmModeObjectProperties *props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		if (props) {
			for (uint32_t j = 0; j < props->count_props; j++) {
				drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
				if (!prop) continue;
				if (strcmp(prop->name, "type") == 0) {
					uint64_t val = props->prop_values[j];
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
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
	int duration_ms = 3000;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fast") == 0) duration_ms = 1000;
		else if (strcmp(argv[i], "--help") == 0) {
			fprintf(stderr,
				"Usage: %s [--fast]\n"
				"\n"
				"Tests SetPlane with and without hw scaling to isolate corruption source.\n"
				"  --fast   1s display per test instead of 3s\n",
				argv[0]);
			return 0;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	fprintf(stderr, "=== DRM SetPlane No-Scale Diagnostic ===\n\n");

	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { fprintf(stderr, "[FAIL] open card0: %s\n", strerror(errno)); return 1; }

	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetMaster(fd);

	int ret = 1;
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { fprintf(stderr, "[FAIL] getResources\n"); goto done; }

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && !conn) conn = c;
		else drmModeFreeConnector(c);
	}
	if (!conn) { fprintf(stderr, "[FAIL] no connector\n"); goto done; }

	drmModeModeInfo *mode = NULL;
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = &conn->modes[i]; break; }
	if (!mode) mode = &conn->modes[0];

	uint32_t disp_w = mode->hdisplay, disp_h = mode->vdisplay;
	fprintf(stderr, "Display: %ux%u @ %uHz\n", disp_w, disp_h, mode->vrefresh);

	uint32_t crtc_id = 0, crtc_index = 0;
	if (conn->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
		if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
	}
	for (int i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == crtc_id) { crtc_index = i; break; }
	if (!crtc_id) { fprintf(stderr, "[FAIL] no CRTC\n"); goto done; }

	drmModeCrtc *saved_crtc = drmModeGetCrtc(fd, crtc_id);
	uint32_t plane_id = find_plane(fd, crtc_index, 0);
	if (!plane_id) { fprintf(stderr, "[FAIL] no primary plane\n"); goto restore; }
	fprintf(stderr, "CRTC: %u, Primary plane: %u\n\n", crtc_id, plane_id);

	/* Background for initial SetCrtc */
	struct fb bg = {0};
	if (fb_create(fd, &bg, disp_w, disp_h) < 0) goto restore;
	fill_color(bg.map, bg.w, bg.h, bg.stride, 32, 32, 32);
	drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
	msleep(300);

	/* ============================================================
	 * TEST A: SetPlane 1280x720 -> 1280x720 (NO scaling, pattern)
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "=== TEST A: SetPlane 1:1 (1280x720 pattern, NO scaling) ===\n");
		struct fb full = {0};
		if (fb_create(fd, &full, disp_w, disp_h) < 0) goto restore;
		fill_pattern(full.map, full.w, full.h, full.stride);

		int err = drmModeSetPlane(fd, plane_id, crtc_id, full.fb_id, 0,
			0, 0, disp_w, disp_h,
			0, 0, disp_w << 16, disp_h << 16);

		if (err < 0)
			fprintf(stderr, "  [FAIL] SetPlane: %s\n", strerror(errno));
		else {
			fprintf(stderr, "  [SHOW] 1280x720 pattern via SetPlane (no scaling)\n");
			fprintf(stderr, "  -> If clean: scaler is the problem, not SetPlane\n");
			fprintf(stderr, "  -> If corrupt: SetPlane itself has cache issue\n");
			msleep(duration_ms);
		}

		/* Restore bg */
		drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(300);
		fb_destroy(fd, &full);
	}

	/* ============================================================
	 * TEST B: CPU-upscale 640x240 -> 1280x720, SetPlane 1:1
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST B: CPU-scale 640x240->1280x720 + SetPlane 1:1 ===\n");

		/* Create source pattern at 640x240 */
		struct fb src = {0};
		if (fb_create(fd, &src, 640, 240) < 0) goto restore;
		fill_pattern(src.map, src.w, src.h, src.stride);

		/* Create display-sized buffer and CPU-upscale into it */
		struct fb dst = {0};
		if (fb_create(fd, &dst, disp_w, disp_h) < 0) { fb_destroy(fd, &src); goto restore; }
		cpu_upscale_integer(src.map, 640, 240, src.stride,
		                     dst.map, disp_w, disp_h, dst.stride, 2, 3);

		int err = drmModeSetPlane(fd, plane_id, crtc_id, dst.fb_id, 0,
			0, 0, disp_w, disp_h,
			0, 0, disp_w << 16, disp_h << 16);

		if (err < 0)
			fprintf(stderr, "  [FAIL] SetPlane: %s\n", strerror(errno));
		else {
			fprintf(stderr, "  [SHOW] CPU-scaled 640x240 via SetPlane 1:1 (no hw scaling)\n");
			fprintf(stderr, "  -> If clean: this is a viable production path!\n");
			msleep(duration_ms);
		}

		drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(300);
		fb_destroy(fd, &dst);
		fb_destroy(fd, &src);
	}

	/* ============================================================
	 * TEST C: SetPlane 640x240 -> 1280x720 (HW scaling, expect corrupt)
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST C: SetPlane HW-scaled 640x240->1280x720 (control) ===\n");
		struct fb hw = {0};
		if (fb_create(fd, &hw, 640, 240) < 0) goto restore;
		fill_pattern(hw.map, hw.w, hw.h, hw.stride);

		int err = drmModeSetPlane(fd, plane_id, crtc_id, hw.fb_id, 0,
			0, 0, disp_w, disp_h,
			0, 0, 640 << 16, 240 << 16);

		if (err < 0)
			fprintf(stderr, "  [FAIL] SetPlane: %s\n", strerror(errno));
		else {
			fprintf(stderr, "  [SHOW] HW-scaled 640x240 (expect corruption here)\n");
			msleep(duration_ms);
		}

		drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(300);
		fb_destroy(fd, &hw);
	}

	/* ============================================================
	 * TEST D: Throughput — CPU-scale + SetPlane 1:1 (double buffered)
	 * This measures the viable production path if A+B are clean.
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST D: Throughput — CPU-scale 640x240 + SetPlane 1:1 ===\n");

		struct fb src_a = {0}, src_b = {0};
		struct fb dst_a = {0}, dst_b = {0};
		if (fb_create(fd, &src_a, 640, 240) < 0) goto restore;
		if (fb_create(fd, &src_b, 640, 240) < 0) { fb_destroy(fd, &src_a); goto restore; }
		if (fb_create(fd, &dst_a, disp_w, disp_h) < 0) { fb_destroy(fd, &src_b); fb_destroy(fd, &src_a); goto restore; }
		if (fb_create(fd, &dst_b, disp_w, disp_h) < 0) { fb_destroy(fd, &dst_a); fb_destroy(fd, &src_b); fb_destroy(fd, &src_a); goto restore; }

		int flips = 120, ok = 0, fail = 0;
		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < flips && g_running; i++) {
			struct fb *src = (i & 1) ? &src_b : &src_a;
			struct fb *dst = (i & 1) ? &dst_b : &dst_a;

			/* Fill source with varying color (simulates game frame) */
			uint8_t v = (i * 4) % 256;
			fill_color(src->map, src->w, src->h, src->stride, v, 255 - v, 128);

			/* CPU upscale to display resolution */
			cpu_upscale_integer(src->map, 640, 240, src->stride,
			                     dst->map, disp_w, disp_h, dst->stride, 2, 3);

			int err = drmModeSetPlane(fd, plane_id, crtc_id, dst->fb_id, 0,
				0, 0, disp_w, disp_h,
				0, 0, disp_w << 16, disp_h << 16);
			if (err < 0) fail++; else ok++;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);

		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		fprintf(stderr, "  [%s] %d frames in %.2fs = %.1f fps (CPU-scale 640x240->1280x720 + SetPlane)\n",
			fail == 0 ? "PASS" : "FAIL", flips, elapsed, flips / elapsed);

		/* Compare: SetCrtc throughput with same CPU-scale */
		if (g_running) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			ok = 0; fail = 0;
			for (int i = 0; i < flips && g_running; i++) {
				struct fb *src = (i & 1) ? &src_b : &src_a;
				struct fb *dst = (i & 1) ? &dst_b : &dst_a;

				uint8_t v = (i * 4) % 256;
				fill_color(src->map, src->w, src->h, src->stride, v, 255 - v, 128);

				cpu_upscale_integer(src->map, 640, 240, src->stride,
				                     dst->map, disp_w, disp_h, dst->stride, 2, 3);

				int err = drmModeSetCrtc(fd, crtc_id, dst->fb_id, 0, 0,
				                          &conn->connector_id, 1, mode);
				if (err < 0) fail++; else ok++;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);

			double elapsed2 = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
			fprintf(stderr, "  [%s] %d frames in %.2fs = %.1f fps (CPU-scale 640x240->1280x720 + SetCrtc)\n",
				fail == 0 ? "PASS" : "FAIL", flips, elapsed2, flips / elapsed2);

			if (elapsed > 0 && elapsed2 > 0)
				fprintf(stderr, "  SetPlane is %.1fx %s than SetCrtc\n",
					elapsed < elapsed2 ? elapsed2 / elapsed : elapsed / elapsed2,
					elapsed < elapsed2 ? "faster" : "slower");
		}

		fb_destroy(fd, &dst_b);
		fb_destroy(fd, &dst_a);
		fb_destroy(fd, &src_b);
		fb_destroy(fd, &src_a);
	}

	/* ============================================================
	 * TEST E: PageFlip 1:1 (pattern — visual check)
	 * drmModePageFlip queues a buffer swap for next vblank without
	 * blocking.  If the pattern is clean, PageFlip is our path.
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST E: PageFlip 1:1 (1280x720 pattern, NO scaling) ===\n");
		struct fb pf = {0};
		if (fb_create(fd, &pf, disp_w, disp_h) < 0) goto restore;
		fill_pattern(pf.map, pf.w, pf.h, pf.stride);

		/* PageFlip needs an active CRTC showing a buffer first */
		drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(100);

		int err = drmModePageFlip(fd, crtc_id, pf.fb_id, 0, NULL);
		if (err < 0)
			fprintf(stderr, "  [FAIL] PageFlip: %s\n", strerror(errno));
		else {
			fprintf(stderr, "  [SHOW] 1280x720 pattern via PageFlip (no scaling)\n");
			fprintf(stderr, "  -> If clean: PageFlip works for 1:1 buffers\n");
			msleep(duration_ms);
		}

		drmModeSetCrtc(fd, crtc_id, bg.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(300);
		fb_destroy(fd, &pf);
	}

	/* ============================================================
	 * TEST F: Throughput — CPU-scale + PageFlip (the fast path)
	 * PageFlip is non-blocking so we can overlap CPU work with
	 * display scanout. We use DRM_MODE_PAGE_FLIP_EVENT + poll()
	 * to pace at vsync without busy-waiting.
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST F: Throughput — CPU-scale 640x240 + PageFlip ===\n");

		struct fb pf_src_a = {0}, pf_src_b = {0};
		struct fb pf_dst_a = {0}, pf_dst_b = {0};
		if (fb_create(fd, &pf_src_a, 640, 240) < 0) goto restore;
		if (fb_create(fd, &pf_src_b, 640, 240) < 0) { fb_destroy(fd, &pf_src_a); goto restore; }
		if (fb_create(fd, &pf_dst_a, disp_w, disp_h) < 0) { fb_destroy(fd, &pf_src_b); fb_destroy(fd, &pf_src_a); goto restore; }
		if (fb_create(fd, &pf_dst_b, disp_w, disp_h) < 0) { fb_destroy(fd, &pf_dst_a); fb_destroy(fd, &pf_src_b); fb_destroy(fd, &pf_src_a); goto restore; }

		/* Ensure CRTC is active */
		drmModeSetCrtc(fd, crtc_id, pf_dst_a.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(100);

		int flips = 120, ok = 0, fail = 0;
		struct timespec t0, t1;

		/* --- PageFlip (non-blocking, no event wait) --- */
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < flips && g_running; i++) {
			struct fb *src = (i & 1) ? &pf_src_b : &pf_src_a;
			struct fb *dst = (i & 1) ? &pf_dst_b : &pf_dst_a;

			uint8_t v = (i * 4) % 256;
			fill_color(src->map, src->w, src->h, src->stride, v, 255 - v, 128);
			cpu_upscale_integer(src->map, 640, 240, src->stride,
			                     dst->map, disp_w, disp_h, dst->stride, 2, 3);

			int err = drmModePageFlip(fd, crtc_id, dst->fb_id, 0, NULL);
			if (err < 0) {
				/* EBUSY = previous flip not done yet, wait a bit */
				if (errno == EBUSY) {
					msleep(1);
					err = drmModePageFlip(fd, crtc_id, dst->fb_id, 0, NULL);
				}
				if (err < 0) fail++; else ok++;
			} else ok++;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);

		double elapsed_pf = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		fprintf(stderr, "  [%s] %d frames in %.2fs = %.1f fps (CPU-scale + PageFlip)\n",
			fail == 0 ? "PASS" : "WARN", flips, elapsed_pf, flips / elapsed_pf);
		if (fail > 0)
			fprintf(stderr, "  (%d EBUSY retries)\n", fail);

		/* --- PageFlip with vblank wait (paced) --- */
		if (g_running) {
			drmModeSetCrtc(fd, crtc_id, pf_dst_a.fb_id, 0, 0, &conn->connector_id, 1, mode);
			msleep(100);

			ok = 0; fail = 0;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (int i = 0; i < flips && g_running; i++) {
				struct fb *src = (i & 1) ? &pf_src_b : &pf_src_a;
				struct fb *dst = (i & 1) ? &pf_dst_b : &pf_dst_a;

				uint8_t v = (i * 4) % 256;
				fill_color(src->map, src->w, src->h, src->stride, v, 255 - v, 128);
				cpu_upscale_integer(src->map, 640, 240, src->stride,
				                     dst->map, disp_w, disp_h, dst->stride, 2, 3);

				/* Wait for vblank so we don't get EBUSY */
				drmVBlank vbl;
				memset(&vbl, 0, sizeof(vbl));
				vbl.request.type = DRM_VBLANK_RELATIVE;
				vbl.request.sequence = 1;
				drmWaitVBlank(fd, &vbl);

				int err = drmModePageFlip(fd, crtc_id, dst->fb_id, 0, NULL);
				if (err < 0) fail++; else ok++;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);

			double elapsed_pf2 = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
			fprintf(stderr, "  [%s] %d frames in %.2fs = %.1f fps (CPU-scale + vblank + PageFlip)\n",
				fail == 0 ? "PASS" : "WARN", flips, elapsed_pf2, flips / elapsed_pf2);
		}

		fb_destroy(fd, &pf_dst_b);
		fb_destroy(fd, &pf_dst_a);
		fb_destroy(fd, &pf_src_b);
		fb_destroy(fd, &pf_src_a);
	}

	/* ============================================================
	 * TEST G: Raw overhead comparison (no CPU fill, just flip)
	 * Measures pure DRM API call cost.
	 * ============================================================ */
	if (g_running) {
		fprintf(stderr, "\n=== TEST G: Pure flip overhead (pre-filled 1280x720) ===\n");

		struct fb pre_a = {0}, pre_b = {0};
		if (fb_create(fd, &pre_a, disp_w, disp_h) < 0) goto restore;
		if (fb_create(fd, &pre_b, disp_w, disp_h) < 0) { fb_destroy(fd, &pre_a); goto restore; }
		fill_pattern(pre_a.map, pre_a.w, pre_a.h, pre_a.stride);
		fill_pattern(pre_b.map, pre_b.w, pre_b.h, pre_b.stride);

		drmModeSetCrtc(fd, crtc_id, pre_a.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(100);

		int flips = 120;
		struct timespec t0, t1;

		/* SetCrtc only (no fill) */
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < flips && g_running; i++) {
			struct fb *cur = (i & 1) ? &pre_b : &pre_a;
			drmModeSetCrtc(fd, crtc_id, cur->fb_id, 0, 0, &conn->connector_id, 1, mode);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double el_crtc = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		fprintf(stderr, "  SetCrtc only:   %d flips in %.2fs = %.1f fps\n", flips, el_crtc, flips / el_crtc);

		/* SetPlane 1:1 only (no fill) */
		drmModeSetCrtc(fd, crtc_id, pre_a.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(100);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < flips && g_running; i++) {
			struct fb *cur = (i & 1) ? &pre_b : &pre_a;
			drmModeSetPlane(fd, plane_id, crtc_id, cur->fb_id, 0,
				0, 0, disp_w, disp_h,
				0, 0, disp_w << 16, disp_h << 16);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double el_plane = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		fprintf(stderr, "  SetPlane 1:1:   %d flips in %.2fs = %.1f fps\n", flips, el_plane, flips / el_plane);

		/* PageFlip only (no fill) */
		drmModeSetCrtc(fd, crtc_id, pre_a.fb_id, 0, 0, &conn->connector_id, 1, mode);
		msleep(100);
		int pf_ok = 0, pf_fail = 0;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < flips && g_running; i++) {
			struct fb *cur = (i & 1) ? &pre_b : &pre_a;
			int err = drmModePageFlip(fd, crtc_id, cur->fb_id, 0, NULL);
			if (err < 0 && errno == EBUSY) {
				/* Wait for previous flip */
				drmVBlank vbl;
				memset(&vbl, 0, sizeof(vbl));
				vbl.request.type = DRM_VBLANK_RELATIVE;
				vbl.request.sequence = 1;
				drmWaitVBlank(fd, &vbl);
				err = drmModePageFlip(fd, crtc_id, cur->fb_id, 0, NULL);
			}
			if (err < 0) pf_fail++; else pf_ok++;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double el_pf = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		fprintf(stderr, "  PageFlip:       %d flips in %.2fs = %.1f fps (%d failures)\n",
			flips, el_pf, flips / el_pf, pf_fail);

		fb_destroy(fd, &pre_b);
		fb_destroy(fd, &pre_a);
	}

	ret = 0;

restore:
	fprintf(stderr, "\n--- Restoring display ---\n");
	if (saved_crtc && saved_crtc->buffer_id)
		drmModeSetCrtc(fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
			saved_crtc->x, saved_crtc->y, &conn->connector_id, 1, &saved_crtc->mode);
	if (saved_crtc) drmModeFreeCrtc(saved_crtc);
	fb_destroy(fd, &bg);

done:
	if (conn) drmModeFreeConnector(conn);
	if (res) drmModeFreeResources(res);
	if (fd >= 0) { drmDropMaster(fd); close(fd); }

	fprintf(stderr, "\n=== SetPlane No-Scale Test %s ===\n", ret == 0 ? "DONE" : "FAILED");
	return ret;
}
