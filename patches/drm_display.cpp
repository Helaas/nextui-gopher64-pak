/*
 * DRM display backend for tg5050
 *
 * Opens /dev/dri/card0, finds the first connected connector,
 * allocates double-buffered dumb buffers at source resolution,
 * and uses drmModeSetPlane with hardware scaling to fill 1280x720.
 */

#include "drm_display.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

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

	// Use AddFB2 with XBGR8888 format — same memory layout as RGBA [R,G,B,X]
	// but ignores the alpha channel (critical: N64 scanout has varying alpha
	// from fog/transparency which would make pixels semi-transparent against
	// the base layer if we used ABGR8888).
	// DRM_FORMAT_XBGR8888: uint32 LE = 0xXXBBGGRR, memory = [R, G, B, X]
	uint32_t handles[4] = {buf.handle, 0, 0, 0};
	uint32_t strides[4] = {buf.stride, 0, 0, 0};
	uint32_t offsets[4] = {0, 0, 0, 0};
	if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XBGR8888, handles, strides, offsets, &buf.fb_id, 0) < 0)
	{
		fprintf(stderr, "[drm_display] addFB2 XBGR8888: %s\n", strerror(errno));
		// Fallback to XRGB8888 with per-pixel swizzle
		if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888, handles, strides, offsets, &buf.fb_id, 0) < 0)
		{
			fprintf(stderr, "[drm_display] addFB2 XRGB8888 fallback: %s\n", strerror(errno));
			return false;
		}
		buf.needs_swizzle = true;
	}
	else
	{
		buf.needs_swizzle = false;
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

// Find the primary plane for a given CRTC
static uint32_t find_primary_plane(int fd, uint32_t crtc_id)
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

		// Check if this is the primary plane via properties
		drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		if (props)
		{
			for (uint32_t j = 0; j < props->count_props; j++)
			{
				drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
				if (prop)
				{
					if (strcmp(prop->name, "type") == 0 && props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
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

bool drm_display_init(DrmDisplay &d)
{
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

	// Find primary plane for hw scaling
	d.plane_id = find_primary_plane(d.fd, d.crtc_id);
	if (!d.plane_id)
	{
		fprintf(stderr, "[drm_display] No primary plane found, falling back to SetCrtc\n");
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

	fprintf(stderr, "[drm_display] Init OK: connector=%u crtc=%u plane=%u display=%ux%u\n",
	        d.connector_id, d.crtc_id, d.plane_id, d.display_width, d.display_height);

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return true;
}

bool drm_display_present(DrmDisplay &d, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
{
	// Allocate buffers on first present or if resolution changed
	if (!d.buffers_ready || d.src_width != width || d.src_height != height)
	{
		// Clean up old buffers
		for (int i = 0; i < 2; i++)
			destroy_dumb_buffer(d.fd, d.buffers[i]);

		d.src_width = width;
		d.src_height = height;

		for (int i = 0; i < 2; i++)
		{
			if (!create_dumb_buffer(d.fd, d.buffers[i], width, height))
			{
				fprintf(stderr, "[drm_display] Failed to create buffer %d (%ux%u)\n", i, width, height);
				return false;
			}
		}

		d.buffers_ready = true;
		d.current_buffer = 0;
		fprintf(stderr, "[drm_display] Allocated %ux%u dumb buffers (stride=%u) for hw-scaled scanout to %ux%u\n",
		        width, height, d.buffers[0].stride, d.display_width, d.display_height);
	}

	DrmDisplay::DumbBuffer &buf = d.buffers[d.current_buffer];

	// Copy scanout pixels into dumb buffer
	if (buf.needs_swizzle)
	{
		// XRGB8888 fallback: per-pixel RGBA → XRGB conversion
		for (uint32_t y = 0; y < height; y++)
		{
			const uint8_t *src_row = rgba + y * stride;
			uint32_t *dst_row = (uint32_t *)(buf.map + y * buf.stride);
			for (uint32_t x = 0; x < width; x++)
			{
				uint8_t r = src_row[x * 4 + 0];
				uint8_t g = src_row[x * 4 + 1];
				uint8_t b = src_row[x * 4 + 2];
				dst_row[x] = (r << 16) | (g << 8) | b;
			}
		}
	}
	else
	{
		// XBGR8888: RGBA bytes match directly, use memcpy per row
		if (stride == buf.stride)
		{
			memcpy(buf.map, rgba, height * stride);
		}
		else
		{
			for (uint32_t y = 0; y < height; y++)
				memcpy(buf.map + y * buf.stride, rgba + y * stride, width * 4);
		}
	}

	if (d.plane_id)
	{
		// Use SetPlane with hardware scaling
		// Source rect is in 16.16 fixed point
		int err = drmModeSetPlane(d.fd, d.plane_id, d.crtc_id, buf.fb_id, 0,
		                         0, 0, d.display_width, d.display_height,           // dst rect
		                         0, 0, width << 16, height << 16);                   // src rect (16.16)
		if (err < 0)
		{
			fprintf(stderr, "[drm_display] setPlane: %s\n", strerror(errno));
			return false;
		}
	}
	else
	{
		// Fallback: no hw scaling
		int err = drmModeSetCrtc(d.fd, d.crtc_id, buf.fb_id, 0, 0,
		                         &d.connector_id, 1, nullptr);
		if (err < 0)
		{
			fprintf(stderr, "[drm_display] setCrtc: %s\n", strerror(errno));
			return false;
		}
	}

	d.current_buffer ^= 1;
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
