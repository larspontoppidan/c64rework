// Created  : 2026-08-24
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "cpumock/ScreenContext.hpp"

#include <cstdio>
#include <vector>

namespace revm::screenctx {

namespace {

std::string hex8(unsigned v) {
	char b[8];
	std::snprintf(b, sizeof b, "\"0x%02X\"", v & 0xffu);
	return b;
}

std::string hex16(unsigned v) {
	char b[12];
	std::snprintf(b, sizeof b, "\"0x%04X\"", v & 0xffffu);
	return b;
}

// Absolute VIC bases from $D018 + CIA2 $DD00 bank bits (inverted, low two).
struct VicMap {
	uint16_t bank = 0;
	uint16_t matrix = 0;
	uint16_t chars = 0;
	uint16_t bitmap = 0;

	static VicMap decode(const Side & s) {
		VicMap m;
		m.bank = uint16_t(0xC000 - (s.dd00 & 3u) * 0x4000);
		m.matrix = uint16_t(m.bank | ((s.d018 >> 4) << 10));
		m.chars = uint16_t(m.bank | ((s.d018 & 0x0Eu) << 10));
		m.bitmap = uint16_t(m.bank | ((s.d018 & 0x0Eu) << 11));
		return m;
	}
};

struct Mode {
	bool ecm = false, bmm = false, mcm = false;
	const char * name = "std_text";
	bool exact_text = true; // false → best-effort attribution only

