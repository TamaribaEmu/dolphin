// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin as a libretro core, for Tamariba: a frontend that owns the window, draws every
// core into one compositor and runs each game on its own thread. Built as dolphin_libretro,
// next to DolphinShield's C interface (Main.cpp), which the old Android app still uses.
//
// Dolphin keeps its own threads and its own timing. Each retro_run hands the frontend the
// newest picture (waiting up to about a frame for one), mixes the audio that came due
// since the last call, passes on the controller, and runs Dolphin's host jobs.
//
// Pictures are rendered on the frontend's Vulkan device and never leave the GPU
// (VideoBackends/Vulkan/VKHost.h): the frontend's context negotiation calls create_device,
// Dolphin creates the device on the frontend's instance for its window, and the game boots
// in context_reset, once the frontend has moved onto that device.
//
// Additions to plain libretro: retro_multigame_set_paused(bool). Dolphin would keep
// running between retro_run calls, so the frontend pauses it while its menu is open. And
// Dolphin's own NetPlay for Tamariba's online rooms (dolphin_netplay_*, TamaribaNetPlay.h):
// asked for before rendering starts, it boots the game itself once every player is in.

// Vulkan types only (VKHost.h): every Vulkan call here goes through Dolphin's own loader.
#define VK_NO_PROTOTYPES
#define DOLPHIN_VKHOST_TYPES_ONLY

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libretro.h>
#include <libretro_vulkan.h>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#ifdef ANDROID
#include "Common/AndroidAnalytics.h"
#endif
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/IOS/Network/TamaribaNet.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/TamaribaWFC.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/InputBackend.h"
#include "UICommon/UICommon.h"
#include "VideoBackends/Vulkan/VKHost.h"
#include "VideoCommon/VideoConfig.h"

#include "DolphinShield/TamaribaNetPlay.h"

