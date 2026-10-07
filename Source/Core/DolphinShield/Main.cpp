#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/AndroidAnalytics.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Host.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinShield/ShieldDebugger.h"
#include "InputCommon/ControllerInterface/Touch/ButtonManager.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr const char* TAG = "DolphinShield";

std::mutex s_mutex;
std::map<std::string, std::string> s_options;
std::string s_game_id = "UNKNOWN";
std::string s_profile_name = "Shield Default";
std::string s_input_mode = "gc_pad";
std::string s_error;
ANativeWindow* s_window = nullptr;
std::atomic<bool> s_initialized{false};
std::atomic<bool> s_running{false};
bool s_audio_sync = true;
std::atomic<bool> s_debug_enabled{false};
int s_cpu_clock_percent = 100;
int s_state_callback = -1;
int s_last_frame = -1;

bool IsEnabled(const std::string& value)
{
  return value == "1" || value == "true" || value == "enabled" || value == "yes" ||
         value == "on";
}

int ToInt(const std::string& value, int fallback)
{
  int result = fallback;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : fallback;
}

std::string Option(const char* key, const char* fallback)
{
  const auto it = s_options.find(key);
  return it == s_options.end() ? fallback : it->second;
}

bool AlertHandler(const char* caption, const char* text, bool, Common::MsgType)
{
  __android_log_print(ANDROID_LOG_ERROR, TAG, "%s: %s", caption, text);
  if (s_error.empty())
    s_error = std::string(caption) + ": " + text;
  return true;
}

void WriteControllerConfigs(const std::string& input_mode)
{
  const std::string config_dir = File::GetUserPath(D_CONFIG_IDX);
  File::CreateFullPath(config_dir);
  std::ofstream gc(config_dir + GCPAD_CONFIG, std::ios::trunc);
  gc << R"ini([GCPad1]
Device = Android/0/Touchscreen
Buttons/A = `Button 0`
Buttons/B = `Button 1`
Buttons/Start = `Button 2`
Buttons/X = `Button 3`
Buttons/Y = `Button 4`
Buttons/Z = `Button 5`
D-Pad/Up = `Button 6`
D-Pad/Down = `Button 7`
D-Pad/Left = `Button 8`
D-Pad/Right = `Button 9`
Main Stick/Up = `Axis 11`
Main Stick/Down = `Axis 12`
Main Stick/Left = `Axis 13`
Main Stick/Right = `Axis 14`
C-Stick/Up = `Axis 16`
C-Stick/Down = `Axis 17`
C-Stick/Left = `Axis 18`
C-Stick/Right = `Axis 19`
Triggers/L = `Axis 20`
Triggers/R = `Axis 21`
Triggers/L-Analog = `Axis 22`
Triggers/R-Analog = `Axis 23`
Rumble/Motor = `Rumble 700`
)ini";

  std::string extension = "None";
  if (input_mode == "wiimote_classic")
    extension = "Classic";
  else if (input_mode == "wiimote_nunchuk")
    extension = "Nunchuk";
  std::ofstream wii(config_dir + WIIPAD_CONFIG, std::ios::trunc);
  wii << R"ini([Wiimote1]
