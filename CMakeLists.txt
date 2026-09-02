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
  Lavfi/Executor.hpp
  Lavfi/AudioNode.hpp
  Lavfi/Node.hpp
)

add_library(score_addon_lavfi
  ${LAVFI_CORE_SOURCES}
  ${LAVFI_HDRS}

  Lavfi/Process.cpp
  Lavfi/Executor.cpp
  Lavfi/AudioNode.cpp
  Lavfi/Node.cpp

  score_addon_lavfi.hpp
  score_addon_lavfi.cpp
)

score_generate_command_list_file(score_addon_lavfi "${LAVFI_HDRS}")

target_link_libraries(score_addon_lavfi PUBLIC
  score_plugin_gfx
  score_plugin_media
  score_plugin_engine
  score_plugin_scenario
  avfilter
)

setup_score_plugin(score_addon_lavfi)

# --- headless test of the wrapper -------------------------------------------
# Not gated on BUILD_TESTING: score's own tests are opt-in per target and this
# one is tiny, but it must not break an SDK build, where CTest is not set up.
if(TARGET Catch2::Catch2 OR SCORE_TESTBED OR BUILD_TESTING)
  add_executable(score_addon_lavfi_graph_test tests/GraphTest.cpp ${LAVFI_CORE_SOURCES})
  target_include_directories(score_addon_lavfi_graph_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_lavfi_graph_test PRIVATE avfilter avutil)
  target_compile_features(score_addon_lavfi_graph_test PRIVATE cxx_std_20)
  if(COMMAND add_test)
    add_test(NAME score_addon_lavfi_graph_test COMMAND score_addon_lavfi_graph_test)
  endif()
endif()