namespace
{
using Clock = std::chrono::steady_clock;

retro_environment_t s_environment;
retro_video_refresh_t s_video;
retro_audio_sample_batch_t s_audio;
retro_input_poll_t s_input_poll;
retro_input_state_t s_input_state;

// Each player's input (retro_run) and whether they have a controller. The pads, the
// configuration and retro_set_controller_port_device below use them.
constexpr int kPorts = 4;
std::atomic<uint32_t> s_buttons[kPorts] = {};
std::atomic<int16_t> s_axes[kPorts][4] = {};  // left x, left y, right x, right y; down positive
// Which players have a controller (retro_set_controller_port_device). Player 1's pad is always
// plugged in; the others follow the frontend's controllers, so a game sees as many pads or
// Wii Remotes as there are players, and a controller connected mid-game joins like a real one.
std::atomic<bool> s_plugged[kPorts] = {true, false, false, false};
retro_log_printf_t s_log;

void Log(retro_log_level level, const char* format, ...)
{
  char text[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (s_log)
    s_log(level, "%s\n", text);
  else
    std::fprintf(stderr, "Dolphin: %s\n", text);
}

// Something the person playing should see: on the frontend's loading card, and in its log.
void Tell(const std::string& text)
{
  Log(RETRO_LOG_INFO, "%s", text.c_str());
  retro_message message{text.c_str(), 240};
  s_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

// --- Options -----------------------------------------------------------------------------
// The keys and defaults of the old app's DolphinShield interface (Main.cpp), so settings
// carry over. Asked for with GET_VARIABLE; unset keys keep these defaults.

std::string Option(const char* key, const char* fallback)
{
  retro_variable variable{key, nullptr};
  if (s_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value)
    return variable.value;
  return fallback;
}

bool Enabled(const char* key, const char* fallback)
{
  const std::string value = Option(key, fallback);
  return value == "enabled" || value == "1" || value == "true" || value == "on";
}

int Number(const char* key, int fallback, int low, int high)
{
  const std::string value = Option(key, "");
  int result = fallback;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    result = fallback;
  return std::clamp(result, low, high);
}

void ApplyConfiguration()
{
  const int performance = Number("dolphin_shield_performance_mode", 2, 0, 2);
  const int cpu_clock = Number("dolphin_cpu_clock_percentage", 100, 5, 300);

  Config::ConfigChangeCallbackGuard guard;
  Config::SetBase(Config::MAIN_ANALYTICS_ENABLED, false);
  Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));
  // Audio goes to the frontend: retro_run pulls it from the mixer, nothing else plays it.
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
  Config::SetBase(Config::MAIN_CPU_THREAD, Enabled("dolphin_main_cpu_thread", "enabled"));
  Config::SetBase(Config::MAIN_FAST_DISC_SPEED, true);
  Config::SetBase(Config::MAIN_FASTMEM, true);
  Config::SetBase(Config::MAIN_FASTMEM_ARENA, true);
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_DSP_JIT, true);
  // A GameCube controller and a Wii Remote for each player with a controller (see s_plugged).
  for (int port = 0; port < kPorts; ++port)
  {
    Config::SetBase(Config::GetInfoForSIDevice(port), s_plugged[port] ? SerialInterface::SIDEVICE_GC_CONTROLLER :
                                                                        SerialInterface::SIDEVICE_NONE);
    Config::SetBase(Config::GetInfoForWiimoteSource(port), s_plugged[port] ? WiimoteSource::Emulated : WiimoteSource::None);
  }
  Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, cpu_clock != 100);
  Config::SetBase(Config::MAIN_OVERCLOCK, cpu_clock / 100.0f);
  Config::SetBase(Config::MAIN_ENABLE_CHEATS, true);
  Config::SetBase(Config::SYSCONF_WIDESCREEN, Enabled("dolphin_widescreen", "enabled"));
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, Enabled("dolphin_widescreen", "enabled"));
  Config::SetBase(Config::GFX_ASPECT_RATIO,
                  static_cast<AspectMode>(Number("dolphin_aspect_ratio", 0, 0, 3)));
  Config::SetBase(Config::GFX_SHOW_FPS, false);
  Config::SetBase(Config::GFX_SHOW_SPEED, false);
  Config::SetBase(Config::GFX_EFB_SCALE, Number("dolphin_efb_scale", 1, 1, 8));
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE,
                  Enabled("dolphin_efb_access_enable", "disabled"));
  Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                  Enabled("dolphin_efb_to_texture", "enabled"));
  Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                  Enabled("dolphin_xfb_to_texture_enable", "enabled"));
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  static_cast<ShaderCompilationMode>(
                      Number("dolphin_shader_compilation_mode", 3, 0, 3)));
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                  Enabled("dolphin_wait_for_shaders", "enabled"));
  Config::SetBase(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                  Number("dolphin_texture_cache_accuracy", 128, 0, 128));
  Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                  Enabled("dolphin_gpu_texture_decoding", "disabled"));
  Config::SetBase(Config::GFX_FAST_DEPTH_CALC, Enabled("dolphin_fast_depth_calculation", "enabled"));
  Config::SetBase(Config::GFX_DISABLE_FOG, Enabled("dolphin_disable_fog", "disabled"));
  Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
                  Enabled("dolphin_disable_copy_filter", "enabled"));
  Config::SetBase(Config::GFX_HACK_VI_SKIP,
                  Enabled("dolphin_vi_skip", performance == 2 ? "enabled" : "disabled"));
  // Tamariba's Settings › Online services: the Wi-Fi Connection service for Wii games
  // (Core/TamaribaWFC.h); their traffic goes through the Plaza's exit (retro_tamariba_set_inet).
  TamaribaWFC::SetService(TamaribaWFC::ParseService(Option("dolphin_wfc", "off")));
}

// --- Controller ------------------------------------------------------------------------
// Each player's RetroPad as a device of Dolphin's own input system ("Tamariba/0/RetroPad" for
// player 1 ... "Tamariba/3/RetroPad"), mapped onto that player's GameCube controller and Wii
// Remote by the ini files below.


