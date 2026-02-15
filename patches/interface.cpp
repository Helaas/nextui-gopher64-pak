/*
 * Patched interface.cpp for tg5050
 *
 * Changes from upstream:
 * 1. Uses init_context_from_platform() + init_device() instead of init_simple()
 *    to skip Vulkan surface/swapchain creation (Mali VK_KHR_display is broken).
 * 2. render_frame() uses scanout_sync() for CPU-side pixel readback and
 *    presents via DRM dumb buffers with hardware plane scaling.
 * 3. rdp_update_screen() uses device->next_frame_context() instead of WSI frame management.
 * 4. DRM display is initialized in rdp_init() and cleaned up in rdp_close().
 */

#include "wsi_platform.hpp"
#include "wsi.hpp"
#include "rdp_device.hpp"
#include "interface.hpp"
#include "drm_display.hpp"
#include <SDL3/SDL_vulkan.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cstring>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

using namespace Vulkan;

#define DP_STATUS_XBUS_DMA 0x01
#define DP_STATUS_FREEZE 0x02
#define DP_STATUS_FLUSH 0x04
#define DP_STATUS_START_GCLK 0x008
#define DP_STATUS_TMEM_BUSY 0x010
#define DP_STATUS_PIPE_BUSY 0x020
#define DP_STATUS_CMD_BUSY 0x040
#define DP_STATUS_CBUF_READY 0x080
#define DP_STATUS_DMA_BUSY 0x100
#define DP_STATUS_END_VALID 0x200
#define DP_STATUS_START_VALID 0x400

enum dpc_registers
{
	DPC_START_REG,
	DPC_END_REG,
	DPC_CURRENT_REG,
	DPC_STATUS_REG,
	DPC_CLOCK_REG,
	DPC_BUFBUSY_REG,
	DPC_PIPEBUSY_REG,
	DPC_TMEM_REG,
	DPC_REGS_COUNT
};

enum vi_registers
{
	VI_STATUS_REG,
	VI_ORIGIN_REG,
	VI_WIDTH_REG,
	VI_V_INTR_REG,
	VI_CURRENT_REG,
	VI_BURST_REG,
	VI_V_SYNC_REG,
	VI_H_SYNC_REG,
	VI_LEAP_REG,
	VI_H_START_REG,
	VI_V_START_REG,
	VI_V_BURST_REG,
	VI_X_SCALE_REG,
	VI_Y_SCALE_REG,
	VI_REGS_COUNT
};

typedef struct
{
	uint32_t depthbuffer_address;
	uint32_t framebuffer_address;
	uint32_t framebuffer_y_offset;
	uint32_t texture_address;
	uint32_t framebuffer_pixel_size;
	uint32_t framebuffer_width;
	uint32_t texture_pixel_size;
	uint32_t texture_width;
	uint32_t framebuffer_height;
	bool depth_buffer_enabled;
} FrameBufferInfo;

typedef struct
{
	uint32_t cmd_data[0x00040000 >> 2];
	int cmd_cur;
	int cmd_ptr;
	uint32_t region;
	FrameBufferInfo frame_buffer_info;
} RDP_DEVICE;

static SDL_Window *window;
static RDP::CommandProcessor *processor;
static SDL_WSIPlatform *wsi_platform;
static WSI *wsi;

static RDP_DEVICE rdp_device;
static bool crop_letterbox;
static CALL_BACK callback;
static GFX_INFO gfx_info;

std::vector<bool> rdram_dirty;
uint64_t sync_signal;

static TTF_Font *message_font;
static std::queue<std::string> messages;
static uint64_t message_timer;

// DRM display for scanout
static DrmDisplay drm_display;

// CPU-side pixel buffer for scanout_sync readback (fallback path)
static std::vector<RDP::RGBA> scanout_pixels;
static_assert(sizeof(RDP::RGBA) == 4, "RDP::RGBA must be 4 bytes");

struct PerfMonitor
{
	bool enabled = true;
	uint64_t window_start_ms = 0;
	uint32_t frames_in_window = 0;
	uint64_t sum_scanout_us = 0;
	uint64_t sum_render_us = 0;
	uint64_t sum_flip_us = 0;
	uint64_t sum_total_us = 0;
	uint64_t max_scanout_us = 0;
	uint64_t max_render_us = 0;
	uint64_t max_flip_us = 0;
	uint64_t max_total_us = 0;
	bool paths_initialized = false;
	char sunxi_gpu_info_path[256] = {};
	char cur_freq_path[256] = {};
	char cpu_freq_path[256] = {};
};

static PerfMonitor perf_monitor;

static uint64_t monotonic_ms()
{
	struct timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000ull + uint64_t(ts.tv_nsec) / 1000000ull;
}

