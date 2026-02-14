/*
 * Patched WSI platform for tg5050
 *
 * All Vulkan surface creation is bypassed. The Mali-G57 driver's
 * VK_KHR_display implementation segfaults, and VK_EXT_headless_surface
 * can't create a usable swapchain. We use Vulkan only for compute
 * (parallel-rdp), and display via DRM dumb buffers + hw plane scaling.
 */

#include "wsi_platform.hpp"
#include <SDL3/SDL_vulkan.h>

VkSurfaceKHR SDL_WSIPlatform::create_surface(VkInstance instance, VkPhysicalDevice gpu)
{
	// Return null -- we don't use Vulkan for presentation.
	// init_context_from_platform passes null surface to context->init_device(),
	// which is fine for compute-only usage.
	return VK_NULL_HANDLE;
}

void SDL_WSIPlatform::destroy_surface(VkInstance instance, VkSurfaceKHR surface)
{
	// Nothing to destroy
}

std::vector<const char *> SDL_WSIPlatform::get_instance_extensions()
{
	// No WSI extensions needed -- we don't create a Vulkan surface.
	// Requesting VK_KHR_surface / VK_KHR_display would be harmless for
	// instance creation, but we avoid it since we don't need it.
	return {};
}

std::vector<const char *> SDL_WSIPlatform::get_device_extensions()
{
	// Don't request VK_KHR_swapchain -- we have no surface/swapchain.
	return {};
}

uint32_t SDL_WSIPlatform::get_surface_width()
{
	// Return N64 native-ish resolution. This is used by the WSI for
	// swapchain dimensions but we don't create a swapchain. The actual
	// source resolution comes from parallel-rdp's scanout image.
	return 640;
}

uint32_t SDL_WSIPlatform::get_surface_height()
{
	return 480;
}

bool SDL_WSIPlatform::alive(Vulkan::WSI &wsi)
{
	return true;
}

void SDL_WSIPlatform::poll_input()
{
}

void SDL_WSIPlatform::poll_input_async(Granite::InputTrackerHandler *handler)
{
}

void SDL_WSIPlatform::set_window(SDL_Window *_window)
{
	window = _window;
}

void SDL_WSIPlatform::do_resize()
{
	resize = true;
}