	static Mode decode(const Side & s) {
		Mode m;
		m.ecm = s.d011 & 0x40;
		m.bmm = s.d011 & 0x20;
		m.mcm = s.d016 & 0x10;
		if (m.bmm)
			m.name = m.mcm ? "mc_bitmap" : "std_bitmap";
		else if (m.ecm)
			m.name = "ecm_text";
		else if (m.mcm)
			m.name = "mc_text";
		m.exact_text = !m.bmm && !m.ecm && !m.mcm;
		return m;
	}
};

} // namespace

Result Analyze(const Params & p) {
	Result r;
	r.context_json = "{\"vic_config_drift\":false}";

	// --- 1. VIC config drift check -------------------------------------
	const VicMap mm = VicMap::decode(p.main);
	const VicMap tm = VicMap::decode(p.twin);

	struct RegDiff {
		const char * name;
		unsigned main_v, twin_v;
	};
	std::vector<RegDiff> diffs;
	auto u8 = [&](const char * n, uint8_t a, uint8_t b) {
		if (a != b) diffs.push_back({n, a, b});
	};
	u8("d011", p.main.d011, p.twin.d011); // DEN/BMM/ECM/RSEL/YSCROLL
	u8("d016", p.main.d016, p.twin.d016); // MCM/CSEL/XSCROLL
	u8("d018", p.main.d018, p.twin.d018); // matrix/char/bitmap base
	u8("dd00", p.main.dd00, p.twin.dd00); // VIC bank select

	if (!diffs.empty()) {
		r.vic_config_drift = true;
		r.cells_json = "[]";
		r.sprites_json = "[]";

		std::string ctx = "{\"vic_config_drift\":true,\"regs\":[";
		for (size_t i = 0; i < diffs.size(); ++i) {
			if (i) ctx += ',';
			ctx += "{\"reg\":";
			ctx += diffs[i].name;
			ctx += ",\"main\":";
			// dd00 is a CIA reg but renders fine as hex either way.
			ctx += hex8(diffs[i].main_v);
			ctx += ",\"twin\":";
			ctx += hex8(diffs[i].twin_v);
			ctx += '}';
		}
		ctx += "],\"matrix_base_main\":";
		ctx += hex16(mm.matrix);
		ctx += ",\"matrix_base_twin\":";
		ctx += hex16(tm.matrix);
		ctx += '}';

		std::string line = "screen context: VIC config drift (";
		for (size_t i = 0; i < diffs.size(); ++i) {
			if (i) line += ' ';
			char b[48];
			std::snprintf(b, sizeof b, "%s main=$%02X twin=$%02X",
			              diffs[i].name, diffs[i].main_v, diffs[i].twin_v);
			line += b;
		}
		line += ')';

		r.context_json = std::move(ctx);
		r.summary = std::move(line);
		return r;
	}

	// --- Shared config (boards agree here) -----------------------------
	const Mode mode = Mode::decode(p.main);
	const unsigned x_scroll = p.main.d016 & 7u;
	const unsigned y_scroll = p.main.d011 & 7u;
	const bool rsel25 = p.main.d011 & 0x08u;
	const bool den = p.main.d011 & 0x10u;

	{
		std::string ctx = "{\"vic_config_drift\":false,\"mode\":\"";
		ctx += mode.name;
		ctx += "\",\"exact_text\":";
		ctx += mode.exact_text ? "true" : "false";
		ctx += ",\"den\":";
		ctx += den ? "true" : "false";
		ctx += ",\"rsel25\":";
		ctx += rsel25 ? "true" : "false";
		ctx += ",\"bank\":";
		ctx += hex16(mm.bank);
		ctx += ",\"matrix_base\":";
		ctx += hex16(mm.matrix);
		ctx += ",\"char_base\":";
		ctx += hex16(mm.chars);
		ctx += ",\"bitmap_base\":";
		ctx += hex16(mm.bitmap);
		ctx += ",\"x_scroll\":";
		ctx += std::to_string(x_scroll);
		ctx += ",\"y_scroll\":";
		ctx += std::to_string(y_scroll);
		ctx += '}';
		r.context_json = std::move(ctx);
	}

	// --- 2. Cell attribution (video-matrix indices of differing pixels)
	struct CellRec {
		unsigned cell = 0;
		uint8_t main_ch = 0, twin_ch = 0;
		uint8_t col_main = 0, col_twin = 0;
		bool approx = true;
		bool have_mem = false;
	};
	std::vector<CellRec> cells;

	if (p.diff_at && p.main_fetch && p.twin_fetch &&
	    p.x1 >= p.x0 && p.y1 >= p.y0 && p.x1 < kCanvasWidth &&
	    p.y1 < kCanvasHeight) {
		const int gfx_x0 = int(kCol40XStart) + int(x_scroll);
		const int text_top_raster =
			int(kRow25YStart) - int(rsel25 ? 0 : 4); // 24-row window shift
		const int first_char_raster = 0x30 + int(y_scroll);

		auto to_cell = [&](unsigned cx, unsigned cy, unsigned & idx,
		                   bool & approx) {
			const long raster = long(cy) + long(kFirstDispLine);
			const long col_px = long(cx) - gfx_x0;
			const long row_px = raster - (first_char_raster > text_top_raster
			                                  ? first_char_raster
			                                  : text_top_raster);
			if (col_px < 0 || row_px < 0) return false;
			const unsigned col = unsigned(col_px) / 8u;
			const unsigned row = unsigned(row_px) / 8u;
			approx = !mode.exact_text || !rsel25;
			if (col >= 40u || row >= 25u) return false;
			idx = row * 40u + col;
			return true;
		};

		std::vector<unsigned> seen;
		for (unsigned cy = p.y0; cy <= p.y1; ++cy) {
			for (unsigned cx = p.x0; cx <= p.x1; ++cx) {
				uint8_t pm = 0, pt = 0;
				if (!p.diff_at(p.diff_user, cx, cy, pm, pt)) continue;
				unsigned idx = 0;
				bool approx = false;
				if (!to_cell(cx, cy, idx, approx)) continue;
				bool dup = false;
				for (unsigned s : seen) dup = dup || (s == idx);
				if (!dup) {
					if (seen.size() >= kMaxCells) continue;
					seen.push_back(idx);
					CellRec rec;
					rec.cell = idx;
					rec.approx = approx;
					rec.main_ch = p.main_fetch(p.main_user,
					                           uint16_t(mm.matrix + idx));
					rec.twin_ch = p.twin_fetch(p.twin_user,
					                           uint16_t(tm.matrix + idx));
					rec.col_main = p.main_fetch(p.main_user,
					                            uint16_t(0xD800 + idx));
					rec.col_twin = p.twin_fetch(p.twin_user,
					                            uint16_t(0xD800 + idx));
					rec.have_mem = true;
					cells.push_back(rec);
				}
				if (seen.size() >= kMaxCells) break;
			}
			if (seen.size() >= kMaxCells) break;
		}
	}

	if (cells.empty()) {
		r.cells_json = "[]";
	} else {
		r.cells_json = "[";
		for (size_t i = 0; i < cells.size(); ++i) {
			const CellRec & c = cells[i];
			if (i) r.cells_json += ',';
			r.cells_json += "{\"cell\":";
			r.cells_json += std::to_string(c.cell);
			r.cells_json += ",\"main\":";
			r.cells_json += c.have_mem ? std::to_string(c.main_ch) : "null";
			r.cells_json += ",\"twin\":";
			r.cells_json += c.have_mem ? std::to_string(c.twin_ch) : "null";
			r.cells_json += ",\"col_main\":";
			r.cells_json += c.have_mem ? std::to_string(c.col_main) : "null";
			r.cells_json += ",\"col_twin\":";
			r.cells_json += c.have_mem ? std::to_string(c.col_twin) : "null";
			r.cells_json += ",\"approx\":";
			r.cells_json += c.approx ? "true" : "false";
			r.cells_json += '}';
		}
		r.cells_json += "]";
	}

	// --- 3. Sprite attribution -----------------------------------------
	struct SprRec {
		unsigned n = 0;
		uint8_t ptr_main = 0, ptr_twin = 0;
		uint16_t x_main = 0, x_twin = 0;
		uint8_t y_main = 0, y_twin = 0;
		bool in_bbox = false;
		bool enabled_both = false;
	};
	std::vector<SprRec> sprites;
	if (p.main_fetch && p.twin_fetch) {
		for (unsigned n = 0; n < 8; ++n) {
			const bool en_m = p.main.d015 & (1u << n);
			const bool en_t = p.twin.d015 & (1u << n);
			if (!en_m && !en_t) continue;
			SprRec sr;
			sr.n = n;
			sr.enabled_both = en_m && en_t;
			sr.ptr_main = p.main_fetch(p.main_user,
			                           uint16_t(mm.matrix + 0x3F8 + n));
			sr.ptr_twin = p.twin_fetch(p.twin_user,
			                           uint16_t(tm.matrix + 0x3F8 + n));
			sr.x_main = p.main.mx[n];
			sr.x_twin = p.twin.mx[n];
			sr.y_main = p.main.my[n];
			sr.y_twin = p.twin.my[n];
			// Box intersection against the diff bbox (canvas coordinates),
			// union of both boards' boxes when their positions differ.
			const int bx0a = int(sr.x_main) + 8, by0a = int(sr.y_main) - 16;
			const int bx0b = int(sr.x_twin) + 8, by0b = int(sr.y_twin) - 16;
			const int w = 24 * ((p.main.d01d | p.twin.d01d) & (1u << n) ? 2 : 1);
			const int h = 21 * ((p.main.d017 | p.twin.d017) & (1u << n) ? 2 : 1);
			const int bx0 = bx0a < bx0b ? bx0a : bx0b;
			const int by0 = by0a < by0b ? by0a : by0b;
			const int bx1 = (bx0a > bx0b ? bx0a : bx0b) + w - 1;
			const int by1 = (by0a > by0b ? by0a : by0b) + h - 1;
			const long px0 = long(p.x0), py0 = long(p.y0);
			const long px1 = long(p.x1), py1 = long(p.y1);
			sr.in_bbox = !(bx0 > px1 || bx1 < px0 || by0 > py1 || by1 < py0);
			sprites.push_back(sr);
		}
	}
	if (sprites.empty()) {
		r.sprites_json = "[]";
	} else {
		r.sprites_json = "[";
		for (size_t i = 0; i < sprites.size(); ++i) {
			const SprRec & s = sprites[i];
			if (i) r.sprites_json += ',';
			r.sprites_json += "{\"sprite\":";
			r.sprites_json += std::to_string(s.n);
			r.sprites_json += ",\"ptr_addr\":";
			r.sprites_json += hex16(mm.matrix + 0x3F8 + s.n);
			r.sprites_json += ",\"ptr_main\":";
			r.sprites_json += std::to_string(s.ptr_main);
			r.sprites_json += ",\"ptr_twin\":";
			r.sprites_json += std::to_string(s.ptr_twin);
			r.sprites_json += ",\"x_main\":";
			r.sprites_json += std::to_string(s.x_main);
			r.sprites_json += ",\"x_twin\":";
			r.sprites_json += std::to_string(s.x_twin);
			r.sprites_json += ",\"y_main\":";
			r.sprites_json += std::to_string(s.y_main);
			r.sprites_json += ",\"y_twin\":";
			r.sprites_json += std::to_string(s.y_twin);
			r.sprites_json += ",\"enabled_both\":";
			r.sprites_json += s.enabled_both ? "true" : "false";
			r.sprites_json += ",\"in_bbox\":";
			r.sprites_json += s.in_bbox ? "true" : "false";
			r.sprites_json += '}';
		}
		r.sprites_json += "]";
	}

	// --- 4. Background / border involvement ----------------------------
	if (p.diff_at) {
		size_t probed = 0;
		for (unsigned cy = p.y0; cy <= p.y1 && probed < kMaxDiffProbe; ++cy) {
			for (unsigned cx = p.x0;
			     cx <= p.x1 && probed < kMaxDiffProbe; ++cx) {
				uint8_t pm = 0, pt = 0;
				if (!p.diff_at(p.diff_user, cx, cy, pm, pt)) continue;
				++probed;
				if (pm == p.main.d021 || pt == p.twin.d021)
					r.background_involved = true;
				if (pm == p.main.d020 || pt == p.twin.d020)
					r.border_involved = true;
				if (r.background_involved && r.border_involved) {
					cy = p.y1 + 1; // both flags set — stop scanning
					break;
				}
			}
		}
	}

	// --- 5. Summary line (strongest signal first) ----------------------
	if (!cells.empty()) {
		char b[96];
		if (cells.size() == 1) {
			std::snprintf(b, sizeof b, "cell %u main=$%02X twin=$%02X",
			              cells[0].cell, cells[0].main_ch, cells[0].twin_ch);
		} else if (cells.back().cell - cells.front().cell ==
		           cells.size() - 1) {
			std::snprintf(b, sizeof b, "cells %u-%u main=$%02X twin=$%02X",
			              cells.front().cell, cells.back().cell,
			              cells.front().main_ch, cells.front().twin_ch);
		} else {
			std::snprintf(b, sizeof b, "cells %u..%u (%zu distinct)",
			              cells.front().cell, cells.back().cell,
			              cells.size());
		}
		r.summary = "screen context: ";
		r.summary += b;
		r.summary += " (";
		r.summary += mode.name;
		r.summary += ")";
	} else {
		bool band = false;
		for (const SprRec & s : sprites) band = band || s.in_bbox;
		for (const SprRec & s : sprites) {
			if (!s.in_bbox) continue;
			char b[112];
			std::snprintf(
				b, sizeof b,
				"screen context: band in sprite %u box, ptr main=$%02X "
				"twin=$%02X (%s)",
				s.n, s.ptr_main, s.ptr_twin, mode.name);
			r.summary = b;
			break;
		}
		if (!band) {
			if (r.background_involved || r.border_involved) {
				r.summary = "screen context: diff pixels in ";
				r.summary += r.background_involved ? "background" : "";
				r.summary += r.background_involved && r.border_involved
				                 ? "/"
				                 : "";
				r.summary += r.border_involved ? "border" : "";
				r.summary += " colors";
			} else {
				r.summary = "screen context: no cell/sprite attribution (";
				r.summary += mode.name;
				r.summary += ")";
			}
		}
	}

	return r;
}

} // namespace revm::screenctx