class RetroPad final : public ciface::Core::Device
{
public:
  explicit RetroPad(int port) : m_port(port)
  {
    static constexpr std::pair<const char*, unsigned> buttons[] = {
        {"A", RETRO_DEVICE_ID_JOYPAD_A},         {"B", RETRO_DEVICE_ID_JOYPAD_B},
        {"X", RETRO_DEVICE_ID_JOYPAD_X},         {"Y", RETRO_DEVICE_ID_JOYPAD_Y},
        {"L", RETRO_DEVICE_ID_JOYPAD_L},         {"R", RETRO_DEVICE_ID_JOYPAD_R},
        {"ZL", RETRO_DEVICE_ID_JOYPAD_L2},       {"ZR", RETRO_DEVICE_ID_JOYPAD_R2},
        {"L3", RETRO_DEVICE_ID_JOYPAD_L3},       {"R3", RETRO_DEVICE_ID_JOYPAD_R3},
        {"Start", RETRO_DEVICE_ID_JOYPAD_START}, {"Select", RETRO_DEVICE_ID_JOYPAD_SELECT},
        {"Up", RETRO_DEVICE_ID_JOYPAD_UP},       {"Down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"Left", RETRO_DEVICE_ID_JOYPAD_LEFT},   {"Right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
    };
    for (const auto& [name, id] : buttons)
      AddInput(new Button(name, id, port));
    static constexpr const char* axes[] = {"Left X", "Left Y", "Right X", "Right Y"};
    for (int i = 0; i < 4; ++i)
      AddAnalogInputs(new Axis(std::string(axes[i]) + "-", port, i, -1),
                      new Axis(std::string(axes[i]) + "+", port, i, 1));
  }
  std::string GetName() const override { return "RetroPad"; }
  std::string GetSource() const override { return "Tamariba"; }
  bool IsVirtualDevice() const override { return true; }
  int GetSortPriority() const override { return DEFAULT_DEVICE_SORT_PRIORITY; }
  // Player N's pad is always "Tamariba/N-1/RetroPad", whatever order devices come back in.
  std::optional<int> GetPreferredId() const override { return m_port; }

private:
  class Button final : public Input
  {
  public:
    Button(const char* name, unsigned id, int port) : m_name(name), m_bit(1u << id), m_port(port) {}
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override { return (s_buttons[m_port].load() & m_bit) ? 1.0 : 0.0; }

  private:
    const char* m_name;
    uint32_t m_bit;
    int m_port;
  };
  class Axis final : public Input
  {
  public:
    Axis(std::string name, int port, int index, int sign)
        : m_name(std::move(name)), m_port(port), m_index(index), m_sign(sign)
    {
    }
    std::string GetName() const override { return m_name; }
    ControlState GetState() const override
    {
      return std::max(0.0, m_sign * s_axes[m_port][m_index].load() / 32767.0);
    }

  private:
    std::string m_name;
    int m_port;
    int m_index;
    int m_sign;
  };

  int m_port;
};

// The printed labels as on a GameCube pad: A, B, X, Y as printed; R (and Select) is Z; ZL and
// ZR are the analog triggers, L doubles ZL. The right stick is the C-Stick and, on the Wii
// Remote, the pointer. (The old app meant Select and L to reach Z and L too, but its
// touch-device mapping overwrote them.)
void WriteControllerConfigs()
{
  const std::string config_dir = File::GetUserPath(D_CONFIG_IDX);
  File::CreateFullPath(config_dir);
  // The same mapping for every player; each reads their own RetroPad.
  static constexpr const char* gc_mapping = R"ini(Buttons/A = `A`
Buttons/B = `B`
Buttons/X = `X`
Buttons/Y = `Y`
Buttons/Z = `R` | `Select`
Buttons/Start = `Start`
D-Pad/Up = `Up`
D-Pad/Down = `Down`
D-Pad/Left = `Left`
D-Pad/Right = `Right`
Main Stick/Up = `Left Y-`
Main Stick/Down = `Left Y+`
Main Stick/Left = `Left X-`
Main Stick/Right = `Left X+`
C-Stick/Up = `Right Y-`
C-Stick/Down = `Right Y+`
C-Stick/Left = `Right X-`
C-Stick/Right = `Right X+`
Triggers/L = `ZL` | `L`
Triggers/R = `ZR`
Triggers/L-Analog = `ZL` | `L`
Triggers/R-Analog = `ZR`
)ini";
  std::ofstream gc(config_dir + GCPAD_CONFIG, std::ios::trunc);
  for (int port = 0; port < kPorts; ++port)
    gc << "[GCPad" << port + 1 << "]\nDevice = Tamariba/" << port << "/RetroPad\n" << gc_mapping;

