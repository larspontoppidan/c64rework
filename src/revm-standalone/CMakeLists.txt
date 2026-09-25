cmake_minimum_required(VERSION 3.16)
project(revm LANGUAGES CXX)

# Stage 1 host — no game plugin. Binaries → <repo>/bin/
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RevmCommon.cmake")
