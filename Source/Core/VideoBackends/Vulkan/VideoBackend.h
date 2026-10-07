// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Common.h"
#include "VideoCommon/VideoBackendBase.h"

// Vulkan's surface handle, without pulling Vulkan's headers into every file that lists the
// video backends (this is how vulkan_core.h defines it on 64-bit targets).
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
static_assert(sizeof(void*) == 8, "VkSurfaceKHR is a pointer only on 64-bit targets");

namespace Vulkan
{
class VideoBackend : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetName() const override { return NAME; }
  std::string GetDisplayName() const override { return _trans("Vulkan"); }
  void InitBackendInfo(const WindowSystemInfo& wsi) override;
  void PrepareWindow(WindowSystemInfo& wsi) override;

private:
  bool InitializeObjects(const WindowSystemInfo& wsi, VkSurfaceKHR surface);

  static constexpr const char* NAME = "Vulkan";
};
}  // namespace Vulkan