  const std::string mode = Option("dolphin_wii_input", "remote");
  const char* extension = mode == "classic" ? "Classic" : mode == "nunchuk" ? "Nunchuk" : "None";
  static constexpr const char* wii_mapping = R"ini(Buttons/A = `A`
Buttons/B = `B`
Buttons/1 = `X`
Buttons/2 = `Y`
Buttons/- = `Select`
Buttons/+ = `Start`
D-Pad/Up = `Up`
D-Pad/Down = `Down`
D-Pad/Left = `Left`
D-Pad/Right = `Right`
IR/Up = `Right Y-`
IR/Down = `Right Y+`
IR/Left = `Right X-`
IR/Right = `Right X+`
IR/Total Pitch = 20
IR/Total Yaw = 25
IR/Vertical Offset = 10
Nunchuk/Buttons/C = `L`
Nunchuk/Buttons/Z = `ZL`
Nunchuk/Stick/Up = `Left Y-`
Nunchuk/Stick/Down = `Left Y+`
Nunchuk/Stick/Left = `Left X-`
Nunchuk/Stick/Right = `Left X+`
Classic/Buttons/A = `A`
Classic/Buttons/B = `B`
Classic/Buttons/X = `X`
Classic/Buttons/Y = `Y`
Classic/Buttons/- = `Select`
Classic/Buttons/+ = `Start`
Classic/Buttons/ZL = `ZL`
Classic/Buttons/ZR = `ZR`
Classic/D-Pad/Up = `Up`
Classic/D-Pad/Down = `Down`
Classic/D-Pad/Left = `Left`
Classic/D-Pad/Right = `Right`
Classic/Left Stick/Up = `Left Y-`
Classic/Left Stick/Down = `Left Y+`
Classic/Left Stick/Left = `Left X-`
Classic/Left Stick/Right = `Left X+`
Classic/Right Stick/Up = `Right Y-`
Classic/Right Stick/Down = `Right Y+`
Classic/Right Stick/Left = `Right X-`
Classic/Right Stick/Right = `Right X+`
Classic/Triggers/L = `L`
Classic/Triggers/R = `R`
)ini";
  std::ofstream wii(config_dir + WIIPAD_CONFIG, std::ios::trunc);
  for (int port = 0; port < kPorts; ++port)
  {
    // Source 1 is an emulated Wii Remote, 0 none: only players with a controller have one.
    wii << "[Wiimote" << port + 1 << "]\nDevice = Tamariba/" << port << "/RetroPad\n" << wii_mapping
        << "Source = " << (s_plugged[port] ? 1 : 0) << "\nExtension = " << extension << '\n';
  }
}

// Dolphin clears and repopulates its devices on every refresh (one happens while booting),
// so the pad comes from an input backend, repopulated with the built-in ones.
class RetroPadBackend final : public ciface::InputBackend
{
public:
  using InputBackend::InputBackend;
  void PopulateDevices() override
  {
    for (int port = 0; port < kPorts; ++port)
      GetControllerInterface().AddDevice(std::make_shared<RetroPad>(port));
  }
};

// --- Pictures ------------------------------------------------------------------------------
// Filled on Dolphin's submission thread (VKHost's frame_ready), taken by retro_run.

std::mutex s_frame_mutex;
std::condition_variable s_frame_ready;
struct Picture
{
  VkImage image = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  float aspect = 4.0f / 3.0f;
  uint64_t serial = 0;
};
Picture s_picture;
uint64_t s_shown_serial = 0;
bool s_wake = false;  // Host_Message: host jobs to run

const retro_hw_render_interface_vulkan* s_vulkan = nullptr;
retro_vulkan_image s_vulkan_image{};
float s_aspect = 0.0f;
uint32_t s_width = 0, s_height = 0;

void FrameReady(void*, VkImage image, uint32_t width, uint32_t height, float aspect)
{
  {
    std::lock_guard lock(s_frame_mutex);
    s_picture.image = image;
    s_picture.width = width;
    s_picture.height = height;
    s_picture.aspect = aspect > 0.0f ? aspect : 4.0f / 3.0f;
    ++s_picture.serial;
  }
  s_frame_ready.notify_one();
}

// --- Game ----------------------------------------------------------------------------------

