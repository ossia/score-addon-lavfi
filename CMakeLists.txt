cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

if(EMSCRIPTEN)
  return()
endif()
if(NOT TARGET score_plugin_gfx OR NOT TARGET score_plugin_media)
  message(STATUS "score-addon-lavfi: requires the gfx and media plug-ins")
  return()
endif()

# FindFFmpeg probes libavfilter and creates the imported target `avfilter`,
# but deliberately keeps it out of FFMPEG_TARGETS (nothing in score links it),
# so it may or may not exist depending on who ran find_package first.
if(NOT TARGET avfilter)
  find_package(FFmpeg COMPONENTS AVFILTER AVUTIL AVCODEC AVFORMAT SWSCALE SWRESAMPLE)
endif()
if(NOT TARGET avfilter)
  message(STATUS "score-addon-lavfi: libavfilter not found, skipping")
  return()
endif()

# Being present is not enough: the graph wrapper needs the segment API
# (avfilter_graph_segment_*), which arrived in lavfi 9.7 / FFmpeg 6.1 and is
# what lets hw_device_ctx reach a filter before its init(). Core/Graph.cpp says
# the same thing with #error, so a distribution that ships an older FFmpeg --
# Debian bookworm, for one -- would otherwise configure happily and then fail
# the build. Asked of the header rather than of a pkg-config version variable,
# which is only set when the component was found that way.
include(CheckCXXSourceCompiles)
set(_lavfi_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
get_target_property(_lavfi_incs avfilter INTERFACE_INCLUDE_DIRECTORIES)
if(_lavfi_incs)
  list(APPEND CMAKE_REQUIRED_INCLUDES ${_lavfi_incs})
endif()
check_cxx_source_compiles("
extern \"C\" {
#include <libavfilter/version.h>
#include <libavutil/version.h>
}
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(9, 7, 100)
#error \"libavfilter is older than 9.7\"
#endif
int main() { return 0; }
" SCORE_LAVFI_HAS_SEGMENT_API)
set(CMAKE_REQUIRED_INCLUDES "${_lavfi_saved_includes}")

if(NOT SCORE_LAVFI_HAS_SEGMENT_API)
  message(STATUS "score-addon-lavfi: libavfilter is older than 9.7 (FFmpeg 6.1), skipping")
  return()
endif()

project(score_addon_lavfi LANGUAGES CXX)

score_common_setup()

# The libavfilter wrapper is Qt-free and ossia-free so it can be tested
# headless; it is compiled into the plug-in and into the test binary.
set(LAVFI_CORE_SOURCES
  Lavfi/Core/Graph.hpp
  Lavfi/Core/Graph.cpp
)

set(LAVFI_HDRS
  Lavfi/Commands.hpp
  Lavfi/Metadata.hpp
  Lavfi/Process.hpp
  Lavfi/Layer.hpp
  Lavfi/Library.hpp
  Lavfi/Executor.hpp
  Lavfi/AudioNode.hpp
  Lavfi/Node.hpp
  Lavfi/Gfx/HwDevice.hpp
  Lavfi/Gfx/VulkanTransport.hpp
)

add_library(score_addon_lavfi
  ${LAVFI_CORE_SOURCES}
  ${LAVFI_HDRS}

  Lavfi/Process.cpp
  Lavfi/Executor.cpp
  Lavfi/AudioNode.cpp
  Lavfi/Node.cpp
  Lavfi/Gfx/HwDevice.cpp
  Lavfi/Gfx/VulkanTransport.cpp

  score_addon_lavfi.hpp
  score_addon_lavfi.cpp
)

score_generate_command_list_file(score_addon_lavfi "${LAVFI_HDRS}")

target_link_libraries(score_addon_lavfi PUBLIC
  score_plugin_gfx
  score_plugin_media
  score_plugin_engine
  score_plugin_scenario
  score_plugin_library
  avfilter
)

setup_score_plugin(score_addon_lavfi)

# --- tests ------------------------------------------------------------------
# Headless tests of the wrapper: no Qt, no score, no GPU.
#
# Gated on SCORE_TESTING, which only the coverage job sets, and that job builds
# against the distribution's shared FFmpeg. They cannot be built against the
# SDK's static FFmpeg: linking `avfilter avutil` there leaves the rest of the
# archives' dependency closure unresolved -- libavfilter.a(af_pan.o) wants
# swr_*, libavutil.a(hwcontext_{drm,vulkan}.o) wants drm*, libavcodec.a
# (hapenc.o) wants snappy_*, and udev_* goes unresolved too. score's own
# targets get all of that through score_plugin_media and score_plugin_gfx;
# these deliberately link neither, so they stay out of packaging builds.
if(SCORE_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/GraphTest.cpp")
  add_executable(score_addon_lavfi_graph_test tests/GraphTest.cpp ${LAVFI_CORE_SOURCES})
  target_include_directories(score_addon_lavfi_graph_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_lavfi_graph_test PRIVATE avfilter avutil)
  target_compile_features(score_addon_lavfi_graph_test PRIVATE cxx_std_20)
  # It also runs every shipped preset. A preset whose filters this FFmpeg was
  # not built with is skipped, not failed.
  target_compile_definitions(score_addon_lavfi_graph_test PRIVATE
    "LAVFI_PRESET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Presets/FFmpeg filter\"")
endif()

# What the presets DO: each one is fed material chosen for it and the output
# frames or samples are checked against what the filter is supposed to have
# done. Same dependencies as the graph test.
if(SCORE_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/PresetBehaviourTest.cpp")
  add_executable(score_addon_lavfi_preset_test tests/PresetBehaviourTest.cpp ${LAVFI_CORE_SOURCES})
  target_include_directories(score_addon_lavfi_preset_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_lavfi_preset_test PRIVATE avfilter avutil)
  target_compile_features(score_addon_lavfi_preset_test PRIVATE cxx_std_20)
  target_compile_definitions(score_addon_lavfi_preset_test PRIVATE
    "LAVFI_PRESET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Presets/FFmpeg filter\"")
endif()

# GPU test of the Vulkan transport on score's shared VkDevice. Needs a Vulkan
# device and a display; prints SKIP and exits 0 without one. Off by default
# in SDK/addon CI builds (no GPU there), on where score builds its own testers.
option(SCORE_LAVFI_GPU_TESTS "Build the lavfi Vulkan transport test" ${SCORE_TESTING})
if(SCORE_LAVFI_GPU_TESTS AND NOT EMSCRIPTEN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/VulkanTransportTest.cpp")
  add_executable(score_addon_lavfi_vulkan_test tests/VulkanTransportTest.cpp)
  target_link_libraries(score_addon_lavfi_vulkan_test PRIVATE
    score_addon_lavfi score_plugin_gfx score_plugin_media ${QT_PREFIX}::Gui)
endif()

# The render pipeline itself: source node -> lavfi node -> offscreen sink, with
# the pixels checked. Uses score's own gfx test fixture, so it only exists in a
# score build that has the test tree (score_add_test comes from the same place).
# score_add_test and Catch2 are available while src/ is being configured;
# score_test_fixtures itself only appears later, in tests/, which is fine:
# CMake resolves a link to a target defined later in the same project.
if(COMMAND score_add_test AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/GfxPresetTest.cpp")
  score_add_test(score_addon_lavfi_gfx_test
    SOURCES tests/GfxPresetTest.cpp
    GUI
    PLUGINS score_plugin_gfx score_plugin_scenario score_lib_process
    LIBS score_addon_lavfi ${QT_PREFIX}::Gui avfilter avutil)
  target_include_directories(score_addon_lavfi_gfx_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # score_test_fixtures carries this include dir, but it is declared after
    # us, so its INTERFACE properties are not there to inherit yet.
    "${SCORE_ROOT_SOURCE_DIR}/tests/fixtures")
  # The source pictures come from score's own shader corpus.
  target_compile_definitions(score_addon_lavfi_gfx_test PRIVATE
    GFX_TEST_CORPUS_DIR="${SCORE_ROOT_SOURCE_DIR}/tests/gfx/corpus")
endif()

if(COMMAND add_test AND TARGET score_addon_lavfi_graph_test)
  add_test(NAME score_addon_lavfi_graph_test COMMAND score_addon_lavfi_graph_test)
  if(TARGET score_addon_lavfi_preset_test)
    add_test(NAME score_addon_lavfi_preset_test COMMAND score_addon_lavfi_preset_test)
    set_tests_properties(score_addon_lavfi_preset_test PROPERTIES TIMEOUT 600)
  endif()
  # The same presets, but through score itself: the process model builds its
  # ports from the graph and the engine runs every one of them. The addons are
  # configured before src/app, so the ossia-score TARGET does not exist yet and
  # cannot be tested for here; the runner reports ctest's skip code (77) when
  # the binary is not there, which is also what happens in an SDK build.
  if(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-presets.sh")
    add_test(NAME score_addon_lavfi_presets
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-presets.sh"
                     "${CMAKE_BINARY_DIR}/ossia-score")
    set_tests_properties(score_addon_lavfi_presets PROPERTIES
      LABELS "gui" TIMEOUT 400 SKIP_RETURN_CODE 77)
  endif()
  if(TARGET score_addon_lavfi_vulkan_test)
    add_test(NAME score_addon_lavfi_vulkan_test COMMAND score_addon_lavfi_vulkan_test)
    set_tests_properties(score_addon_lavfi_vulkan_test PROPERTIES
      ENVIRONMENT "SCORE_AUDIO_BACKEND=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=0"
      LABELS "gui")
  endif()
endif()
