/*
 * DRM display backend for tg5050
 *
 * Opens /dev/dri/card0, finds the first connected connector,
 * allocates double-buffered dumb buffers at display resolution,
 * CPU-upscales from source resolution, and uses drmModePageFlip
 * for vsync-paced buffer swaps.
 *
 * The Allwinner DE3.3 hw scaler (drmModeSetPlane with src != dst)
 * corrupts non-uniform patterns. SetPlane/PageFlip at 1:1 are clean.
 */

#include "drm_display.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static bool create_dumb_buffer(int fd, DrmDisplay::DumbBuffer &buf, uint32_t width, uint32_t height)
{
	struct drm_mode_create_dumb create = {};
	create.width = width;
	create.height = height;
	create.bpp = 32;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
	{
		fprintf(stderr, "[drm_display] create_dumb: %s\n", strerror(errno));
		return false;
	}

	buf.handle = create.handle;
	buf.stride = create.pitch;
	buf.size = create.size;
	buf.width = width;
	buf.height = height;

	// Prefer legacy AddFB first. Our standalone DRM tests use this path and
	// scan out correctly on tg5050.
	if (drmModeAddFB(fd, width, height, 24, 32, buf.stride, buf.handle, &buf.fb_id) == 0)
	{
		buf.legacy_addfb = true;
	}
	else
	{
		// Fallback to AddFB2 XRGB8888 on drivers without legacy AddFB support.
		uint32_t handles[4] = {buf.handle, 0, 0, 0};
		uint32_t strides[4] = {buf.stride, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888, handles, strides, offsets, &buf.fb_id, 0) < 0)
		{
			fprintf(stderr, "[drm_display] addFB + addFB2(XRGB8888) both failed: %s\n", strerror(errno));
			return false;
		}
		buf.legacy_addfb = false;
	}

	struct drm_mode_map_dumb map_req = {};
	map_req.handle = buf.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0)
	{
		fprintf(stderr, "[drm_display] map_dumb: %s\n", strerror(errno));
		return false;
	}

	buf.map = (uint8_t *)mmap(NULL, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_req.offset);
	if (buf.map == MAP_FAILED)
	{
		fprintf(stderr, "[drm_display] mmap: %s\n", strerror(errno));
		buf.map = nullptr;
		return false;
	}

	return true;
}

static void destroy_dumb_buffer(int fd, DrmDisplay::DumbBuffer &buf)
{
	if (buf.map)
	{
		munmap(buf.map, buf.size);
		buf.map = nullptr;
	}
	if (buf.fb_id)
	{
		drmModeRmFB(fd, buf.fb_id);
		buf.fb_id = 0;
	}
	if (buf.handle)
	{
		struct drm_mode_destroy_dumb destroy = {};
		destroy.handle = buf.handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		buf.handle = 0;
	}
}

