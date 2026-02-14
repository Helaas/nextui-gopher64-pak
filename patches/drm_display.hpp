/*
 * DRM display backend for tg5050
 *
 * Manages DRM/KMS modesetting and dumb buffer scanout with hardware
 * plane scaling. Used to bypass the broken VK_KHR_display on Mali-G57.
 *
 * Usage: init() -> present(pixels, w, h) in a loop -> cleanup()
 */

#pragma once

#include <cstdint>
#include <xf86drmMode.h>

struct DrmDisplay
{
	int fd = -1;
	uint32_t connector_id = 0;
	uint32_t crtc_id = 0;
	uint32_t plane_id = 0;

	// Display native resolution
	uint32_t display_width = 0;
	uint32_t display_height = 0;

	// Saved mode for deferred mode setting
	drmModeModeInfo mode_info = {};

	// Double-buffered dumb buffers (allocated at source resolution)
	struct DumbBuffer
	{
		uint32_t handle = 0;
		uint32_t fb_id = 0;
		uint32_t stride = 0;
		uint32_t size = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint8_t *map = nullptr;
		bool legacy_addfb = false;
	};

	DumbBuffer buffers[2] = {};
	DumbBuffer mode_buf = {};  // Display-sized buffer for CRTC mode set (kept alive)
	int current_buffer = 0;
	int frame_count = 0;

	// Source resolution (set on first present)
	uint32_t src_width = 0;
	uint32_t src_height = 0;
	bool buffers_ready = false;
	bool mode_set = false;
	bool dirtyfb_checked = false;
	bool dirtyfb_supported = false;
	bool debug_flags_initialized = false;
	bool debug_test_pattern = false;
	bool debug_force_msync = false;
	bool debug_disable_plane = false;
	bool debug_use_overlay = false;
	bool debug_no_vblank_sync = false;
	bool setcrtc_error_logged = false;
	bool plane_is_overlay = false;
	bool vblank_error_logged = false;
	bool fast_upscale_logged = false;
};

// Initialize DRM: open device, find connector/CRTC/plane, set mode.
// Returns true on success.
bool drm_display_init(DrmDisplay &d);

// Present a frame. Copies RGBA8888 pixels into a dumb buffer and
// issues a plane flip with hardware scaling to display resolution.
// First call allocates buffers sized to (width x height).
bool drm_display_present(DrmDisplay &d, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride);

// Tear down: release buffers, restore CRTC, close fd.
void drm_display_cleanup(DrmDisplay &d);
