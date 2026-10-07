// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Rendering for a frontend that owns the window (Tamariba, through its libretro host).
//
// Instead of creating its own instance, surface and swapchain, Dolphin creates its device
// on the frontend's instance and GPU, able to present to the frontend's window, and the
// frontend moves its own rendering onto that device. Both submit to the one queue under
// the frontend's lock. Each finished picture is copied into a small ring of images the
// frontend reads (see VKGfx::PresentToHost), so Dolphin never presents anything itself.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

#ifdef DOLPHIN_VKHOST_TYPES_ONLY
// The frontend's side (DolphinShield/Libretro.cpp) sees only Vulkan's types: it never calls
// Vulkan itself, and the libretro headers it also includes bring Vulkan's prototypes,
// which would clash with Dolphin's loader.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#else
#include "VideoBackends/Vulkan/VulkanLoader.h"
#endif

namespace Vulkan
{
class VulkanContext;

namespace Host
{
struct Interface
{
  void* handle = nullptr;
  void (*lock_queue)(void* handle) = nullptr;
  void (*unlock_queue)(void* handle) = nullptr;
  // A picture is ready: `image` is in SHADER_READ_ONLY_OPTIMAL, and the commands that
  // fill it have been submitted to the queue. Called on the thread that submitted them.
  void (*frame_ready)(void* handle, VkImage image, u32 width, u32 height, float aspect) = nullptr;
};

// For the frontend (libretro's create_device): loads Vulkan, then creates the device on
// `gpu` of the frontend's `instance`, with a queue that can present to `surface` (which
// may be null). The device stays the frontend's: Dolphin never destroys it, or the
// instance. It is kept here until the video backend starts and takes it.
bool CreateDevice(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                  const char* const* required_extensions, u32 required_extension_count,
                  VkDevice* device, VkQueue* queue, u32* queue_family);
void SetInterface(const Interface& iface);
// The frontend is done: forget the interface and any context the backend never took.
void Reset();

// Whether Dolphin is rendering for a frontend (set once CreateDevice succeeded).
bool Active();
// For VideoBackend::Initialize.
std::unique_ptr<VulkanContext> TakeContext();

// Around every queue submission, present and wait-idle while Active().
void LockQueue();
void UnlockQueue();
void FrameReady(VkImage image, u32 width, u32 height, float aspect);
}  // namespace Host
}  // namespace Vulkan