Device = Android/4/Touchscreen
Buttons/A = `Button 100`
Buttons/B = `Button 101`
Buttons/- = `Button 102`
Buttons/+ = `Button 103`
Buttons/Home = `Button 104`
D-Pad/Up = `Button 107`
D-Pad/Down = `Button 108`
D-Pad/Left = `Button 109`
D-Pad/Right = `Button 110`
IR/Up = `Axis 112`
IR/Down = `Axis 113`
IR/Left = `Axis 114`
IR/Right = `Axis 115`
IR/Forward = `Axis 116`
IR/Backward = `Axis 117`
IR/Hide = `Button 118`
IR/Total Pitch = 20
IR/Total Yaw = 25
IR/Vertical Offset = 10
Nunchuk/Buttons/C = `Button 200`
Nunchuk/Buttons/Z = `Button 201`
Nunchuk/Stick/Up = `Axis 203`
Nunchuk/Stick/Down = `Axis 204`
Nunchuk/Stick/Left = `Axis 205`
Nunchuk/Stick/Right = `Axis 206`
Classic/Buttons/A = `Button 300`
Classic/Buttons/B = `Button 301`
Classic/Buttons/X = `Button 302`
Classic/Buttons/Y = `Button 303`
Classic/Buttons/- = `Button 304`
Classic/Buttons/+ = `Button 305`
Classic/Buttons/Home = `Button 306`
Classic/Buttons/ZL = `Button 307`
Classic/Buttons/ZR = `Button 308`
Classic/D-Pad/Up = `Button 309`
Classic/D-Pad/Down = `Button 310`
Classic/D-Pad/Left = `Button 311`
Classic/D-Pad/Right = `Button 312`
Classic/Left Stick/Up = `Axis 314`
Classic/Left Stick/Down = `Axis 315`
Classic/Left Stick/Left = `Axis 316`
Classic/Left Stick/Right = `Axis 317`
Classic/Right Stick/Up = `Axis 319`
Classic/Right Stick/Down = `Axis 320`
Classic/Right Stick/Left = `Axis 321`
Classic/Right Stick/Right = `Axis 322`
Classic/Triggers/L = `Axis 323`
Classic/Triggers/R = `Axis 324`
Source = 1
Extension = )ini"
      << extension << '\n';
}

void ApplyConfiguration()
{
  const bool dual_core = IsEnabled(Option("dolphin_main_cpu_thread", "enabled"));
  const bool debugger_bridge = IsEnabled(Option("dolphin_debug_mode_enabled", "disabled"));
  const bool jit_debugging = IsEnabled(Option("dolphin_jit_debugging_enabled", "disabled"));
  const int efb_scale = std::clamp(ToInt(Option("dolphin_efb_scale", "1"), 1), 1, 8);
  const int shader_mode =
      std::clamp(ToInt(Option("dolphin_shader_compilation_mode", "3"), 3), 0, 3);
  const int texture_cache =
      std::clamp(ToInt(Option("dolphin_texture_cache_accuracy", "128"), 128), 0, 128);
  const int aspect = std::clamp(ToInt(Option("dolphin_aspect_ratio", "0"), 0), 0, 3);
  const int performance =
      std::clamp(ToInt(Option("dolphin_shield_performance_mode", "2"), 2), 0, 2);

  Config::ConfigChangeCallbackGuard guard;
  Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_OPENSLES));
  Config::SetBase(Config::MAIN_CPU_THREAD, dual_core);
  Config::SetBase(Config::MAIN_FAST_DISC_SPEED, true);
  Config::SetBase(Config::MAIN_FASTMEM, true);
  Config::SetBase(Config::MAIN_FASTMEM_ARENA, true);
  Config::SetBase(Config::MAIN_DSP_HLE, true);
  Config::SetBase(Config::MAIN_DSP_JIT, true);
  Config::SetBase(Config::GetInfoForSIDevice(0),
                  SerialInterface::SIDEVICE_GC_CONTROLLER);
  for (int channel = 1; channel < 4; ++channel)
    Config::SetBase(Config::GetInfoForSIDevice(channel), SerialInterface::SIDEVICE_NONE);
  Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, s_cpu_clock_percent != 100);
  Config::SetBase(Config::MAIN_OVERCLOCK, s_cpu_clock_percent / 100.0f);
  // Keep Gecko execution independent from Dolphin's expensive JIT debugger. Enabled codes are
  // still selected by the normal GameSettings lists, so an empty list remains a no-op.
  Config::SetBase(Config::MAIN_ENABLE_CHEATS, true);
  Config::SetBase(Config::MAIN_ENABLE_DEBUGGING, jit_debugging);
  Config::SetBase(Config::SYSCONF_WIDESCREEN, IsEnabled(Option("dolphin_widescreen", "enabled")));
  Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                  IsEnabled(Option("dolphin_widescreen", "enabled")));
  Config::SetBase(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(aspect));
  Config::SetBase(Config::GFX_SHOW_FPS, false);
  Config::SetBase(Config::GFX_SHOW_FTIMES, false);
  Config::SetBase(Config::GFX_SHOW_VPS, false);
  Config::SetBase(Config::GFX_SHOW_VTIMES, false);
  Config::SetBase(Config::GFX_SHOW_GRAPHS, false);
  Config::SetBase(Config::GFX_SHOW_SPEED, false);
  Config::SetBase(Config::GFX_EFB_SCALE, efb_scale);
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE,
                  IsEnabled(Option("dolphin_efb_access_enable", "disabled")));
  Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                  IsEnabled(Option("dolphin_efb_to_texture", "enabled")));
  Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                  IsEnabled(Option("dolphin_xfb_to_texture_enable", "enabled")));
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  static_cast<ShaderCompilationMode>(shader_mode));
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                  IsEnabled(Option("dolphin_wait_for_shaders", "enabled")));
  Config::SetBase(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, texture_cache);
  Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                  IsEnabled(Option("dolphin_gpu_texture_decoding", "disabled")));
  Config::SetBase(Config::GFX_FAST_DEPTH_CALC,
                  IsEnabled(Option("dolphin_fast_depth_calculation", "enabled")));
  Config::SetBase(Config::GFX_DISABLE_FOG,
                  IsEnabled(Option("dolphin_disable_fog", "disabled")));
  Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
                  IsEnabled(Option("dolphin_disable_copy_filter", "enabled")));
  Config::SetBase(Config::GFX_HACK_VI_SKIP,
                  IsEnabled(Option("dolphin_vi_skip", performance == 2 ? "enabled" : "disabled")));
  s_debug_enabled = debugger_bridge;
  ShieldDebuggerSetEnabled(debugger_bridge);
}

