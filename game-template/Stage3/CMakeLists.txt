cmake_minimum_required(VERSION 3.16)
project(revm-game-stage LANGUAGES CXX)

# Stage 3 plugin: this directory is linked into revm as the CPU replica.
if(NOT C64REWORK_ROOT OR NOT REWORK_RUNTIME_OUTPUT_DIRECTORY OR
   NOT REWORK_GAME_NAME OR NOT REWORK_STAGE_NUMBER)
  message(FATAL_ERROR
    "Build through the game-root rework launcher; framework and output roots are required")
endif()

set(REVM_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(REVM_OUTPUT_NAME "revm-s${REWORK_STAGE_NUMBER}-${REWORK_GAME_NAME}")

include("${C64REWORK_ROOT}/src/revm/cmake/RevmCommon.cmake")
set_target_properties(revm PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${REWORK_RUNTIME_OUTPUT_DIRECTORY}")
