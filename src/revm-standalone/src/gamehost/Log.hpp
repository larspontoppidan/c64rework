// Created  : 2026-09-13
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Game-facing logging for standalone exports: the same logger, levels, and
// output format as the runtime's util/Log.hpp, re-exposed under game-facing
// names so exported game source carries no REVM identifiers. Game modules
// keep their existing statements and module tags; the export lowering only
// rewires the macro and level names.

#include "util/Log.hpp"

#define GAME_VERBOSE ::revm::log::Level::Verbose
#define GAME_DEBUG ::revm::log::Level::Debug
#define GAME_INFO ::revm::log::Level::Info
#define GAME_ERROR ::revm::log::Level::Error

// Per .cpp file, before first GAME_LOG / GAME_LOG_TIMED use:
//   #define GAME_LOG_MODULE "ff-game"
// Expanded at the call site, like REVM_LOG_MODULE.
#define GAME_LOG(level, ...) \
	::revm::log::Logf((level), false, GAME_LOG_MODULE, __VA_ARGS__)
#define GAME_LOG_TIMED(level, ...) \
	::revm::log::Logf((level), true, GAME_LOG_MODULE, __VA_ARGS__)
