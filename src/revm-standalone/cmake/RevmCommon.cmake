# Standalone export build — include()'d after project() from the exported
# game root. Always builds one executable and one cpu-mock game object set.
#
# Set before include:
#   REVM_GAME_DIR      — required path to the lowered game sources
#   REVM_OUTPUT_NAME   — binary name under bin/ (default: game)

get_filename_component(_REVM_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_REVM_ROOT "${_REVM_CMAKE_DIR}/.." ABSOLUTE)
get_filename_component(_C64REWORK_ROOT "${_REVM_ROOT}/../.." ABSOLUTE)
set(_C64REWORK_VERSION_FILE "${_C64REWORK_ROOT}/c64rework.version")
if(NOT EXISTS "${_C64REWORK_VERSION_FILE}")
  message(FATAL_ERROR "Missing C64 Rework version: ${_C64REWORK_VERSION_FILE}")
endif()
file(READ "${_C64REWORK_VERSION_FILE}" C64REWORK_VERSION)
string(STRIP "${C64REWORK_VERSION}" C64REWORK_VERSION)
if(NOT C64REWORK_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Invalid C64 Rework VERSION: ${C64REWORK_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The cycle-stepped host crosses several static-library boundaries on every
# emulated Phi2.  Release IPO lets the compiler inline those tiny probes and
# dispatch helpers without changing debug/profile builds.
option(REVM_ENABLE_IPO "Enable interprocedural optimization in Release builds" OFF)
if(REVM_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _REVM_IPO_SUPPORTED OUTPUT _REVM_IPO_ERROR
                      LANGUAGES CXX)
  if(_REVM_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "REVM Release IPO unavailable: ${_REVM_IPO_ERROR}")
  endif()
endif()

# Objects/libs stay under the caller’s -B dir; binaries land in repo bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${_C64REWORK_ROOT}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT REVM_OUTPUT_NAME)
  set(REVM_OUTPUT_NAME "game")
endif()
if(NOT REVM_GAME_DIR)
  message(FATAL_ERROR "REVM_GAME_DIR must point at the lowered game sources")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(Threads REQUIRED)
find_package(PNG 1.6 REQUIRED)

set(FRODO_DIR "${_REVM_ROOT}/src/vendor/frodo")
set(REVM_DIR "${_REVM_ROOT}/src")
set(RESID_DIR "${_C64REWORK_ROOT}/src/resid")

if(NOT EXISTS "${RESID_DIR}/sid.h")
  message(FATAL_ERROR
    "Vendored reSID not found at ${RESID_DIR}.\n"
    "  Standalone exports must include src/resid/ next to src/revm/.\n"
    "  Re-run ./rework export from the game root; do not treat reSID as a git submodule here.")
endif()
foreach(_w
    wave6581_PST wave6581_PS_ wave6581_P_T wave6581__ST
    wave8580_PST wave8580_PS_ wave8580_P_T wave8580__ST)
  if(NOT EXISTS "${RESID_DIR}/${_w}.dat")
    message(FATAL_ERROR "Vendored reSID waveform table missing: ${RESID_DIR}/${_w}.dat")
  endif()
endforeach()
find_program(PERL_EXECUTABLE perl REQUIRED)

# --- reSID (vendored sources; build-dir generated headers) ---
set(RESID_GEN_DIR "${CMAKE_BINARY_DIR}/resid-gen")
file(MAKE_DIRECTORY "${RESID_GEN_DIR}")

set(RESID_SIDDEFS_IN "${RESID_DIR}/siddefs.h.in")
set(RESID_SIDDEFS_OUT "${RESID_GEN_DIR}/siddefs.h")
# Values match a typical reSID configure (inline + new 8580 filter + branch hints).
file(READ "${RESID_SIDDEFS_IN}" _RESID_SIDDEFS)
string(REPLACE "@RESID_INLINING@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@RESID_INLINE@" "inline" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@RESID_BRANCH_HINTS@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@NEW_8580_FILTER@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@HAVE_BOOL@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@HAVE_BUILTIN_EXPECT@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
string(REPLACE "@HAVE_LOG1P@" "1" _RESID_SIDDEFS "${_RESID_SIDDEFS}")
set(RESID_SIDDEFS_TMP "${RESID_GEN_DIR}/siddefs.h.tmp")
file(WRITE "${RESID_SIDDEFS_TMP}" "${_RESID_SIDDEFS}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${RESID_SIDDEFS_TMP}" "${RESID_SIDDEFS_OUT}")
file(REMOVE "${RESID_SIDDEFS_TMP}")

set(RESID_WAVE_DATS
  wave6581_PST wave6581_PS_ wave6581_P_T wave6581__ST
  wave8580_PST wave8580_PS_ wave8580_P_T wave8580__ST
)
set(RESID_WAVE_HEADERS)
foreach(_w ${RESID_WAVE_DATS})
  set(_dat "${RESID_DIR}/${_w}.dat")
  set(_hdr "${RESID_GEN_DIR}/${_w}.h")
  add_custom_command(
    OUTPUT "${_hdr}"
    COMMAND ${PERL_EXECUTABLE} "${RESID_DIR}/samp2src.pl" "${_w}" "${_dat}" "${_hdr}"
    DEPENDS "${_dat}" "${RESID_DIR}/samp2src.pl"
    COMMENT "Generating reSID ${_w}.h"
    VERBATIM)
  list(APPEND RESID_WAVE_HEADERS "${_hdr}")
endforeach()

set(RESID_SOURCES
  ${RESID_DIR}/sid.cc
  ${RESID_DIR}/voice.cc
  ${RESID_DIR}/wave.cc
  ${RESID_DIR}/envelope.cc
  ${RESID_DIR}/filter8580new.cc
  ${RESID_DIR}/dac.cc
  ${RESID_DIR}/extfilt.cc
  ${RESID_DIR}/pot.cc
  ${RESID_DIR}/version.cc
)

add_library(resid STATIC ${RESID_SOURCES} ${RESID_WAVE_HEADERS})
# Keep resid/sid.h off PUBLIC include paths — it collides with Frodo SID.h on
# case-insensitive filesystems. Only resid + ResidEngine.cpp see it.
target_include_directories(resid PRIVATE "${RESID_DIR}" "${RESID_GEN_DIR}")
target_compile_definitions(resid PRIVATE VERSION="1.0-pre2" NDEBUG NEW_8580_FILTER=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(resid PRIVATE -Wno-unused-parameter)
endif()

# ResidEngine.cpp must see resid/sid.h, not Frodo SID.h.
add_library(resid_engine STATIC "${REVM_DIR}/sid/ResidEngine.cpp")
target_include_directories(resid_engine PUBLIC "${REVM_DIR}")
target_include_directories(resid_engine PRIVATE "${RESID_DIR}" "${RESID_GEN_DIR}")
target_compile_definitions(resid_engine PRIVATE NEW_8580_FILTER=1 NDEBUG)
target_link_libraries(resid_engine PUBLIC resid)

# --- Vendored Frodo SC cores ---
set(FRODO_SOURCES
  ${FRODO_DIR}/C64_SC.cpp
  ${FRODO_DIR}/CPUC64_SC.cpp
  ${FRODO_DIR}/CPU_common.cpp
  ${FRODO_DIR}/VIC_SC.cpp
  ${FRODO_DIR}/CIA_SC.cpp
  ${FRODO_DIR}/SID.cpp
  ${FRODO_DIR}/Display.cpp
  ${FRODO_DIR}/Prefs.cpp
  ${FRODO_DIR}/frodo_globals.cpp
  ${REVM_DIR}/sid/ResidRenderer.cpp
)

add_library(frodo_sc STATIC ${FRODO_SOURCES})
target_compile_definitions(frodo_sc PUBLIC FRODO_SC)
target_include_directories(frodo_sc PUBLIC ${FRODO_DIR} ${REVM_DIR})
target_link_libraries(frodo_sc PUBLIC PkgConfig::SDL2 Threads::Threads resid_engine)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(frodo_sc PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-result)
endif()

# --- Main-only standalone runtime ---
set(REVM_SOURCES
  ${REVM_DIR}/core/Board.cpp
  ${REVM_DIR}/input/LiveInput.cpp
  ${REVM_DIR}/input/JoystickConfig.cpp
  ${REVM_DIR}/goldens/PlayInput.cpp
  ${REVM_DIR}/goldens/PlayRecorder.cpp
  ${REVM_DIR}/util/Log.cpp
  ${REVM_DIR}/util/Png.cpp
  ${REVM_DIR}/gamehost/GameHost.cpp
  ${REVM_DIR}/core/StandaloneRunner.cpp
)
add_library(revm_standalone_runtime STATIC ${REVM_SOURCES})
target_include_directories(revm_standalone_runtime PUBLIC ${REVM_DIR} ${REVM_DIR}/vendor)
target_link_libraries(revm_standalone_runtime PUBLIC frodo_sc PNG::PNG)

add_executable(revm "${_REVM_ROOT}/apps/revm/main.cpp")
target_compile_definitions(revm PRIVATE C64REWORK_VERSION="${C64REWORK_VERSION}")
target_link_libraries(revm PRIVATE revm_standalone_runtime)
set_target_properties(revm PROPERTIES OUTPUT_NAME "${REVM_OUTPUT_NAME}")

if(NOT EXISTS "${REVM_GAME_DIR}/plugin.cpp")
  message(FATAL_ERROR "missing plugin.cpp in ${REVM_GAME_DIR}")
endif()
set(_src "${REVM_GAME_DIR}/plugin.cpp")
file(GLOB_RECURSE _extra CONFIGURE_DEPENDS "${REVM_GAME_DIR}/*.cpp")
list(FILTER _extra EXCLUDE REGEX ".*/plugin\\.cpp$")
list(FILTER _extra EXCLUDE REGEX ".*/\\._[^/]+$")
add_library(revm_cpu_mock_plugin OBJECT ${_src} ${_extra})
target_include_directories(revm_cpu_mock_plugin PRIVATE "${REVM_GAME_DIR}")
target_include_directories(revm_cpu_mock_plugin PRIVATE ${REVM_DIR} ${REVM_DIR}/vendor)
target_link_libraries(revm_cpu_mock_plugin PUBLIC revm_standalone_runtime)
target_link_libraries(revm PRIVATE revm_cpu_mock_plugin)
message(STATUS "REVM cpu-mock plugin → ${REVM_GAME_DIR}")

install(TARGETS revm RUNTIME DESTINATION bin)