void SetButton(int type, bool pressed)
{
  ButtonManager::GamepadEvent("Touchscreen", type,
                              pressed ? ButtonManager::BUTTON_PRESSED :
                                        ButtonManager::BUTTON_RELEASED);
}

void SetAxis(int negative, int positive, int16_t value)
{
  const float normalized = std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
  ButtonManager::GamepadAxisEvent("Touchscreen", negative, std::max(0.0f, -normalized));
  ButtonManager::GamepadAxisEvent("Touchscreen", positive, std::max(0.0f, normalized));
}

void UpdateInput(uint32_t buttons, int16_t x, int16_t y, int16_t right_x, int16_t right_y)
{
  struct Mapping
  {
    uint32_t mask;
    int gc;
    int classic;
    int wiimote;
  };
  constexpr Mapping mappings[] = {
      {1, ButtonManager::BUTTON_A, ButtonManager::CLASSIC_BUTTON_A,
       ButtonManager::WIIMOTE_BUTTON_A},
      {2, ButtonManager::BUTTON_B, ButtonManager::CLASSIC_BUTTON_B,
       ButtonManager::WIIMOTE_BUTTON_B},
      {4, ButtonManager::BUTTON_Z, ButtonManager::CLASSIC_BUTTON_MINUS,
       ButtonManager::WIIMOTE_BUTTON_MINUS},
      {8, ButtonManager::BUTTON_START, ButtonManager::CLASSIC_BUTTON_PLUS,
       ButtonManager::WIIMOTE_BUTTON_PLUS},
      {16, ButtonManager::BUTTON_RIGHT, ButtonManager::CLASSIC_DPAD_RIGHT,
       ButtonManager::WIIMOTE_RIGHT},
      {32, ButtonManager::BUTTON_LEFT, ButtonManager::CLASSIC_DPAD_LEFT,
       ButtonManager::WIIMOTE_LEFT},
      {64, ButtonManager::BUTTON_UP, ButtonManager::CLASSIC_DPAD_UP, ButtonManager::WIIMOTE_UP},
      {128, ButtonManager::BUTTON_DOWN, ButtonManager::CLASSIC_DPAD_DOWN,
       ButtonManager::WIIMOTE_DOWN},
      {256, ButtonManager::BUTTON_Z, ButtonManager::CLASSIC_BUTTON_ZR,
       ButtonManager::NUNCHUK_BUTTON_Z},
      {512, ButtonManager::TRIGGER_L, ButtonManager::CLASSIC_BUTTON_ZL,
       ButtonManager::NUNCHUK_BUTTON_C},
      {1024, ButtonManager::BUTTON_X, ButtonManager::CLASSIC_BUTTON_X,
       ButtonManager::WIIMOTE_BUTTON_1},
      {2048, ButtonManager::BUTTON_Y, ButtonManager::CLASSIC_BUTTON_Y,
       ButtonManager::WIIMOTE_BUTTON_2},
  };
  for (const Mapping& mapping : mappings)
  {
    const bool pressed = (buttons & mapping.mask) != 0;
    SetButton(mapping.gc, pressed);
    SetButton(mapping.classic, pressed);
    SetButton(mapping.wiimote, pressed);
  }
  SetButton(ButtonManager::TRIGGER_L, (buttons & 4096) != 0);
  SetButton(ButtonManager::TRIGGER_R, (buttons & 8192) != 0);
  SetButton(ButtonManager::CLASSIC_TRIGGER_L, (buttons & 4096) != 0);
  SetButton(ButtonManager::CLASSIC_TRIGGER_R, (buttons & 8192) != 0);
  SetAxis(ButtonManager::STICK_MAIN_LEFT, ButtonManager::STICK_MAIN_RIGHT, x);
  SetAxis(ButtonManager::STICK_MAIN_UP, ButtonManager::STICK_MAIN_DOWN, y);
  SetAxis(ButtonManager::STICK_C_LEFT, ButtonManager::STICK_C_RIGHT, right_x);
  SetAxis(ButtonManager::STICK_C_UP, ButtonManager::STICK_C_DOWN, right_y);
  SetAxis(ButtonManager::CLASSIC_STICK_LEFT_LEFT, ButtonManager::CLASSIC_STICK_LEFT_RIGHT, x);
  SetAxis(ButtonManager::CLASSIC_STICK_LEFT_UP, ButtonManager::CLASSIC_STICK_LEFT_DOWN, y);
  SetAxis(ButtonManager::CLASSIC_STICK_RIGHT_LEFT, ButtonManager::CLASSIC_STICK_RIGHT_RIGHT,
          right_x);
  SetAxis(ButtonManager::CLASSIC_STICK_RIGHT_UP, ButtonManager::CLASSIC_STICK_RIGHT_DOWN, right_y);
  SetAxis(ButtonManager::NUNCHUK_STICK_LEFT, ButtonManager::NUNCHUK_STICK_RIGHT, x);
  SetAxis(ButtonManager::NUNCHUK_STICK_UP, ButtonManager::NUNCHUK_STICK_DOWN, y);
  SetAxis(ButtonManager::WIIMOTE_IR_LEFT, ButtonManager::WIIMOTE_IR_RIGHT, right_x);
  SetAxis(ButtonManager::WIIMOTE_IR_UP, ButtonManager::WIIMOTE_IR_DOWN, right_y);
}
}  // namespace

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}
void Host_PPCSymbolsChanged() {}
void Host_RefreshDSPDebuggerWindow() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID) {}
void Host_UpdateTitle(const std::string& title)
{
  __android_log_print(ANDROID_LOG_INFO, TAG, "%s", title.c_str());
}
void Host_UpdateDisasmDialog() {}
void Host_UpdateMainFrame() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() { return true; }
bool Host_RendererHasFullFocus() { return true; }
bool Host_RendererIsFullscreen() { return true; }
void Host_YieldToUI() { std::this_thread::yield(); }
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

