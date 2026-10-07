set(DOLPHIN_SHIELD_OUTPUT_NAME "dolphin_shield" CACHE STRING
  "Output name for the Shield Dolphin shared library")
option(DOLPHIN_SHIELD_DEVELOPMENT "Build the isolated development core variant" OFF)

# The old Android app's C interface (Main.cpp).
if(ANDROID)
add_library(dolphin_shield SHARED
  Main.cpp
  ShieldDebugger.cpp
)

target_link_libraries(dolphin_shield PRIVATE
  core
  uicommon
  androidcommon
  android
  log
)

set_target_properties(dolphin_shield PROPERTIES
  OUTPUT_NAME "${DOLPHIN_SHIELD_OUTPUT_NAME}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(DOLPHIN_SHIELD_DEVELOPMENT)
  target_compile_definitions(dolphin_shield PRIVATE DOLPHIN_SHIELD_DEVELOPMENT=1)
endif()

target_link_options(dolphin_shield PRIVATE "-Wl,--no-warn-mismatch")
target_compile_options(dolphin_shield PRIVATE "-fvisibility=default")
endif()

# Tamariba runs Dolphin as a libretro core (Libretro.cpp), on every platform. It brings the
# libretro headers itself; without them this library is not built.
set(DOLPHIN_LIBRETRO_INCLUDE_DIR "" CACHE PATH "Folder holding libretro.h and libretro_vulkan.h")
if(DOLPHIN_LIBRETRO_INCLUDE_DIR)
  add_library(dolphin_libretro SHARED Libretro.cpp TamaribaNetPlay.cpp)
  # Dolphin's own Vulkan headers first (VKHost.h), as videovulkan uses them.
  target_include_directories(dolphin_libretro PRIVATE
    ${CMAKE_SOURCE_DIR}/Externals/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/Externals/VulkanMemoryAllocator/include
    "${DOLPHIN_LIBRETRO_INCLUDE_DIR}")
  target_link_libraries(dolphin_libretro PRIVATE core uicommon)
  if(ANDROID)
    target_link_libraries(dolphin_libretro PRIVATE androidcommon log)
    target_link_options(dolphin_libretro PRIVATE "-Wl,-z,max-page-size=16384")
  endif()
  set_target_properties(dolphin_libretro PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(WIN32)
    # A DLL is a runtime output (Binaries/ in a Windows build): put it where the other
    # platforms' library goes.
    set_target_properties(dolphin_libretro PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  endif()
endif()

# Tamariba: tests of the Wii's network through the Plaza's internet exit (TamaribaNetTest.cpp,
# against a stand-in exit) and of the WiiLink WFC patches' lookup. Not built by default: cmake --build . --target dolphin_tamariba_net_test
add_executable(dolphin_tamariba_net_test EXCLUDE_FROM_ALL
  TamaribaNetTest.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/IOS/Network/TamaribaNet.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/TamaribaWFC.cpp
  ${CMAKE_SOURCE_DIR}/Source/Core/Core/GeckoCodeConfig.cpp)
target_link_libraries(dolphin_tamariba_net_test PRIVATE common MbedTLS::mbedtls)
target_compile_definitions(dolphin_tamariba_net_test PRIVATE
  TAMARIBA_SYS_DIR="${CMAKE_SOURCE_DIR}/Data/Sys")