std::string s_rom_path;
std::string s_error;
std::atomic<bool> s_booted{false};
// Rendering is ready (context_reset), so a game may boot: at once, or when NetPlay starts it.
bool s_context_ready = false;
bool s_controllers = false;  // UICommon::InitControllers ran
bool s_boot_failed = false;  // it won't boot: retro_run stops trying
bool s_game_over = false;    // the online game ended: it does not start again
// Tests only (dolphin_tamariba_headless): no rendering at all (Dolphin's Null video backend),
// so a test can run two cores in two processes without a GPU (Tamariba's NetPlay test).
bool s_headless = false;
Clock::time_point s_audio_clock;
double s_audio_due = 0.0;
constexpr unsigned kSampleRate = 48000;

bool AlertHandler(const char* caption, const char* text, bool, Common::MsgType)
{
  Log(RETRO_LOG_ERROR, "%s: %s", caption, text);
  if (s_error.empty())
    s_error = std::string(caption) + ": " + text;
  return true;
}

bool CreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                  VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr, const char** extensions,
                  unsigned extension_count, const char**, unsigned, const VkPhysicalDeviceFeatures*)
{
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  u32 family = 0;
  if (!Vulkan::Host::CreateDevice(instance, gpu, surface, extensions, extension_count, &device,
                                  &queue, &family))
    return false;
  context->gpu = gpu;
  context->device = device;
  context->queue = queue;
  context->queue_family_index = family;
  context->presentation_queue = queue;
  context->presentation_queue_family_index = family;
  return true;
}

const retro_hw_render_context_negotiation_interface_vulkan s_negotiation = {
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
    nullptr,  // get_application_info
    CreateDevice,
    nullptr,  // destroy_device: the frontend owns the device
};

void InitControllers()
{
  if (s_controllers)
    return;
  const WindowSystemInfo wsi(WindowSystemType::Headless, nullptr, nullptr, nullptr);
  UICommon::InitControllers(wsi);
  g_controller_interface.AddInputBackend(std::make_unique<RetroPadBackend>(&g_controller_interface));
  s_controllers = true;
}

// Boots the game (with NetPlay's session data when NetPlay starts it).
bool Boot(const std::string& path, BootSessionData session)
{
  const WindowSystemInfo wsi(WindowSystemType::Headless, nullptr, nullptr, nullptr);
  auto boot = BootParameters::GenerateFromFile(path, std::move(session));
  if (!boot || !BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    Tell(s_error.empty() ? "Dolphin could not boot this disc" : s_error);
    s_boot_failed = true;
    return false;
  }
  s_audio_clock = Clock::now();
  s_audio_due = 0.0;
  s_booted = true;
  return true;
}

// The frontend is on the device now: start the game (unless NetPlay will).
void ContextReset()
{
  if (!s_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &s_vulkan) || !s_vulkan ||
      s_vulkan->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
  {
    Tell("Dolphin could not get the frontend's Vulkan interface");
    return;
  }
  Vulkan::Host::Interface host;
  host.handle = s_vulkan->handle;
  host.lock_queue = s_vulkan->lock_queue;
  host.unlock_queue = s_vulkan->unlock_queue;
  host.frame_ready = FrameReady;
  Vulkan::Host::SetInterface(host);

  InitControllers();
  s_context_ready = true;
  if (!TamaribaNetPlay::Wanted())
    Boot(s_rom_path, BootSessionData());
}