// Find plane of a specific type for a given CRTC.
static uint32_t find_plane_by_type(int fd, uint32_t crtc_id, uint64_t plane_type)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
		return 0;

	// Find CRTC index
	int crtc_index = -1;
	for (int i = 0; i < res->count_crtcs; i++)
	{
		if (res->crtcs[i] == crtc_id)
		{
			crtc_index = i;
			break;
		}
	}
	drmModeFreeResources(res);

	if (crtc_index < 0)
		return 0;

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res)
		return 0;

	uint32_t result = 0;

	for (uint32_t i = 0; i < plane_res->count_planes && result == 0; i++)
	{
		drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
		if (!plane)
			continue;

		if (!(plane->possible_crtcs & (1u << crtc_index)))
		{
			drmModeFreePlane(plane);
			continue;
		}

		// Check plane type via properties.
		drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		if (props)
		{
			for (uint32_t j = 0; j < props->count_props; j++)
			{
				drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
				if (prop)
				{
					if (strcmp(prop->name, "type") == 0 && props->prop_values[j] == plane_type)
						result = plane->plane_id;
					drmModeFreeProperty(prop);
				}
			}
			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	return result;
}

static void init_debug_flags(DrmDisplay &d)
{
	if (d.debug_flags_initialized)
		return;

	const char *test_pattern = getenv("G64_DRM_TEST_PATTERN");
	const char *force_msync = getenv("G64_DRM_FORCE_MSYNC");
	const char *disable_plane = getenv("G64_DRM_DISABLE_PLANE");
	const char *use_overlay = getenv("G64_DRM_USE_OVERLAY");
	const char *no_vblank_sync = getenv("G64_DRM_NO_VBLANK_SYNC");
	d.debug_test_pattern = test_pattern && test_pattern[0] != '\0' && test_pattern[0] != '0';
	d.debug_force_msync = force_msync && force_msync[0] != '\0' && force_msync[0] != '0';
	d.debug_disable_plane = disable_plane && disable_plane[0] != '\0' && disable_plane[0] != '0';
	d.debug_use_overlay = use_overlay && use_overlay[0] != '\0' && use_overlay[0] != '0';
	d.debug_no_vblank_sync = no_vblank_sync && no_vblank_sync[0] != '\0' && no_vblank_sync[0] != '0';
	d.debug_flags_initialized = true;

	fprintf(stderr, "[drm_display] Debug flags: test_pattern=%s force_msync=%s disable_plane=%s use_overlay=%s no_vblank_sync=%s\n",
	        d.debug_test_pattern ? "on" : "off",
	        d.debug_force_msync ? "on" : "off",
	        d.debug_disable_plane ? "on" : "off",
	        d.debug_use_overlay ? "on" : "off",
	        d.debug_no_vblank_sync ? "on" : "off");
}

static void wait_vblank(DrmDisplay &d)
{
	// Pace plane updates to vblank to avoid scanout/write races on dumb buffers.
	if (d.debug_no_vblank_sync)
		return;

	drmVBlank vbl = {};
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(d.fd, &vbl) < 0 && !d.vblank_error_logged)
	{
		fprintf(stderr, "[drm_display] drmWaitVBlank failed: %s\n", strerror(errno));
		d.vblank_error_logged = true;
	}
}

