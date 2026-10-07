// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Vulkan/VKHost.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "Common/Logging/Log.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan::Host
{
namespace
{
std::mutex s_mutex;
Interface s_interface;
std::unique_ptr<VulkanContext> s_pending;
std::atomic<bool> s_active{false};

// A device that was created but never handed to the frontend: its allocator first (the
// context's destructor), then the device itself.
void DestroyUnused(std::unique_ptr<VulkanContext> context)
{
  const VkDevice device = context->GetDevice();
  context.reset();
  vkDestroyDevice(device, nullptr);
}
}  // namespace

bool CreateDevice(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                  const char* const* required_extensions, u32 required_extension_count,
                  VkDevice* device, VkQueue* queue, u32* queue_family)
{
  if (!LoadVulkanLibrary())
  {
    ERROR_LOG_FMT(VIDEO, "Host: could not load the Vulkan library");
    return false;
  }
  if (!LoadVulkanInstanceFunctions(instance))
  {
    ERROR_LOG_FMT(VIDEO, "Host: could not load Vulkan instance functions");
    return false;
  }
  // The frontend's instance is created for Vulkan 1.1.
  std::unique_ptr<VulkanContext> context =
      VulkanContext::CreateForHost(instance, gpu, surface, VK_API_VERSION_1_1);
  if (!context)
  {
    ERROR_LOG_FMT(VIDEO, "Host: could not create the Vulkan device");
    return false;
  }
  // Dolphin enables what it needs itself; with a surface that includes VK_KHR_swapchain,
  // which is all the frontend asks for. Refuse anything else rather than ignore it.
  for (u32 i = 0; i < required_extension_count; ++i)
  {
    if (!context->SupportsDeviceExtension(required_extensions[i]))
    {
      ERROR_LOG_FMT(VIDEO, "Host: the frontend needs {}, which Dolphin did not enable",
                    required_extensions[i]);
      DestroyUnused(std::move(context));
      return false;
    }
  }
  if (context->GetGraphicsQueue() != context->GetPresentQueue() && surface != VK_NULL_HANDLE)
  {
    // The frontend draws and presents on one queue.
    ERROR_LOG_FMT(VIDEO, "Host: the GPU cannot draw and present on the same queue");
    DestroyUnused(std::move(context));
    return false;
  }
  *device = context->GetDevice();
  *queue = context->GetGraphicsQueue();
  *queue_family = context->GetGraphicsQueueFamilyIndex();

  std::lock_guard lock(s_mutex);
  s_pending = std::move(context);
  s_active.store(true);
  return true;
}

void SetInterface(const Interface& iface)
{
  std::lock_guard lock(s_mutex);
  s_interface = iface;
}

void Reset()
{
  std::lock_guard lock(s_mutex);
  s_pending.reset();
  s_interface = {};
  s_active.store(false);
}

bool Active()
{
  return s_active.load();
}

std::unique_ptr<VulkanContext> TakeContext()
{
  std::lock_guard lock(s_mutex);
  return std::move(s_pending);
}

void LockQueue()
{
  if (s_active.load() && s_interface.lock_queue)
    s_interface.lock_queue(s_interface.handle);
}

void UnlockQueue()
{
  if (s_active.load() && s_interface.unlock_queue)
    s_interface.unlock_queue(s_interface.handle);
}

void FrameReady(VkImage image, u32 width, u32 height, float aspect)
{
  if (s_interface.frame_ready)
    s_interface.frame_ready(s_interface.handle, image, width, height, aspect);
}
}  // namespace Vulkan::Host