void Stop()
{
  if (!s_booted.exchange(false))
    return;
  Core::System& system = Core::System::GetInstance();
  TamaribaNetPlay::BeforeStop();  // nobody waits for another player's buttons any more
  Core::Stop(system);
  // Stopping needs host jobs run on this thread (and pictures may still be on their way).
  while (Core::IsRunningOrStarting(system))
  {
    Core::HostDispatchJobs(system);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Core::Shutdown(system);
}

void ShutdownControllers()
{
  if (s_controllers)
    UICommon::ShutdownControllers();
  s_controllers = false;
}

void ContextDestroy()
{
  Stop();  // the frontend is leaving the device; nothing of ours may use it any more
  ShutdownControllers();
  s_context_ready = false;
  Vulkan::Host::Reset();
  s_vulkan = nullptr;
}

// Audio that came due since the last call, from Dolphin's mixer.
void MixAudio()
{
  SoundStream* stream = Core::System::GetInstance().GetSoundStream();
  if (!stream || !stream->GetMixer())
    return;
  const Clock::time_point now = Clock::now();
  s_audio_due += std::chrono::duration<double>(now - s_audio_clock).count() * kSampleRate;
  s_audio_clock = now;
  s_audio_due = std::min(s_audio_due, kSampleRate * 0.1);  // after a stall, not a flood
  const unsigned frames = static_cast<unsigned>(s_audio_due);
  if (frames == 0)
    return;
  s_audio_due -= frames;
  static std::vector<int16_t> samples;
  samples.resize(frames * 2);
  stream->GetMixer()->Mix(samples.data(), frames);
  s_audio(samples.data(), frames);
}

std::vector<u8> s_state;  // the last save state, between retro_serialize_size and retro_serialize
}  // namespace

// --- Host interface (what Dolphin's core expects of a frontend) ------------------------------

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}
void Host_PPCSymbolsChanged() {}
void Host_RefreshDSPDebuggerWindow() {}
bool Host_UIBlocksControllerState()
{
  return false;
}
void Host_Message(HostMessageID)
{
  // Usually "host jobs are waiting": wake retro_run, which runs them.
  {
    std::lock_guard lock(s_frame_mutex);
    s_wake = true;
  }
  s_frame_ready.notify_one();
}
void Host_UpdateTitle(const std::string&) {}
void Host_UpdateDisasmDialog() {}
void Host_UpdateMainFrame() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus()
{
  return true;
}
bool Host_RendererHasFullFocus()
{
  return true;
}
bool Host_RendererIsFullscreen()
{
  return true;
}
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   int64_t, int64_t, int, int)
{
  return false;
}
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

// --- libretro ------------------------------------------------------------------------------

RETRO_API unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t callback)
{
  s_environment = callback;
  retro_log_callback log{};
  if (s_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    s_log = log.log;
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t callback)
{
  s_video = callback;
}
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback)
{
  s_audio = callback;
}
RETRO_API void retro_set_input_poll(retro_input_poll_t callback)
{
  s_input_poll = callback;
}
RETRO_API void retro_set_input_state(retro_input_state_t callback)
{
  s_input_state = callback;
}

RETRO_API void retro_get_system_info(retro_system_info* info)
{
  *info = {};
  info->library_name = "Dolphin";
  info->library_version = "MMJR2 Tamariba";
  info->valid_extensions = "iso|gcm|ciso|gcz|wbfs|wia|rvz|tgc|dol|elf";
  info->need_fullpath = true;
  info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info)
{
  *info = {};
  info->geometry = {640, 528, 1920, 1584, 4.0f / 3.0f};
  info->timing.fps = 60.0;  // Dolphin keeps its own time (NTSC or PAL); this is nominal
  info->timing.sample_rate = kSampleRate;
}

RETRO_API void retro_init() {}
RETRO_API void retro_deinit() {}
// Tamariba says which players have a controller: before the game starts (the pads are then
// plugged in at boot) and whenever a controller comes or goes. Player 1's stays plugged in.
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
  if (port == 0 || port >= kPorts)
    return;
  // Online, NetPlay decides who plays which controller (each device plays with its first).
  if (TamaribaNetPlay::Wanted())
    return;
  const bool plugged = (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
  if (s_plugged[port].exchange(plugged) == plugged)
    return;
  const auto pad = plugged ? SerialInterface::SIDEVICE_GC_CONTROLLER : SerialInterface::SIDEVICE_NONE;
  const auto remote = plugged ? WiimoteSource::Emulated : WiimoteSource::None;
  Log(RETRO_LOG_INFO, "Player %u's controller %s", port + 1, plugged ? "plugged in" : "unplugged");
  if (!s_booted)
  {
    Config::SetBase(Config::GetInfoForSIDevice(port), pad);
    Config::SetBase(Config::GetInfoForWiimoteSource(port), remote);
    return;
  }
  // While running, as Dolphin's own controller settings do: the serial interface swaps the
  // GameCube device at its next poll, and the config change connects or drops the Wii Remote.
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(port), pad);
  Core::System::GetInstance().GetSerialInterface().ChangeDevice(pad, static_cast<int>(port));
  Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(port), remote);
}

