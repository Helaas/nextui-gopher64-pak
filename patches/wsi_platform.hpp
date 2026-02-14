/*
 * Patched WSI platform for tg5050
 *
 * Returns VK_NULL_HANDLE for surface creation (no Vulkan WSI needed).
 * Returns empty instance extensions (avoids VK_KHR_display which segfaults on Mali-G57).
 * Display output is handled by drm_display.cpp instead.
 */

#pragma once

#include "wsi.hpp"
#include <SDL3/SDL.h>

class SDL_WSIPlatform : public Vulkan::WSIPlatform
{
public:
	VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice gpu) override;
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface) override;
	std::vector<const char *> get_instance_extensions() override;
	std::vector<const char *> get_device_extensions() override;
	uint32_t get_surface_width() override;
	uint32_t get_surface_height() override;
	bool alive(Vulkan::WSI &wsi) override;
	void poll_input() override;
	void poll_input_async(Granite::InputTrackerHandler *handler) override;
	void set_window(SDL_Window *_window);
	void do_resize();

private:
	SDL_Window *window;
};