static uint64_t monotonic_us()
{
	struct timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

static bool read_text_file(const char *path, char *out, size_t out_size)
{
	if (!out || out_size == 0)
		return false;
	out[0] = '\0';

	FILE *fp = fopen(path, "r");
	if (!fp)
		return false;

	size_t n = fread(out, 1, out_size - 1, fp);
	out[n] = '\0';

	fclose(fp);
	return n > 0;
}

static bool parse_first_integer_after(const char *s, const char *key, int &value_out)
{
	if (!s || !key)
		return false;

	const char *pos = strstr(s, key);
	if (!pos)
		return false;
	pos += strlen(key);

	while (*pos == ' ' || *pos == '\t')
		pos++;
	if (*pos == '\0')
		return false;

	char *end = nullptr;
	long v = strtol(pos, &end, 10);
	if (end == pos)
		return false;

	value_out = static_cast<int>(v);
	return true;
}

static bool parse_gpu_util_and_mhz(const char *s, int &util_percent, int &mhz)
{
	bool got_util = parse_first_integer_after(s, "Utilisation from last show:", util_percent);
	bool got_mhz = parse_first_integer_after(s, "Frequency:", mhz);
	return got_util || got_mhz;
}

static bool read_int_file(const char *path, int &value_out)
{
	char text[64] = {};
	if (!read_text_file(path, text, sizeof(text)))
		return false;
	char *end = nullptr;
	long v = strtol(text, &end, 10);
	if (end == text)
		return false;
	value_out = static_cast<int>(v);
	return true;
}

static void init_perf_monitor()
{
	const char *env = getenv("G64_PERF_LOG");
	if (env && env[0] == '0')
		perf_monitor.enabled = false;

	if (!perf_monitor.enabled || perf_monitor.paths_initialized)
		return;

	const char *sunxi_candidates[] = {
		"/sys/devices/platform/soc@3000000/1800000.gpu/sunxi_gpu/sunxi_gpu_freq",
		"/sys/devices/platform/1800000.gpu/sunxi_gpu/sunxi_gpu_freq",
		"/sys/class/devfreq/1800000.gpu/device/sunxi_gpu/sunxi_gpu_freq"
	};
	for (size_t i = 0; i < sizeof(sunxi_candidates) / sizeof(sunxi_candidates[0]); i++)
	{
		const char *p = sunxi_candidates[i];
		if (access(p, R_OK) == 0)
		{
			snprintf(perf_monitor.sunxi_gpu_info_path,
			         sizeof(perf_monitor.sunxi_gpu_info_path), "%s", p);
			break;
		}
	}

	const char *cur_freq_candidates[] = {
		"/sys/class/devfreq/1800000.gpu/cur_freq",
		"/sys/devices/platform/soc@3000000/1800000.gpu/devfreq/1800000.gpu/cur_freq",
		"/sys/devices/platform/1800000.gpu/devfreq/1800000.gpu/cur_freq"
	};
	for (size_t i = 0; i < sizeof(cur_freq_candidates) / sizeof(cur_freq_candidates[0]); i++)
	{
		const char *p = cur_freq_candidates[i];
		if (access(p, R_OK) == 0)
		{
			snprintf(perf_monitor.cur_freq_path,
			         sizeof(perf_monitor.cur_freq_path), "%s", p);
			break;
		}
	}

	const char *cpu_freq_candidates[] = {
		"/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
		"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq"
	};
	for (size_t i = 0; i < sizeof(cpu_freq_candidates) / sizeof(cpu_freq_candidates[0]); i++)
	{
		const char *p = cpu_freq_candidates[i];
		if (access(p, R_OK) == 0)
		{
			snprintf(perf_monitor.cpu_freq_path,
			         sizeof(perf_monitor.cpu_freq_path), "%s", p);
			break;
		}
	}

	perf_monitor.paths_initialized = true;
}

static void perf_monitor_frame(const char *path_tag,
                               uint64_t scanout_us,
                               uint64_t render_us,
                               uint64_t flip_us,
                               uint64_t total_us)
{
	if (!perf_monitor.enabled)
		return;

	if (!perf_monitor.paths_initialized)
		init_perf_monitor();

	uint64_t now_ms = monotonic_ms();
	if (perf_monitor.window_start_ms == 0)
		perf_monitor.window_start_ms = now_ms;

	perf_monitor.frames_in_window++;
	perf_monitor.sum_scanout_us += scanout_us;
	perf_monitor.sum_render_us += render_us;
	perf_monitor.sum_flip_us += flip_us;
	perf_monitor.sum_total_us += total_us;
	if (scanout_us > perf_monitor.max_scanout_us)
		perf_monitor.max_scanout_us = scanout_us;
	if (render_us > perf_monitor.max_render_us)
		perf_monitor.max_render_us = render_us;
	if (flip_us > perf_monitor.max_flip_us)
		perf_monitor.max_flip_us = flip_us;
	if (total_us > perf_monitor.max_total_us)
		perf_monitor.max_total_us = total_us;

	uint64_t elapsed_ms = now_ms - perf_monitor.window_start_ms;
	if (elapsed_ms < 1000)
		return;

	double fps = (1000.0 * perf_monitor.frames_in_window) / double(elapsed_ms);
	int gpu_util = -1;
	int gpu_mhz = -1;
	int cpu_mhz = -1;

	if (perf_monitor.sunxi_gpu_info_path[0] != '\0')
	{
		char text[1024] = {};
		if (read_text_file(perf_monitor.sunxi_gpu_info_path, text, sizeof(text)))
			parse_gpu_util_and_mhz(text, gpu_util, gpu_mhz);
	}

	if (gpu_mhz < 0 && perf_monitor.cur_freq_path[0] != '\0')
	{
		int hz = 0;
		if (read_int_file(perf_monitor.cur_freq_path, hz) && hz > 0)
			gpu_mhz = hz / 1000000;
	}

	if (perf_monitor.cpu_freq_path[0] != '\0')
	{
		int hz = 0;
		if (read_int_file(perf_monitor.cpu_freq_path, hz) && hz > 0)
			cpu_mhz = hz / 1000;
	}

	const double frames = double(perf_monitor.frames_in_window);
	const double avg_scanout_ms = double(perf_monitor.sum_scanout_us) / (1000.0 * frames);
	const double avg_render_ms = double(perf_monitor.sum_render_us) / (1000.0 * frames);
	const double avg_flip_ms = double(perf_monitor.sum_flip_us) / (1000.0 * frames);
	const double avg_total_ms = double(perf_monitor.sum_total_us) / (1000.0 * frames);
	const double max_total_ms = double(perf_monitor.max_total_us) / 1000.0;

	if (gpu_util >= 0 && gpu_mhz >= 0 && cpu_mhz >= 0)
	{
		fprintf(stderr,
		        "[perf] path=%s fps=%.1f cpu=%dMHz gpu=%d%%@%dMHz "
		        "stage_ms(avg scanout=%.2f render=%.2f flip=%.2f total=%.2f max_total=%.2f)\n",
		        path_tag, fps, cpu_mhz, gpu_util, gpu_mhz,
		        avg_scanout_ms, avg_render_ms, avg_flip_ms, avg_total_ms, max_total_ms);
	}
	else if (gpu_mhz >= 0 && cpu_mhz >= 0)
	{
		fprintf(stderr,
		        "[perf] path=%s fps=%.1f cpu=%dMHz gpu_freq=%dMHz "
		        "stage_ms(avg scanout=%.2f render=%.2f flip=%.2f total=%.2f max_total=%.2f)\n",
		        path_tag, fps, cpu_mhz, gpu_mhz,
		        avg_scanout_ms, avg_render_ms, avg_flip_ms, avg_total_ms, max_total_ms);
	}
	else if (cpu_mhz >= 0)
	{
		fprintf(stderr,
		        "[perf] path=%s fps=%.1f cpu=%dMHz "
		        "stage_ms(avg scanout=%.2f render=%.2f flip=%.2f total=%.2f max_total=%.2f)\n",
		        path_tag, fps, cpu_mhz,
		        avg_scanout_ms, avg_render_ms, avg_flip_ms, avg_total_ms, max_total_ms);
	}
	else
	{
		fprintf(stderr,
		        "[perf] path=%s fps=%.1f "
		        "stage_ms(avg scanout=%.2f render=%.2f flip=%.2f total=%.2f max_total=%.2f)\n",
		        path_tag, fps,
		        avg_scanout_ms, avg_render_ms, avg_flip_ms, avg_total_ms, max_total_ms);
	}

	perf_monitor.window_start_ms = now_ms;
	perf_monitor.frames_in_window = 0;
	perf_monitor.sum_scanout_us = 0;
	perf_monitor.sum_render_us = 0;
	perf_monitor.sum_flip_us = 0;
	perf_monitor.sum_total_us = 0;
	perf_monitor.max_scanout_us = 0;
	perf_monitor.max_render_us = 0;
	perf_monitor.max_flip_us = 0;
	perf_monitor.max_total_us = 0;
}

// ---------------------------------------------------------------------------
// Zero-copy GPU→DRM display via DMA-buf
// ---------------------------------------------------------------------------
struct GpuDisplayBuffer
{
	Vulkan::ImageHandle image;
	uint32_t drm_fb_id = 0;
	uint32_t gem_handle = 0;
};

static GpuDisplayBuffer gpu_display_bufs[2];
static int gpu_display_idx = 0;
static bool gpu_display_ready = false;
static bool gpu_display_failed = false;

static bool init_gpu_display(Vulkan::Device &device)
{
	if (gpu_display_failed)
		return false;
	if (gpu_display_ready)
		return true;

	const uint32_t dw = drm_display.display_width;
	const uint32_t dh = drm_display.display_height;

	for (int i = 0; i < 2; i++)
	{
		// 1. Create DRM dumb buffer at display resolution
		struct drm_mode_create_dumb create_req = {};
		create_req.width = dw;
		create_req.height = dh;
		create_req.bpp = 32;
		if (drmIoctl(drm_display.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
		{
			fprintf(stderr, "[gpu_display] Failed to create dumb buffer %d: %s\n",
			        i, strerror(errno));
			gpu_display_failed = true;
			return false;
		}
		const uint32_t stride = create_req.pitch;
		const uint32_t gem_handle = create_req.handle;

		// 2. Register as DRM framebuffer
		// VK_FORMAT_R8G8B8A8_UNORM bytes [R,G,B,A] = DRM_FORMAT_ABGR8888
		uint32_t handles[4] = { gem_handle, 0, 0, 0 };
		uint32_t strides[4] = { stride, 0, 0, 0 };
		uint32_t offsets[4] = { 0, 0, 0, 0 };
		uint32_t fb_id = 0;

		int ret = drmModeAddFB2(drm_display.fd, dw, dh,
		                        DRM_FORMAT_ABGR8888, handles, strides, offsets, &fb_id, 0);
		if (ret < 0)
		{
			fprintf(stderr, "[gpu_display] AddFB2 ABGR8888 failed (%s), trying XBGR8888\n",
			        strerror(errno));
			ret = drmModeAddFB2(drm_display.fd, dw, dh,
			                    DRM_FORMAT_XBGR8888, handles, strides, offsets, &fb_id, 0);
		}
		if (ret < 0)
		{
			fprintf(stderr, "[gpu_display] AddFB2 failed: %s\n", strerror(errno));
			// Clean up dumb buffer
			struct drm_mode_destroy_dumb destroy_req = {};
			destroy_req.handle = gem_handle;
			drmIoctl(drm_display.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			gpu_display_failed = true;
			return false;
		}

		// 3. Export dumb buffer as DMA-buf fd
		int dmabuf_fd = -1;
		if (drmPrimeHandleToFD(drm_display.fd, gem_handle,
		                       DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0)
		{
			fprintf(stderr, "[gpu_display] drmPrimeHandleToFD: %s\n", strerror(errno));
			drmModeRmFB(drm_display.fd, fb_id);
			struct drm_mode_destroy_dumb destroy_req = {};
			destroy_req.handle = gem_handle;
			drmIoctl(drm_display.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			gpu_display_failed = true;
			return false;
		}

		// 4. Import DMA-buf fd into Vulkan as an image
		VkSubresourceLayout plane_layout = {};
		plane_layout.offset = 0;
		plane_layout.rowPitch = stride;
		// For single-plane 2D import, only offset + rowPitch are required.
		// Keeping the remaining fields zero avoids strict-driver rejects.
		plane_layout.size = 0;
		plane_layout.depthPitch = 0;
		plane_layout.arrayPitch = 0;

		VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod = {
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT
		};
		drm_mod.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
		drm_mod.drmFormatModifierPlaneCount = 1;
		drm_mod.pPlaneLayouts = &plane_layout;

		Vulkan::ImageCreateInfo img_ci = {};
		img_ci.domain = Vulkan::ImageDomain::Physical;
		img_ci.width = dw;
		img_ci.height = dh;
		img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
		img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		// Granite requires UNDEFINED initial layout for external-memory images.
		img_ci.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		img_ci.misc = Vulkan::IMAGE_MISC_EXTERNAL_MEMORY_BIT
		            | Vulkan::IMAGE_MISC_NO_DEFAULT_VIEWS_BIT;
		img_ci.external.memory_handle_type =
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		img_ci.external.handle = dmabuf_fd;  // Granite imports (and closes) the fd
		img_ci.pnext = &drm_mod;

		auto image = device.create_image(img_ci);
		if (!image)
		{
			fprintf(stderr, "[gpu_display] Failed to import DMA-buf into Vulkan (buf %d)\n", i);
			if (dmabuf_fd >= 0)
				close(dmabuf_fd);
			drmModeRmFB(drm_display.fd, fb_id);
			struct drm_mode_destroy_dumb destroy_req = {};
			destroy_req.handle = gem_handle;
			drmIoctl(drm_display.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			gpu_display_failed = true;
			return false;
		}

		gpu_display_bufs[i].image = std::move(image);
		gpu_display_bufs[i].drm_fb_id = fb_id;
		gpu_display_bufs[i].gem_handle = gem_handle;
	}

	gpu_display_ready = true;
	fprintf(stderr, "[gpu_display] Zero-copy GPU->DRM ready (import): %ux%u\n", dw, dh);
	return true;
}

static void cleanup_gpu_display()
{
	for (int i = 0; i < 2; i++)
	{
		// Release Vulkan image first (releases imported DMA-buf reference)
		gpu_display_bufs[i].image.reset();

		if (gpu_display_bufs[i].drm_fb_id)
		{
			drmModeRmFB(drm_display.fd, gpu_display_bufs[i].drm_fb_id);
			gpu_display_bufs[i].drm_fb_id = 0;
		}
		if (gpu_display_bufs[i].gem_handle)
		{
			struct drm_mode_destroy_dumb destroy_req = {};
			destroy_req.handle = gpu_display_bufs[i].gem_handle;
			drmIoctl(drm_display.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			gpu_display_bufs[i].gem_handle = 0;
		}
	}
	gpu_display_ready = false;
	gpu_display_failed = false;
}

#define MESSAGE_TIME 3000 // 3 seconds

static const unsigned cmd_len_lut[64] = {
	1, 1, 1, 1, 1, 1, 1, 1,
	4, 6, 12, 14, 12, 14, 20, 22,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 2, 2, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
};

bool sdl_event_filter(void *userdata, SDL_Event *event)
{
	if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
	{
		callback.paused = false;
		callback.emu_running = false;
	}
	else if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat)
	{
		switch (event->key.scancode)
		{
		case SDL_SCANCODE_RETURN:
			if (event->key.mod & SDL_KMOD_ALT)
			{
				gfx_info.fullscreen = !gfx_info.fullscreen;
				SDL_SetWindowFullscreen(window, gfx_info.fullscreen);
			}
			break;
		case SDL_SCANCODE_F:
			if (event->key.mod & SDL_KMOD_ALT)
			{
				callback.enable_speedlimiter = !callback.enable_speedlimiter;
			}
			break;
		case SDL_SCANCODE_P:
			if (event->key.mod & SDL_KMOD_ALT)
			{
				callback.paused = !callback.paused;
			}
			break;
		case SDL_SCANCODE_ESCAPE:
			if (gfx_info.fullscreen)
				callback.emu_running = false;
			break;
		case SDL_SCANCODE_F4:
			crop_letterbox = !crop_letterbox;
			break;
		case SDL_SCANCODE_F5:
			callback.save_state = true;
			break;
		case SDL_SCANCODE_F7:
			callback.load_state = true;
			break;
		case SDL_SCANCODE_LEFTBRACKET:
			callback.lower_volume = true;
			break;
		case SDL_SCANCODE_RIGHTBRACKET:
			callback.raise_volume = true;
			break;
		case SDL_SCANCODE_SLASH:
			callback.frame_advance = true;
			break;
		case SDL_SCANCODE_0:
		case SDL_SCANCODE_1:
		case SDL_SCANCODE_2:
		case SDL_SCANCODE_3:
		case SDL_SCANCODE_4:
		case SDL_SCANCODE_5:
		case SDL_SCANCODE_6:
		case SDL_SCANCODE_7:
		case SDL_SCANCODE_8:
		case SDL_SCANCODE_9:
			if (event->key.mod & SDL_KMOD_ALT)
			{
				if (event->key.scancode == SDL_SCANCODE_0)
					callback.save_state_slot = 0;
				else
					callback.save_state_slot = event->key.scancode - SDL_SCANCODE_1 + 1;
			}
			break;
		default:
			break;
		}
	}

	return 0;
}

void rdp_new_processor(GFX_INFO _gfx_info)
{
	gfx_info = _gfx_info;

	sync_signal = 0;
	rdram_dirty.assign(gfx_info.RDRAM_SIZE >> 3, false);

	if (processor)
	{
		delete processor;
	}
	RDP::CommandProcessorFlags flags = 0;

	if (gfx_info.upscale == 2)
	{
		flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
	}
	else if (gfx_info.upscale == 4)
	{
		flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
	}
	else if (gfx_info.upscale == 8)
	{
		flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
		flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
	}

	processor = new RDP::CommandProcessor(wsi->get_device(), gfx_info.RDRAM, 0, gfx_info.RDRAM_SIZE, gfx_info.RDRAM_SIZE / 2, flags);
}

void rdp_init(void *_window, GFX_INFO _gfx_info, const void *font, size_t font_size)
{
	memset(&rdp_device, 0, sizeof(RDP_DEVICE));

	window = (SDL_Window *)_window;
	SDL_SyncWindow(window);
	bool result = SDL_AddEventWatch(sdl_event_filter, nullptr);
	if (!result)
	{
		printf("Could not add event watch.\n");
		return;
	}

	gfx_info = _gfx_info;

	// Initialize DRM display for scanout
	if (!drm_display_init(drm_display))
	{
		printf("[interface] Failed to initialize DRM display\n");
		rdp_close();
		return;
	}

	// Initialize Vulkan for compute only (no WSI surface/swapchain)
	wsi = new WSI;
	wsi_platform = new SDL_WSIPlatform;
	wsi_platform->set_window(window);
	wsi->set_platform(wsi_platform);

	Context::SystemHandles handles = {};
	if (!::Vulkan::Context::init_loader((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr()))
	{
		printf("[interface] Failed to init Vulkan loader\n");
		rdp_close();
		return;
	}

	// Use init_context_from_platform + init_device instead of init_simple.
	// This skips init_surface_swapchain() which would try to create a Vulkan
	// surface — and that crashes on Mali-G57's broken VK_KHR_display.
	if (!wsi->init_context_from_platform(1, handles))
	{
		printf("[interface] Failed to create Vulkan context\n");
		rdp_close();
		return;
	}
	if (!wsi->init_device())
	{
		printf("[interface] Failed to create Vulkan device\n");
		rdp_close();
		return;
	}

	rdp_new_processor(gfx_info);

	if (!processor->device_is_supported())
	{
		printf("[interface] GPU device not supported by parallel-rdp\n");
		rdp_close();
		return;
	}

	message_font = TTF_OpenFontIO(SDL_IOFromConstMem(font, font_size), true, 30.0);
	if (!message_font)
	{
		printf("[interface] Failed to open font\n");
		rdp_close();
		return;
	}

	// No wsi->begin_frame() — we manage frame context directly
	auto &device = wsi->get_device();
	device.next_frame_context();

	callback.emu_running = true;
	callback.enable_speedlimiter = true;
	callback.paused = false;
	callback.save_state_slot = 0;
	crop_letterbox = false;

	messages = std::queue<std::string>();
	message_timer = 0;

	fprintf(stderr, "[interface] Init complete: Vulkan compute + DRM scanout\n");
}

void rdp_close()
{
	if (wsi)
	{
		auto &device = wsi->get_device();
		device.wait_idle();
	}

	cleanup_gpu_display();
	drm_display_cleanup(drm_display);

	if (message_font)
	{
		TTF_CloseFont(message_font);
		message_font = nullptr;
	}
	if (processor)
	{
		delete processor;
		processor = nullptr;
	}
	if (wsi)
	{
		delete wsi;
		wsi = nullptr;
	}
	if (wsi_platform)
	{
		delete wsi_platform;
		wsi_platform = nullptr;
	}
}

static void render_frame(Vulkan::Device &device)
{
	RDP::ScanoutOptions options = {};
	const uint64_t frame_start_us = monotonic_us();
	options.persist_frame_on_invalid_input = true;
	options.blend_previous_frame = true;
	options.upscale_deinterlacing = false;

	if (crop_letterbox && gfx_info.widescreen)
	{
		options.crop_rect.enable = true;
		if (gfx_info.PAL)
		{
			options.crop_rect.top = 36;
			options.crop_rect.bottom = 36;
		}
		else
		{
			options.crop_rect.top = 30;
			options.crop_rect.bottom = 30;
		}
	}

	// ---------------------------------------------------------------
	// Zero-copy GPU path: scanout → GPU blit → DMA-buf → DRM flip
	// ---------------------------------------------------------------
	if (init_gpu_display(device))
	{
		auto scanout_image = processor->scanout(options);
		const uint64_t scanout_done_us = monotonic_us();
		if (!scanout_image)
			return;

		static bool logged_gpu_first = false;
		if (!logged_gpu_first)
		{
			fprintf(stderr, "[gpu_display] First scanout: %ux%u -> %ux%u (GPU blit)\n",
			        scanout_image->get_width(), scanout_image->get_height(),
			        drm_display.display_width, drm_display.display_height);
			logged_gpu_first = true;
		}

		auto &dst_buf = gpu_display_bufs[gpu_display_idx];
		auto cmd = device.request_command_buffer();

		// Transition display image to TRANSFER_DST
		cmd->image_barrier(*dst_buf.image,
		                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
		                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		// GPU blit: scale + copy (same format, no swizzle needed)
		VkOffset3D src_offset0 = { 0, 0, 0 };
		VkOffset3D src_extent = {
			static_cast<int32_t>(scanout_image->get_width()),
			static_cast<int32_t>(scanout_image->get_height()), 1
		};
		VkOffset3D dst_offset0 = { 0, 0, 0 };
		VkOffset3D dst_extent = {
			static_cast<int32_t>(drm_display.display_width),
			static_cast<int32_t>(drm_display.display_height), 1
		};

		cmd->blit_image(*dst_buf.image, *scanout_image,
		                dst_offset0, dst_extent,
		                src_offset0, src_extent,
		                0, 0, 0, 0, 1,
		                VK_FILTER_NEAREST);

		// Barrier: make writes visible to external (DRM) consumer
		cmd->image_barrier(*dst_buf.image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		                   VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

		Vulkan::Fence fence;
		device.submit(cmd, &fence);
		fence->wait();
		const uint64_t gpu_done_us = monotonic_us();

		if (drm_display_flip(drm_display, dst_buf.drm_fb_id))
		{
			const uint64_t flip_done_us = monotonic_us();
			perf_monitor_frame("gpu-dmabuf",
			                   scanout_done_us - frame_start_us,
			                   gpu_done_us - scanout_done_us,
			                   flip_done_us - gpu_done_us,
			                   flip_done_us - frame_start_us);
			gpu_display_idx ^= 1;
		}
		return;
	}

	// ---------------------------------------------------------------
	// Fallback: CPU readback + NEON blit (if GPU display failed)
	// ---------------------------------------------------------------
	unsigned width = 0, height = 0;
	processor->scanout_sync(scanout_pixels, width, height, options);
	const uint64_t scanout_done_us = monotonic_us();

	if (width == 0 || height == 0 || scanout_pixels.empty())
		return;

	const size_t expected_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (scanout_pixels.size() < expected_pixels)
	{
		fprintf(stderr, "[rdp] scanout_sync size mismatch: got %zu, expected at least %zu (%ux%u)\n",
		        scanout_pixels.size(), expected_pixels, width, height);
		return;
	}

	const uint32_t src_stride = width * sizeof(RDP::RGBA);

	static bool logged_first_frame = false;
	if (!logged_first_frame)
	{
		fprintf(stderr, "[rdp] First scanout: %ux%u, %zu pixels, src_stride=%u (CPU fallback)\n",
		        width, height, scanout_pixels.size(), src_stride);
		logged_first_frame = true;
	}

	if (drm_display_present(drm_display,
	                        reinterpret_cast<const uint8_t *>(scanout_pixels.data()),
	                        width, height, src_stride))
	{
		const uint64_t present_done_us = monotonic_us();
		perf_monitor_frame("cpu-fallback",
		                   scanout_done_us - frame_start_us,
		                   present_done_us - scanout_done_us,
		                   0,
		                   present_done_us - frame_start_us);
	}
}

void rdp_set_vi_register(uint32_t reg, uint32_t value)
{
	processor->set_vi_register(RDP::VIRegister(reg), value);
}

void rdp_render_frame()
{
	auto &device = wsi->get_device();
	render_frame(device);
}

void rdp_update_screen()
{
	// No WSI swapchain — manage frame context directly
	auto &device = wsi->get_device();
	device.end_frame_context();
	device.next_frame_context();
}

CALL_BACK rdp_check_callback()
{
	CALL_BACK return_value = callback;
	callback.save_state = false;
	callback.load_state = false;
	callback.lower_volume = false;
	callback.raise_volume = false;
	callback.frame_advance = false;
	return return_value;
}

void rdp_check_framebuffers(uint32_t address, uint32_t length)
{
	if (sync_signal)
	{
		address >>= 3;
		length = (length + 7) >> 3;

		if (address >= rdram_dirty.size())
			return;

		uint32_t end_addr = std::min(address + length, static_cast<uint32_t>(rdram_dirty.size()));

		auto it = std::find(rdram_dirty.begin() + address, rdram_dirty.begin() + end_addr, true);
		if (it != rdram_dirty.begin() + end_addr)
		{
			processor->wait_for_timeline(sync_signal);
			rdram_dirty.assign(gfx_info.RDRAM_SIZE >> 3, false);
			sync_signal = 0;
		}
	}
}

size_t rdp_state_size()
{
	return sizeof(RDP_DEVICE);
}

void rdp_save_state(uint8_t *state)
{
	processor->wait_for_timeline(processor->signal_timeline());
	memcpy(state, &rdp_device, sizeof(RDP_DEVICE));
}

void rdp_load_state(const uint8_t *state)
{
	memcpy(&rdp_device, state, sizeof(RDP_DEVICE));
}

void rdp_onscreen_message(const char *_message)
{
	if (messages.empty())
		message_timer = SDL_GetTicks() + MESSAGE_TIME;
	messages.push(_message);
}

uint32_t pixel_size(uint32_t pixel_type, uint32_t area)
{
	switch (pixel_type)
	{
	case 0:
		return area / 2;
	case 1:
		return area;
	case 2:
		return area * 2;
	case 3:
		return area * 4;
	default:
		printf("Invalid pixel size: %u\n", pixel_type);
		return 0;
	}
}

uint64_t rdp_process_commands()
{
	uint64_t interrupt_timer = 0;
	const uint32_t DP_CURRENT = *gfx_info.DPC_CURRENT_REG & 0x00FFFFF8;
	const uint32_t DP_END = *gfx_info.DPC_END_REG & 0x00FFFFF8;

	int length = DP_END - DP_CURRENT;
	if (length <= 0)
		return interrupt_timer;

	length = unsigned(length) >> 3;
	if ((rdp_device.cmd_ptr + length) & ~(0x0003FFFF >> 3))
		return interrupt_timer;

	uint32_t offset = DP_CURRENT;
	if (*gfx_info.DPC_STATUS_REG & DP_STATUS_XBUS_DMA)
	{
		do
		{
			offset &= 0xFF8;
			rdp_device.cmd_data[2 * rdp_device.cmd_ptr + 0] = SDL_Swap32BE(*reinterpret_cast<const uint32_t *>(gfx_info.DMEM + offset));
			rdp_device.cmd_data[2 * rdp_device.cmd_ptr + 1] = SDL_Swap32BE(*reinterpret_cast<const uint32_t *>(gfx_info.DMEM + offset + 4));
			offset += sizeof(uint64_t);
			rdp_device.cmd_ptr++;
		} while (--length > 0);
	}
	else
	{
		if (DP_END > 0x7ffffff || DP_CURRENT > 0x7ffffff)
		{
			return interrupt_timer;
		}
		else
		{
			do
			{
				offset &= 0xFFFFF8;
				rdp_device.cmd_data[2 * rdp_device.cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(gfx_info.RDRAM + offset);
				rdp_device.cmd_data[2 * rdp_device.cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(gfx_info.RDRAM + offset + 4);
				offset += sizeof(uint64_t);
				rdp_device.cmd_ptr++;
			} while (--length > 0);
		}
	}

	while (rdp_device.cmd_cur - rdp_device.cmd_ptr < 0)
	{
		uint32_t w1 = rdp_device.cmd_data[2 * rdp_device.cmd_cur];
		uint32_t w2 = rdp_device.cmd_data[2 * rdp_device.cmd_cur + 1];
		uint32_t command = (w1 >> 24) & 63;
		int cmd_length = cmd_len_lut[command];

		if (rdp_device.cmd_ptr - rdp_device.cmd_cur - cmd_length < 0)
		{
			*gfx_info.DPC_START_REG = *gfx_info.DPC_CURRENT_REG = *gfx_info.DPC_END_REG;
			return interrupt_timer;
		}

		if (command >= 8)
			processor->enqueue_command(cmd_length * 2, &rdp_device.cmd_data[2 * rdp_device.cmd_cur]);

		switch (RDP::Op(command))
		{
		case RDP::Op::FillTriangle:
		case RDP::Op::FillZBufferTriangle:
		case RDP::Op::TextureTriangle:
		case RDP::Op::TextureZBufferTriangle:
		case RDP::Op::ShadeTriangle:
		case RDP::Op::ShadeZBufferTriangle:
		case RDP::Op::ShadeTextureTriangle:
		case RDP::Op::ShadeTextureZBufferTriangle:
		case RDP::Op::TextureRectangle:
		case RDP::Op::TextureRectangleFlip:
		case RDP::Op::FillRectangle:
		{
			uint32_t offset_address = (rdp_device.frame_buffer_info.framebuffer_address + pixel_size(rdp_device.frame_buffer_info.framebuffer_pixel_size, rdp_device.frame_buffer_info.framebuffer_y_offset * rdp_device.frame_buffer_info.framebuffer_width)) >> 3;
			if (offset_address < rdram_dirty.size() && !rdram_dirty[offset_address])
			{
				uint32_t end_addr = std::min(offset_address + ((pixel_size(rdp_device.frame_buffer_info.framebuffer_pixel_size, rdp_device.frame_buffer_info.framebuffer_width * rdp_device.frame_buffer_info.framebuffer_height) + 7) >> 3), static_cast<uint32_t>(rdram_dirty.size()));
				std::fill(rdram_dirty.begin() + offset_address, rdram_dirty.begin() + end_addr, true);
			}

			if (rdp_device.frame_buffer_info.depth_buffer_enabled)
			{
				offset_address = (rdp_device.frame_buffer_info.depthbuffer_address + pixel_size(2, rdp_device.frame_buffer_info.framebuffer_y_offset * rdp_device.frame_buffer_info.framebuffer_width)) >> 3;
				if (offset_address < rdram_dirty.size() && !rdram_dirty[offset_address])
				{
					uint32_t end_addr = std::min(offset_address + ((pixel_size(2, rdp_device.frame_buffer_info.framebuffer_width * rdp_device.frame_buffer_info.framebuffer_height) + 7) >> 3), static_cast<uint32_t>(rdram_dirty.size()));
					std::fill(rdram_dirty.begin() + offset_address, rdram_dirty.begin() + end_addr, true);
				}
			}
		}
		break;
		case RDP::Op::LoadTLut:
		case RDP::Op::LoadTile:
		{
			uint32_t upper_left_t = (w1 & 0xFFF) >> 2;
			uint32_t offset_address = (rdp_device.frame_buffer_info.texture_address + pixel_size(rdp_device.frame_buffer_info.texture_pixel_size, upper_left_t * rdp_device.frame_buffer_info.texture_width)) >> 3;
			if (offset_address < rdram_dirty.size() && !rdram_dirty[offset_address])
			{
				uint32_t lower_right_t = (w2 & 0xFFF) >> 2;
				uint32_t end_addr = std::min(offset_address + ((pixel_size(rdp_device.frame_buffer_info.texture_pixel_size, (lower_right_t - upper_left_t) * rdp_device.frame_buffer_info.texture_width) + 7) >> 3), static_cast<uint32_t>(rdram_dirty.size()));
				std::fill(rdram_dirty.begin() + offset_address, rdram_dirty.begin() + end_addr, true);
			}
		}
		break;
		case RDP::Op::LoadBlock:
		{
			uint32_t upper_left_s = ((w1 >> 12) & 0xFFF);
			uint32_t upper_left_t = (w1 & 0xFFF);
			uint32_t offset_address = (rdp_device.frame_buffer_info.texture_address + pixel_size(rdp_device.frame_buffer_info.texture_pixel_size, upper_left_s + upper_left_t * rdp_device.frame_buffer_info.texture_width)) >> 3;
			if (offset_address < rdram_dirty.size() && !rdram_dirty[offset_address])
			{
				uint32_t lower_right_s = ((w2 >> 12) & 0xFFF);
				uint32_t end_addr = std::min(offset_address + ((pixel_size(rdp_device.frame_buffer_info.texture_pixel_size, lower_right_s - upper_left_s) + 7) >> 3), static_cast<uint32_t>(rdram_dirty.size()));
				std::fill(rdram_dirty.begin() + offset_address, rdram_dirty.begin() + end_addr, true);
			}
		}
		break;
		case RDP::Op::SetColorImage:
			rdp_device.frame_buffer_info.framebuffer_address = (w2 & 0x00FFFFFF);
			rdp_device.frame_buffer_info.framebuffer_pixel_size = (w1 >> 19) & 0x3;
			rdp_device.frame_buffer_info.framebuffer_width = (w1 & 0x3FF) + 1;
			break;
		case RDP::Op::SetMaskImage:
			rdp_device.frame_buffer_info.depthbuffer_address = (w2 & 0x00FFFFFF);
			break;
		case RDP::Op::SetTextureImage:
			rdp_device.frame_buffer_info.texture_address = (w2 & 0x00FFFFFF);
			rdp_device.frame_buffer_info.texture_pixel_size = (w1 >> 19) & 0x3;
			rdp_device.frame_buffer_info.texture_width = (w1 & 0x3FF) + 1;
			break;
		case RDP::Op::SetScissor:
		{
			uint32_t upper_left_x = ((w1 >> 12) & 0xFFF) >> 2;
			uint32_t upper_left_y = (w1 & 0xFFF) >> 2;
			uint32_t lower_right_x = ((w2 >> 12) & 0xFFF) >> 2;
			uint32_t lower_right_y = (w2 & 0xFFF) >> 2;
			if (lower_right_x > upper_left_x && lower_right_y > upper_left_y)
			{
				rdp_device.region = (lower_right_x - upper_left_x) * (lower_right_y - upper_left_y);
			}
			else
			{
				rdp_device.region = 0;
			}

			rdp_device.frame_buffer_info.framebuffer_y_offset = upper_left_y;
			rdp_device.frame_buffer_info.framebuffer_height = lower_right_y - upper_left_y;
		}
		break;
		case RDP::Op::SetOtherModes:
		{
			uint8_t cycle_type = (w1 >> 20) & 3;
			uint8_t depth_read_write = (w2 >> 4) & 3;
			rdp_device.frame_buffer_info.depth_buffer_enabled = ((cycle_type & 2) == 0) && (depth_read_write != 0);
		}
		break;
		case RDP::Op::SyncFull:
			sync_signal = processor->signal_timeline();

			interrupt_timer = rdp_device.region;
			if (interrupt_timer == 0)
				interrupt_timer = 5000;
			break;
		default:
			break;
		}

		rdp_device.cmd_cur += cmd_length;
	}

	rdp_device.cmd_ptr = 0;
	rdp_device.cmd_cur = 0;
	*gfx_info.DPC_CURRENT_REG = *gfx_info.DPC_END_REG;

	return interrupt_timer;
}