RETRO_API bool retro_load_game(const retro_game_info* game)
{
  if (!game || !game->path)
    return false;
  s_rom_path = game->path;
  s_error.clear();
  Common::RegisterMsgAlertHandler(AlertHandler);
#ifdef ANDROID
  // Android builds call these while describing the device (even with analytics off, which
  // it is below); an unset one aborts the app. Same answers as Main.cpp gives.
  Common::AndroidSetReportHandler([](std::string, std::string) {});
  DolphinAnalytics::AndroidSetGetValFunc([](std::string key) {
    if (key == "DEVICE_MANUFACTURER")
      return std::string("NVIDIA");
    if (key == "DEVICE_MODEL")
      return std::string("SHIELD Android TV");
    if (key == "DEVICE_OS")
      return std::string("Android (Tamariba)");
    if (key == "DEVICE_TYPE")
      return std::string("tamariba");
    return std::string();
  });
#endif
  // retro_run, the pause hook and unloading all come from this thread.
  Core::DeclareAsHostThread();

  // Dolphin's data (Sys) is installed by the frontend under its system folder; the user
  // folder (memory cards, the Wii NAND, caches, settings) is under the save folder, shared
  // by every GameCube and Wii game, as on the real consoles.
  const char* system_dir = nullptr;
  const char* save_dir = nullptr;
  if (!s_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir ||
      !s_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir)
  {
    Log(RETRO_LOG_ERROR, "The frontend gave no system or save folder");
    return false;
  }
  const std::string sys = std::string(system_dir) + "/dolphin/Sys";
  if (!File::IsDirectory(sys + "/GameSettings"))
  {
    Tell("Dolphin's data is missing from " + sys);
    return false;
  }
  File::SetSysDirectory(sys);
  // Only player 1 until the frontend says otherwise (retro_set_controller_port_device).
  for (int port = 0; port < kPorts; ++port)
    s_plugged[port] = port == 0;
  UICommon::SetUserDirectory(std::string(save_dir) + "/User");
  UICommon::CreateDirectories();
  WriteControllerConfigs();
  UICommon::Init();
  ApplyConfiguration();
  // The Wii's network is the Plaza's exit (retro_tamariba_set_inet) or none: never the host's.
  IOS::HLE::TamaribaNet::SetEnabled(true);
  IOS::HLE::TamaribaNet::SetInterface(nullptr);
  TamaribaNetPlay::Prepare(s_rom_path, Boot, Tell);
  s_boot_failed = false;
  s_game_over = false;

  retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
  s_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
  s_headless = Enabled("dolphin_tamariba_headless", "disabled");
  if (s_headless)
  {
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
    InitControllers();
    s_context_ready = true;  // the game boots in retro_run
    return true;
  }
  retro_hw_render_callback hw{};
  hw.context_type = RETRO_HW_CONTEXT_VULKAN;
  hw.version_major = VK_API_VERSION_1_1;
  hw.context_reset = ContextReset;
  hw.context_destroy = ContextDestroy;
  if (!s_environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw))
  {
    Log(RETRO_LOG_ERROR, "The frontend cannot render with Vulkan");
    UICommon::Shutdown();
    return false;
  }
  s_environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
                const_cast<retro_hw_render_context_negotiation_interface_vulkan*>(&s_negotiation));
  return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t)
{
  return false;
}

RETRO_API void retro_unload_game()
{
  Stop();
  TamaribaNetPlay::Shutdown();
  if (s_headless)
    ShutdownControllers();
  s_context_ready = false;
  UICommon::Shutdown();
  Core::UndeclareAsHostThread();
}