extern "C" void dolphin_shield_clear_options()
{
  std::lock_guard lock(s_mutex);
  s_options.clear();
}

extern "C" void dolphin_shield_set_option(const char* key, const char* value)
{
  if (!key || !value)
    return;
  std::lock_guard lock(s_mutex);
  s_options[key] = value;
}

extern "C" void dolphin_shield_set_cpu_clock_percent(int percent)
{
  s_cpu_clock_percent = std::clamp(percent, 5, 300);
}

extern "C" void dolphin_shield_set_audio_sync_enabled(bool enabled)
{
  s_audio_sync = enabled;
}

extern "C" void dolphin_shield_set_profile(const char* game_id, const char* name,
                                            const char* input_mode)
{
  std::lock_guard lock(s_mutex);
  s_game_id = game_id && *game_id ? game_id : "UNKNOWN";
  s_profile_name = name && *name ? name : "Shield Default";
  s_input_mode = input_mode && *input_mode ? input_mode : "gc_pad";
}

extern "C" void dolphin_shield_set_surface(ANativeWindow* window)
{
  std::lock_guard lock(s_mutex);
  ANativeWindow* old = s_window;
  s_window = window;
  if (g_presenter)
    g_presenter->ChangeSurface(window);
  if (old)
    ANativeWindow_release(old);
}

