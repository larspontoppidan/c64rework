cmake_minimum_required(VERSION 3.16)
project(c64rework_standalone_game LANGUAGES CXX)

set(REVM_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
set(REVM_OUTPUT_NAME "game")
include("${CMAKE_CURRENT_SOURCE_DIR}/src/revm/cmake/RevmCommon.cmake")