RETRO_API void retro_run()
{
  Core::System& system = Core::System::GetInstance();
  if (!s_booted)
  {
    // Waiting for rendering, or for NetPlay to start the game (or a boot that failed).
    bool booted = false;
    if (s_context_ready && !s_boot_failed && !s_game_over)
    {
      switch (TamaribaNetPlay::PumpBeforeBoot())
      {
      case TamaribaNetPlay::Before::Wait:
        break;
      case TamaribaNetPlay::Before::Booted:
        booted = true;
        break;
      case TamaribaNetPlay::Before::Offline:
        booted = Boot(s_rom_path, BootSessionData());
        break;
      }
    }
    if (!booted)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      s_video(nullptr, s_width, s_height, 0);
      return;
    }
  }
  if (TamaribaNetPlay::PumpRunning())
  {
    Tell("The online game has ended");
    Stop();
    s_game_over = true;
    s_video(nullptr, s_width, s_height, 0);
    return;
  }
  Core::HostDispatchJobs(system);

  s_input_poll();
  for (unsigned port = 0; port < kPorts; ++port)
  {
    uint32_t buttons = 0;
    for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
    {
      if (s_input_state(port, RETRO_DEVICE_JOYPAD, 0, id))
        buttons |= 1u << id;
    }
    s_buttons[port] = buttons;
    for (unsigned stick = 0; stick < 2; ++stick)
    {
      for (unsigned axis = 0; axis < 2; ++axis)
        s_axes[port][stick * 2 + axis] = s_input_state(port, RETRO_DEVICE_ANALOG, stick, axis);
    }
  }

  // The newest picture, waiting up to about a frame and a half for one.
  Picture picture;
  {
    std::unique_lock lock(s_frame_mutex);
    s_frame_ready.wait_for(lock, std::chrono::milliseconds(25),
                           [] { return s_picture.serial != s_shown_serial || s_wake; });
    s_wake = false;
    picture = s_picture;
  }
  MixAudio();
  if (picture.serial == s_shown_serial || !picture.image || !s_vulkan)
  {
    s_video(nullptr, s_width, s_height, 0);
    return;
  }
  s_shown_serial = picture.serial;
  if (picture.width != s_width || picture.height != s_height || picture.aspect != s_aspect)
  {
    s_width = picture.width;
    s_height = picture.height;
    s_aspect = picture.aspect;
    retro_game_geometry geometry{s_width, s_height, s_width, s_height, s_aspect};
    s_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
  }
  s_vulkan_image = {};
  s_vulkan_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  s_vulkan_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  s_vulkan_image.create_info.image = picture.image;
  s_vulkan_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  s_vulkan_image.create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  s_vulkan_image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  s_vulkan->set_image(s_vulkan->handle, &s_vulkan_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
  s_video(RETRO_HW_FRAME_BUFFER_VALID, picture.width, picture.height, 0);
}

RETRO_API void retro_reset()
{
  if (s_booted && !TamaribaNetPlay::Running())
    Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
}

RETRO_API size_t retro_serialize_size()
{
  if (!s_booted)
    return 0;
  State::SaveToBuffer(Core::System::GetInstance(), s_state);
  return s_state.size();
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
  if (!s_booted || s_state.empty() || size < s_state.size())
    return false;
  std::memcpy(data, s_state.data(), s_state.size());
  s_state.clear();
  return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
  if (!s_booted || !data || size == 0)
    return false;
  std::vector<u8> state(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
  State::LoadFromBuffer(Core::System::GetInstance(), state);
  return true;
}

RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API unsigned retro_get_region()
{
  return RETRO_REGION_NTSC;
}
RETRO_API void* retro_get_memory_data(unsigned)
{
  return nullptr;  // memory cards and the NAND are files in the user folder
}
RETRO_API size_t retro_get_memory_size(unsigned)
{
  return 0;
}

// Tamariba's pause: Dolphin's threads stop until the frontend resumes it.
extern "C" RETRO_API void retro_multigame_set_paused(bool paused)
{
  if (!s_booted)
    return;
  Core::SetState(Core::System::GetInstance(), paused ? Core::State::Paused : Core::State::Running);
  s_audio_clock = Clock::now();  // nothing came due while paused
  s_audio_due = 0.0;
}

// Tamariba's internet exit for the console's own online play (DolphinShield/tamariba_net.h):
// the Wii's sockets, name lookups and SSL go through it (IOS/Network/TamaribaNet.h). NULL: the
// console has no network.
extern "C" RETRO_API void retro_tamariba_set_inet(const tamariba_inet_interface* inet)
{
  IOS::HLE::TamaribaNet::SetInterface(inet);
}