extern "C" bool dolphin_shield_initialize(const char* rom_path, const char* save_directory,
                                           const char* config_directory)
{
  if (!rom_path || !save_directory || !config_directory)
    return false;
  s_error.clear();
  Common::RegisterMsgAlertHandler(AlertHandler);
  Common::AndroidSetReportHandler([](std::string, std::string) {});
  DolphinAnalytics::AndroidSetGetValFunc([](std::string key) {
    if (key == "DEVICE_MANUFACTURER")
      return std::string("NVIDIA");
    if (key == "DEVICE_MODEL")
      return std::string("SHIELD Android TV");
    if (key == "DEVICE_OS")
      return std::string("Android native host");
    if (key == "DEVICE_TYPE")
      return std::string("shield-native");
    return std::string();
  });
  Core::DeclareAsHostThread();
  File::SetSysDirectory(std::string(config_directory) + "/dolphin-emu/Sys");
  UICommon::SetUserDirectory(std::string(save_directory) + "/User");
#ifdef DOLPHIN_SHIELD_DEVELOPMENT
  // Keep generated development artifacts away from the stable core. NAND and memory cards stay
  // shared so both variants can run the same game progress during A/B testing.
  File::SetUserPath(D_CONFIG_IDX, std::string(save_directory) + "/User/Config-Development/");
  File::SetUserPath(D_CACHE_IDX, std::string(save_directory) + "/User/Cache-Development/");
#endif
  File::SetUserPath(D_GAMESETTINGS_IDX, std::string(config_directory) + "/User/GameSettings/");
  UICommon::CreateDirectories();
  WriteControllerConfigs(s_input_mode);
  UICommon::Init();
  ApplyConfiguration();

  ANativeWindow* window = nullptr;
  for (int attempt = 0; attempt < 200 && !window; ++attempt)
  {
    {
      std::lock_guard lock(s_mutex);
      window = s_window;
    }
    if (!window)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!window)
  {
    s_error = "No Android render surface was supplied";
    UICommon::Shutdown();
    Core::UndeclareAsHostThread();
    return false;
  }

  const WindowSystemInfo wsi(WindowSystemType::Android, nullptr, window, window);
  UICommon::InitControllers(wsi);
  auto boot = BootParameters::GenerateFromFile(rom_path, BootSessionData());
  if (!boot || !BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    if (s_error.empty())
      s_error = "Dolphin could not boot the disc image";
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
    Core::UndeclareAsHostThread();
    return false;
  }
  ButtonManager::Init(SConfig::GetInstance().GetGameID());
  s_state_callback = Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Uninitialized)
      s_running = false;
  });
  s_initialized = true;
  s_running = true;
  __android_log_print(ANDROID_LOG_INFO, TAG, "Started %s with profile %s", rom_path,
                      s_profile_name.c_str());
  return true;
}