bool drm_display_init(DrmDisplay &d)
{
	init_debug_flags(d);

	d.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (d.fd < 0)
	{
		fprintf(stderr, "[drm_display] Cannot open /dev/dri/card0: %s\n", strerror(errno));
		return false;
	}

	// Need master for modesetting
	drmSetMaster(d.fd);

	// Enable universal planes
	drmSetClientCap(d.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	drmModeRes *res = drmModeGetResources(d.fd);
	if (!res)
	{
		fprintf(stderr, "[drm_display] getResources: %s\n", strerror(errno));
		return false;
	}

	// Find connected connector
	drmModeConnector *conn = nullptr;
	for (int i = 0; i < res->count_connectors; i++)
	{
		drmModeConnector *c = drmModeGetConnector(d.fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
		{
			conn = c;
			break;
		}
		if (c)
			drmModeFreeConnector(c);
	}

	if (!conn)
	{
		fprintf(stderr, "[drm_display] No connected connector found\n");
		drmModeFreeResources(res);
		return false;
	}

	d.connector_id = conn->connector_id;

	// Use preferred mode
	drmModeModeInfo *mode = nullptr;
	for (int i = 0; i < conn->count_modes; i++)
	{
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
		{
			mode = &conn->modes[i];
			break;
		}
	}
	if (!mode)
		mode = &conn->modes[0];

	d.display_width = mode->hdisplay;
	d.display_height = mode->vdisplay;

	// Find CRTC
	if (conn->encoder_id)
	{
		drmModeEncoder *enc = drmModeGetEncoder(d.fd, conn->encoder_id);
		if (enc)
		{
			d.crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}

	if (!d.crtc_id)
	{
		for (int i = 0; i < conn->count_encoders; i++)
		{
			drmModeEncoder *enc = drmModeGetEncoder(d.fd, conn->encoders[i]);
			if (!enc)
				continue;
			for (int j = 0; j < res->count_crtcs; j++)
			{
				if (enc->possible_crtcs & (1u << j))
				{
					d.crtc_id = res->crtcs[j];
					drmModeFreeEncoder(enc);
					goto found_crtc;
				}
			}
			drmModeFreeEncoder(enc);
		}
	}
found_crtc:

	if (!d.crtc_id)
	{
		fprintf(stderr, "[drm_display] No CRTC found\n");
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return false;
	}

	d.mode_info = *mode;

	// Choose plane for hw scaling.
	if (d.debug_use_overlay)
	{
		d.plane_id = find_plane_by_type(d.fd, d.crtc_id, DRM_PLANE_TYPE_OVERLAY);
		d.plane_is_overlay = d.plane_id != 0;
		if (!d.plane_id)
		{
			d.plane_id = find_plane_by_type(d.fd, d.crtc_id, DRM_PLANE_TYPE_PRIMARY);
			d.plane_is_overlay = false;
		}
	}
	else
	{
		d.plane_id = find_plane_by_type(d.fd, d.crtc_id, DRM_PLANE_TYPE_PRIMARY);
		d.plane_is_overlay = false;
		if (!d.plane_id)
		{
			d.plane_id = find_plane_by_type(d.fd, d.crtc_id, DRM_PLANE_TYPE_OVERLAY);
			d.plane_is_overlay = d.plane_id != 0;
		}
	}
	if (!d.plane_id)
	{
		fprintf(stderr, "[drm_display] No usable plane found, falling back to SetCrtc\n");
		// Not fatal - we'll use drmModeSetCrtc instead of SetPlane
	}

	// Set the CRTC mode with a proper display-sized buffer.
	// This ensures the display pipeline is fully initialized.
	if (!create_dumb_buffer(d.fd, d.mode_buf, d.display_width, d.display_height))
	{
		fprintf(stderr, "[drm_display] Failed to create mode-set buffer (%ux%u)\n",
		        d.display_width, d.display_height);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return false;
	}
	memset(d.mode_buf.map, 0, d.mode_buf.size);

	int err = drmModeSetCrtc(d.fd, d.crtc_id, d.mode_buf.fb_id, 0, 0,
	                         &d.connector_id, 1, mode);
	if (err < 0)
	{
		fprintf(stderr, "[drm_display] setCrtc: %s\n", strerror(errno));
		// Non-fatal: mode might already be active
	}
	else
	{
		fprintf(stderr, "[drm_display] Mode set OK: %ux%u\n", d.display_width, d.display_height);
	}

	d.frame_count = 0;

	fprintf(stderr, "[drm_display] Init OK: connector=%u crtc=%u plane=%u(%s) display=%ux%u\n",
	        d.connector_id, d.crtc_id, d.plane_id,
	        d.plane_is_overlay ? "overlay" : "primary",
	        d.display_width, d.display_height);

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return true;
}

// ---------------------------------------------------------------------------
// NEON helpers for RGBA→XRGB row conversion (write directly to destination)
// ---------------------------------------------------------------------------

#ifdef __aarch64__

// 1:1 format conversion — no scaling. 8 pixels per iteration.
static void neon_row_rgba_to_xrgb_1to1(const uint8_t *src, uint8_t *dst, uint32_t pixel_count)
{
	uint32_t x = 0;
	const uint32_t neon_end = pixel_count & ~7u;
	for (; x < neon_end; x += 8)
	{
		uint8x8x4_t px = vld4_u8(src + x * 4);
		uint8x8x4_t out;
		out.val[0] = px.val[2]; // B
		out.val[1] = px.val[1]; // G
		out.val[2] = px.val[0]; // R
		out.val[3] = vdup_n_u8(0);
		vst4_u8(dst + x * 4, out);
	}
	for (; x < pixel_count; x++)
	{
		const uint8_t *p = src + x * 4;
		reinterpret_cast<uint32_t *>(dst)[x] =
			(uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
	}
}

// 2x horizontal expand + format conversion. Writes width*2 pixels.
static void neon_row_rgba_to_xrgb_2x(const uint8_t *src, uint8_t *dst, uint32_t src_width)
{
	uint32_t *out = reinterpret_cast<uint32_t *>(dst);
	uint32_t src_x = 0;
	const uint32_t neon_end = src_width & ~7u;
	for (; src_x < neon_end; src_x += 8)
	{
		uint8x8x4_t px = vld4_u8(src + src_x * 4);
		uint8x8_t zero = vdup_n_u8(0);

		uint8x8x2_t b_dup = vzip_u8(px.val[2], px.val[2]);
		uint8x8x2_t g_dup = vzip_u8(px.val[1], px.val[1]);
		uint8x8x2_t r_dup = vzip_u8(px.val[0], px.val[0]);
		uint8x8x2_t a_dup = vzip_u8(zero, zero);

		uint8x8x4_t lo = { { b_dup.val[0], g_dup.val[0], r_dup.val[0], a_dup.val[0] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out), lo);
		uint8x8x4_t hi = { { b_dup.val[1], g_dup.val[1], r_dup.val[1], a_dup.val[1] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out + 8), hi);
		out += 16;
	}
	for (; src_x < src_width; src_x++)
	{
		const uint8_t *p = src + src_x * 4;
		uint32_t rgb = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
		*out++ = rgb;
		*out++ = rgb;
	}
}

// 4x horizontal expand + format conversion. Writes width*4 pixels.
static void neon_row_rgba_to_xrgb_4x(const uint8_t *src, uint8_t *dst, uint32_t src_width)
{
	uint32_t *out = reinterpret_cast<uint32_t *>(dst);
	uint32_t src_x = 0;
	const uint32_t neon_end = src_width & ~7u;
	for (; src_x < neon_end; src_x += 8)
	{
		uint8x8x4_t px = vld4_u8(src + src_x * 4);
		uint8x8_t zero = vdup_n_u8(0);

		uint8x8x2_t b2 = vzip_u8(px.val[2], px.val[2]);
		uint8x8x2_t g2 = vzip_u8(px.val[1], px.val[1]);
		uint8x8x2_t r2 = vzip_u8(px.val[0], px.val[0]);
		uint8x8x2_t z2 = vzip_u8(zero, zero);

		uint8x8x2_t b4_lo = vzip_u8(b2.val[0], b2.val[0]);
		uint8x8x2_t g4_lo = vzip_u8(g2.val[0], g2.val[0]);
		uint8x8x2_t r4_lo = vzip_u8(r2.val[0], r2.val[0]);
		uint8x8x2_t z4_lo = vzip_u8(z2.val[0], z2.val[0]);

		uint8x8x4_t blk0 = { { b4_lo.val[0], g4_lo.val[0], r4_lo.val[0], z4_lo.val[0] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out), blk0);
		uint8x8x4_t blk1 = { { b4_lo.val[1], g4_lo.val[1], r4_lo.val[1], z4_lo.val[1] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out + 8), blk1);

		uint8x8x2_t b4_hi = vzip_u8(b2.val[1], b2.val[1]);
		uint8x8x2_t g4_hi = vzip_u8(g2.val[1], g2.val[1]);
		uint8x8x2_t r4_hi = vzip_u8(r2.val[1], r2.val[1]);
		uint8x8x2_t z4_hi = vzip_u8(z2.val[1], z2.val[1]);

		uint8x8x4_t blk2 = { { b4_hi.val[0], g4_hi.val[0], r4_hi.val[0], z4_hi.val[0] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out + 16), blk2);
		uint8x8x4_t blk3 = { { b4_hi.val[1], g4_hi.val[1], r4_hi.val[1], z4_hi.val[1] } };
		vst4_u8(reinterpret_cast<uint8_t *>(out + 24), blk3);
		out += 32;
	}
	for (; src_x < src_width; src_x++)
	{
		const uint8_t *p = src + src_x * 4;
		uint32_t rgb = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
		*out++ = rgb; *out++ = rgb; *out++ = rgb; *out++ = rgb;
	}
}

#endif // __aarch64__

// Scalar row conversion — format convert + optional nearest-neighbor horizontal scale.
static void scalar_row_rgba_to_xrgb(const uint8_t *src, uint8_t *dst,
                                     uint32_t src_width, uint32_t dst_width)
{
	uint32_t *out = reinterpret_cast<uint32_t *>(dst);
	for (uint32_t x = 0; x < dst_width; x++)
	{
		uint32_t src_x = (x * src_width) / dst_width;
		if (src_x >= src_width) src_x = src_width - 1;
		const uint8_t *p = src + src_x * 4;
		out[x] = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
	}
}

// ---------------------------------------------------------------------------

bool drm_display_present(DrmDisplay &d, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
	init_debug_flags(d);

	const uint32_t min_stride = width * 4;
	if (stride < min_stride)
	{
		fprintf(stderr, "[drm_display] Invalid source stride: stride=%u width=%u (need >= %u)\n",
		        stride, width, min_stride);
		return false;
	}

	// Always allocate at display resolution.  The Allwinner DE3.3 hw scaler
	// corrupts non-uniform patterns, so we CPU-upscale and PageFlip at 1:1.
	const uint32_t alloc_width = d.display_width;
	const uint32_t alloc_height = d.display_height;

	if (!d.buffers_ready || d.src_width != width || d.src_height != height)
	{
		// Clean up old buffers
		for (int i = 0; i < 2; i++)
			destroy_dumb_buffer(d.fd, d.buffers[i]);

		d.src_width = width;
		d.src_height = height;

		for (int i = 0; i < 2; i++)
		{
			if (!create_dumb_buffer(d.fd, d.buffers[i], alloc_width, alloc_height))
			{
				fprintf(stderr, "[drm_display] Failed to create buffer %d (%ux%u)\n", i, alloc_width, alloc_height);
				return false;
			}
		}

		d.buffers_ready = true;
		d.current_buffer = 0;
		fprintf(stderr,
		        "[drm_display] Allocated %ux%u dumb buffers (dst_stride=%u, fb_api=%s, format=XRGB8888(swizzle)) input=%ux%u display=%ux%u\n",
		        alloc_width, alloc_height, d.buffers[0].stride,
		        d.buffers[0].legacy_addfb ? "AddFB" : "AddFB2",
		        width, height,
		        d.display_width, d.display_height);
	}

	DrmDisplay::DumbBuffer &buf = d.buffers[d.current_buffer];

	if (d.debug_test_pattern)
	{
		// Deterministic pattern to isolate DRM path from Vulkan readback path.
		uint8_t phase = uint8_t((d.frame_count * 3) & 0xFF);
		for (uint32_t y = 0; y < buf.height; y++)
		{
			uint32_t *dst_row = (uint32_t *)(buf.map + y * buf.stride);
			for (uint32_t x = 0; x < buf.width; x++)
			{
				uint8_t r = uint8_t((x + phase) & 0xFF);
				uint8_t g = uint8_t((y * 2 + phase) & 0xFF);
				uint8_t b = uint8_t(((x ^ y) + phase) & 0xFF);

				if ((x % 64) == 0 || (y % 32) == 0)
				{
					r = g = b = 255;
				}
				if (x < 3 || y < 3 || x >= buf.width - 3 || y >= buf.height - 3)
				{
					r = 255; g = 0; b = 0;
				}
				dst_row[x] = (r << 16) | (g << 8) | b;
			}
		}
	}
	else
	{
		// Copy scanout pixels into dumb buffer.
		// Input is RGBA8888.  FB is XRGB8888.
		//
		// Unified per-axis blit: fixed-point vertical row selection handles
		// upscale, downscale, or 1:1.  Horizontal dispatch picks the best
		// NEON (or scalar) path for the detected ratio.

		const bool h_exact   = (width == buf.width);
		const bool h_up_2x   = (!h_exact && width > 0 && buf.width == width * 2);
		const bool h_up_4x   = (!h_exact && width > 0 && buf.width == width * 4);

		if (!d.blit_path_logged)
		{
			const char *h_tag = h_exact ? "1:1" : h_up_2x ? "2x" : h_up_4x ? "4x" : "generic";
			const char *v_tag = (height == buf.height)  ? "1:1"
			                  : (height > buf.height)    ? "down"
			                                             : "up";
			fprintf(stderr,
			        "[drm_display] Blit path: %ux%u -> %ux%u  H=%s V=%s\n",
			        width, height, buf.width, buf.height, h_tag, v_tag);
			d.blit_path_logged = true;
		}

		for (uint32_t dst_y = 0; dst_y < buf.height; dst_y++)
		{
			// Fixed-point nearest-neighbor vertical row selection.
			uint32_t src_y = (dst_y * height) / buf.height;
			if (src_y >= height) src_y = height - 1;

			const uint8_t *src_row = rgba + src_y * stride;
			uint8_t *dst_row = buf.map + dst_y * buf.stride;

#ifdef __aarch64__
			if (h_exact)
				neon_row_rgba_to_xrgb_1to1(src_row, dst_row, buf.width);
			else if (h_up_2x)
				neon_row_rgba_to_xrgb_2x(src_row, dst_row, width);
			else if (h_up_4x)
				neon_row_rgba_to_xrgb_4x(src_row, dst_row, width);
			else
#endif
				scalar_row_rgba_to_xrgb(src_row, dst_row, width, buf.width);
		}
	}

	if (d.debug_force_msync)
	{
		long page_size = sysconf(_SC_PAGESIZE);
		if (page_size <= 0)
			page_size = 4096;
		uintptr_t start = reinterpret_cast<uintptr_t>(buf.map);
		uintptr_t aligned_start = start & ~uintptr_t(page_size - 1);
		uintptr_t end = start + buf.size;
		uintptr_t aligned_end = (end + uintptr_t(page_size - 1)) & ~uintptr_t(page_size - 1);
		size_t aligned_size = aligned_end - aligned_start;
		if (msync(reinterpret_cast<void *>(aligned_start), aligned_size, MS_SYNC) < 0)
			fprintf(stderr, "[drm_display] msync failed: %s\n", strerror(errno));
	}

	// Some DRM drivers need dirtyfb notification to make CPU writes visible.
	// Probe once and then use it only if supported.
	if (!d.dirtyfb_checked)
	{
		int err = drmModeDirtyFB(d.fd, buf.fb_id, nullptr, 0);
		if (err == 0)
		{
			d.dirtyfb_supported = true;
			fprintf(stderr, "[drm_display] drmModeDirtyFB supported\n");
		}
		else
		{
			d.dirtyfb_supported = false;
			fprintf(stderr, "[drm_display] drmModeDirtyFB unsupported (%s)\n", strerror(errno));
		}
		d.dirtyfb_checked = true;
	}
	else if (d.dirtyfb_supported)
	{
		drmModeDirtyFB(d.fd, buf.fb_id, nullptr, 0);
	}

	// The initial SetCrtc in drm_display_init() established the mode.
	// For frame updates we use PageFlip: it queues a buffer swap at the
	// next vblank without blocking, unlike SetCrtc which does a full modeset.
	//
	// On first frame we must use SetCrtc to associate our scanout buffer
	// with the CRTC (PageFlip only works after a buffer has been displayed).
	if (!d.mode_set)
	{
		int err = drmModeSetCrtc(d.fd, d.crtc_id, buf.fb_id, 0, 0,
		                         &d.connector_id, 1, &d.mode_info);
		if (err < 0)
		{
			fprintf(stderr, "[drm_display] initial setCrtc: %s\n", strerror(errno));
			return false;
		}
		d.mode_set = true;
	}
	else
	{
		int err = drmModePageFlip(d.fd, d.crtc_id, buf.fb_id, 0, nullptr);
		if (err < 0 && errno == EBUSY)
		{
			// Previous flip not yet completed — wait for vblank and retry.
			wait_vblank(d);
			err = drmModePageFlip(d.fd, d.crtc_id, buf.fb_id, 0, nullptr);
		}
		if (err < 0)
		{
			if (!d.setcrtc_error_logged)
			{
				fprintf(stderr, "[drm_display] pageFlip: %s\n", strerror(errno));
				d.setcrtc_error_logged = true;
			}
			return false;
		}
	}

	d.current_buffer ^= 1;
	d.frame_count++;
	return true;
}

bool drm_display_flip(DrmDisplay &d, uint32_t fb_id)
{
	if (!d.mode_set)
	{
		int err = drmModeSetCrtc(d.fd, d.crtc_id, fb_id, 0, 0,
		                         &d.connector_id, 1, &d.mode_info);
		if (err < 0)
		{
			fprintf(stderr, "[drm_display] flip: initial setCrtc: %s\n", strerror(errno));
			return false;
		}
		d.mode_set = true;
	}
	else
	{
		int err = drmModePageFlip(d.fd, d.crtc_id, fb_id, 0, nullptr);
		if (err < 0 && errno == EBUSY)
		{
			wait_vblank(d);
			err = drmModePageFlip(d.fd, d.crtc_id, fb_id, 0, nullptr);
		}
		if (err < 0)
		{
			if (!d.setcrtc_error_logged)
			{
				fprintf(stderr, "[drm_display] flip: pageFlip: %s\n", strerror(errno));
				d.setcrtc_error_logged = true;
			}
			return false;
		}
	}
	d.frame_count++;
	return true;
}

void drm_display_cleanup(DrmDisplay &d)
{
	for (int i = 0; i < 2; i++)
		destroy_dumb_buffer(d.fd, d.buffers[i]);
	destroy_dumb_buffer(d.fd, d.mode_buf);

	if (d.fd >= 0)
	{
		drmDropMaster(d.fd);
		close(d.fd);
		d.fd = -1;
	}

	d.buffers_ready = false;
	d.mode_set = false;
}
