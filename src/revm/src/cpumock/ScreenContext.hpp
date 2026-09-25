// Created  : 2026-08-24
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

// Screen-fail context oracle — decodes WHAT diverged at Main↔Twin screen
// compare-fail time into C64 display semantics (VIC config drift, implicated
// video-matrix cells, sprite boxes, background/border involvement).
//
// Pure after-the-fact arithmetic over frozen register values + byte-fetch
// callbacks: NO rendering/capture pipeline involvement and zero cost when no
// screen fail occurs. Game-agnostic by construction.
//
// Geometry constants mirror the Frodo SC raster pipeline (PAL):
//   canvas row r          ↔ raster line r + FIRST_DISP_LINE ($10)
//   text window top       ↔ ROW25_YSTART ($33) with DEN + RSEL=1
//   graphics x origin     ↔ COL40_XSTART ($20) with CSEL=1, plus XSCROLL
//   sprite box            ↔ x = MxX + 8 .. +24/48, y = MyY - 16 .. +21

#include <cstdint>
#include <string>

namespace revm::screenctx {

constexpr unsigned kCanvasWidth = 0x180;   // DISPLAY_X
constexpr unsigned kCanvasHeight = 0x110;  // DISPLAY_Y
constexpr unsigned kFirstDispLine = 0x10;
constexpr unsigned kRow25YStart = 0x33;
constexpr unsigned kCol40XStart = 0x20;

// Caps on emitted attribution detail.
constexpr size_t kMaxCells = 16;    // distinct video-matrix cells
constexpr size_t kMaxDiffProbe = 4 * 1024; // bg/border pixel probes per side

// One board's frozen VIC view at fail time (register values only).
struct Side {
	uint8_t d011 = 0;    // $D011 ctrl1: YSCROLL/RSEL/DEN/BMM/ECM
	uint8_t d016 = 0;    // $D016 ctrl2: XSCROLL/CSEL/MCM
	uint8_t d018 = 0;    // $D018 video matrix / char / bitmap base
	uint8_t dd00 = 0xff; // CIA2 PRA: bits 0-1 select the VIC bank (inverted)
	uint8_t d015 = 0;    // sprite enable
	uint8_t d017 = 0;    // sprite Y expansion
	uint8_t d01c = 0;    // sprite multicolor
	uint8_t d01d = 0;    // sprite X expansion
	uint8_t d020 = 0;    // border color
	uint8_t d021 = 0;    // background color 0
	uint16_t mx[8]{};    // sprite X incl $D010 MSB
	uint8_t my[8]{};     // sprite Y
};

// Memory access callback: absolute C64 bus address → byte for that board's
// own state. The host routes $D800-$DBEF to color RAM; video-matrix and
// sprite-pointer reads land in plain DRAM via the bank-decoded base.
using FetchByteFn = uint8_t (*)(const void * user, uint16_t addr);

// Pixel-diff probe on the two captured framebuffers (canvas coordinates):
// true when the pixel differs between main and twin; out_main / out_twin
// receive each side's palette-index value at that position.
using DiffAtFn = bool (*)(const void * user, unsigned x, unsigned y,
                          uint8_t & out_main, uint8_t & out_twin);

struct Params {
	Side main;
	Side twin;

	FetchByteFn main_fetch = nullptr;
	const void * main_user = nullptr;
	FetchByteFn twin_fetch = nullptr;
	const void * twin_user = nullptr;

	DiffAtFn diff_at = nullptr;
	const void * diff_user = nullptr;

	// Diff bounding box in canvas coordinates (inclusive).
	unsigned x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct Result {
	// True when relevant VIC mode/base fields disagree between boards — that
	// alone is the diagnosis; cells/sprites are skipped then.
	bool vic_config_drift = false;

	// Pre-serialized JSON fragments (verbatim values, always valid JSON):
	//   context_json — {"vic_config_drift":…,…} object
	//   cells_json   — [{cell,main,twin,col_main,col_twin},…] array ([] if none)
	//   sprites_json — [{sprite,ptr_addr,ptr_main,ptr_twin,x_main,x_twin,
	//                     y_main,y_twin,in_bbox},…] array ([] if none)
	std::string context_json;
	std::string cells_json;
	std::string sprites_json;

	bool background_involved = false;
	bool border_involved = false;

	// One human line naming the strongest signal (drift > cells > sprites >
	// colors > none).
	std::string summary;
};

Result Analyze(const Params & p);

} // namespace revm::screenctx