extern "C" bool dolphin_shield_poll(uint32_t buttons, int16_t x, int16_t y, int16_t right_x,
                                    int16_t right_y)
{
  if (!s_initialized)
    return false;
  Core::System& system = Core::System::GetInstance();
  Core::HostDispatchJobs(system);
  UpdateInput(buttons, x, y, right_x, right_y);
  if (g_presenter)
  {
    const int frame = g_presenter->FrameCount();
    if (frame != s_last_frame)
    {
      s_last_frame = frame;
      ShieldDebuggerSample();
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return s_running && Core::IsRunningOrStarting(system);
}

extern "C" void dolphin_shield_reset()
{
  if (s_initialized)
    Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
}

extern "C" bool dolphin_shield_save_state(const char* path)
{
  if (!s_initialized || !path || !*path)
    return false;
  State::SaveAs(Core::System::GetInstance(), path, true);
  return File::Exists(path);
}

extern "C" bool dolphin_shield_load_state(const char* path)
{
  if (!s_initialized || !path || !File::Exists(path))
    return false;
  State::LoadAs(Core::System::GetInstance(), path);
  return true;
}

extern "C" void dolphin_shield_pause()
{
  if (s_initialized)
    Core::SetState(Core::System::GetInstance(), Core::State::Paused);
}

extern "C" void dolphin_shield_resume()
{
  if (s_initialized)
    Core::SetState(Core::System::GetInstance(), Core::State::Running);
}

extern "C" void dolphin_shield_shutdown()
{
  if (s_initialized)
  {
    Core::System& system = Core::System::GetInstance();
    ButtonManager::Shutdown();
    Core::Stop(system);
    while (Core::IsRunningOrStarting(system))
    {
      Core::HostDispatchJobs(system);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Core::Shutdown(system);
    if (s_state_callback >= 0)
      Core::RemoveOnStateChangedCallback(&s_state_callback);
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
    Core::UndeclareAsHostThread();
  }
  s_initialized = false;
  s_running = false;
  ShieldDebuggerSetEnabled(false);
  {
    std::lock_guard lock(s_mutex);
    if (s_window)
      ANativeWindow_release(s_window);
    s_window = nullptr;
  }
}

extern "C" double dolphin_shield_fps()
{
  return s_initialized ? g_perf_metrics.GetFPS() : 0.0;
}
extern "C" uint64_t dolphin_shield_frame_count()
{
  return g_presenter ? static_cast<uint64_t>(std::max(0, g_presenter->FrameCount())) : 0;
}
extern "C" double dolphin_shield_speed()
{
  return s_initialized ? g_perf_metrics.GetSpeed() : 0.0;
}
extern "C" bool dolphin_shield_debug_enabled() { return s_debug_enabled; }
extern "C" int dolphin_shield_debug_command(const char* command, char* response,
                                             std::size_t response_size)
{
  return ShieldDebuggerCommand(command, response, response_size);
}
extern "C" int dolphin_shield_shader_progress(int* percent, char* detail, std::size_t detail_size)
{
  if (percent)
    *percent = s_initialized ? 100 : 0;
  if (detail && detail_size)
    std::snprintf(detail, detail_size, "%s", s_initialized ? "Native shader cache ready" : "Idle");
  return s_initialized ? 2 : 0;
}
extern "C" const char* dolphin_shield_error() { return s_error.c_str(); }
