// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "goldens/CompareReport.hpp"
#include "snapshot/ChipIo.hpp"
#include "util/Png.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#define REVM_LOG_MODULE "compare"
#include "util/Log.hpp"

namespace fs = std::filesystem;

namespace revm {

void IgnoreTally::LogLine(const char * module, const char * label) const {
	REVM_LOG_M(module, REVM_DEBUG, "%s: compared=%llu failed=%llu", label,
	           static_cast<unsigned long long>(compared),
	           static_cast<unsigned long long>(failed));
}

namespace {

bool excluded(const RamCompareMask & mask, uint16_t addr) {
	for (const auto & r : mask.Ranges()) {
		if (addr >= r.lo && addr <= r.hi) return true;
	}
	return false;
}

template <typename T>
void field_u8(FILE * out, const char * name, T e, T a) {
	if (e != a) {
		std::fprintf(out, "    %-12s  expected=$%02X  actual=$%02X\n", name,
		             unsigned(e) & 0xff, unsigned(a) & 0xff);
	}
}

template <typename T>
void field_u16(FILE * out, const char * name, T e, T a) {
	if (e != a) {
		std::fprintf(out, "    %-12s  expected=$%04X  actual=$%04X\n", name,
		             unsigned(e) & 0xffff, unsigned(a) & 0xffff);
	}
}

void field_bool(FILE * out, const char * name, bool e, bool a) {
	if (e != a) {
		std::fprintf(out, "    %-12s  expected=%s  actual=%s\n", name,
		             e ? "true" : "false", a ? "true" : "false");
	}
}

void print_vic_diff(FILE * out, const MOS6569State & e, const MOS6569State & a) {
	if (VicConfigEqual(e, a)) return;
	std::fprintf(out, "  VIC config diffs:\n");
	field_u8(out, "m0x", e.m0x, a.m0x);
	field_u8(out, "m0y", e.m0y, a.m0y);
	field_u8(out, "m1x", e.m1x, a.m1x);
	field_u8(out, "m1y", e.m1y, a.m1y);
	field_u8(out, "m2x", e.m2x, a.m2x);
	field_u8(out, "m2y", e.m2y, a.m2y);
	field_u8(out, "m3x", e.m3x, a.m3x);
	field_u8(out, "m3y", e.m3y, a.m3y);
	field_u8(out, "m4x", e.m4x, a.m4x);
	field_u8(out, "m4y", e.m4y, a.m4y);
	field_u8(out, "m5x", e.m5x, a.m5x);
	field_u8(out, "m5y", e.m5y, a.m5y);
	field_u8(out, "m6x", e.m6x, a.m6x);
	field_u8(out, "m6y", e.m6y, a.m6y);
	field_u8(out, "m7x", e.m7x, a.m7x);
	field_u8(out, "m7y", e.m7y, a.m7y);
	field_u8(out, "mx8", e.mx8, a.mx8);
	field_u8(out, "ctrl1", uint8_t(e.ctrl1 & 0x7f), uint8_t(a.ctrl1 & 0x7f));
	field_u8(out, "me", e.me, a.me);
	field_u8(out, "ctrl2", e.ctrl2, a.ctrl2);
	field_u8(out, "mye", e.mye, a.mye);
	field_u8(out, "vbase", e.vbase, a.vbase);
	field_u8(out, "irq_mask", e.irq_mask, a.irq_mask);
	field_u8(out, "mdp", e.mdp, a.mdp);
	field_u8(out, "mmc", e.mmc, a.mmc);
	field_u8(out, "mxe", e.mxe, a.mxe);
	field_u8(out, "ec", e.ec, a.ec);
	field_u8(out, "b0c", e.b0c, a.b0c);
	field_u8(out, "b1c", e.b1c, a.b1c);
	field_u8(out, "b2c", e.b2c, a.b2c);
	field_u8(out, "b3c", e.b3c, a.b3c);
	field_u8(out, "mm0", e.mm0, a.mm0);
	field_u8(out, "mm1", e.mm1, a.mm1);
	field_u8(out, "m0c", e.m0c, a.m0c);
	field_u8(out, "m1c", e.m1c, a.m1c);
	field_u8(out, "m2c", e.m2c, a.m2c);
	field_u8(out, "m3c", e.m3c, a.m3c);
	field_u8(out, "m4c", e.m4c, a.m4c);
	field_u8(out, "m5c", e.m5c, a.m5c);
	field_u8(out, "m6c", e.m6c, a.m6c);
	field_u8(out, "m7c", e.m7c, a.m7c);
	field_u16(out, "irq_raster", e.irq_raster, a.irq_raster);
}

void print_sid_diff(FILE * out, const MOS6581State & e, const MOS6581State & a) {
	if (SidConfigEqual(e, a)) return;
	std::fprintf(out, "  SID config diffs:\n");
	field_u8(out, "freq_lo_1", e.freq_lo_1, a.freq_lo_1);
	field_u8(out, "freq_hi_1", e.freq_hi_1, a.freq_hi_1);
	field_u8(out, "pw_lo_1", e.pw_lo_1, a.pw_lo_1);
	field_u8(out, "pw_hi_1", e.pw_hi_1, a.pw_hi_1);
	field_u8(out, "ctrl_1", e.ctrl_1, a.ctrl_1);
	field_u8(out, "AD_1", e.AD_1, a.AD_1);
	field_u8(out, "SR_1", e.SR_1, a.SR_1);
	field_u8(out, "freq_lo_2", e.freq_lo_2, a.freq_lo_2);
	field_u8(out, "freq_hi_2", e.freq_hi_2, a.freq_hi_2);
	field_u8(out, "pw_lo_2", e.pw_lo_2, a.pw_lo_2);
	field_u8(out, "pw_hi_2", e.pw_hi_2, a.pw_hi_2);
	field_u8(out, "ctrl_2", e.ctrl_2, a.ctrl_2);
	field_u8(out, "AD_2", e.AD_2, a.AD_2);
	field_u8(out, "SR_2", e.SR_2, a.SR_2);
	field_u8(out, "freq_lo_3", e.freq_lo_3, a.freq_lo_3);
	field_u8(out, "freq_hi_3", e.freq_hi_3, a.freq_hi_3);
	field_u8(out, "pw_lo_3", e.pw_lo_3, a.pw_lo_3);
	field_u8(out, "pw_hi_3", e.pw_hi_3, a.pw_hi_3);
	field_u8(out, "ctrl_3", e.ctrl_3, a.ctrl_3);
	field_u8(out, "AD_3", e.AD_3, a.AD_3);
	field_u8(out, "SR_3", e.SR_3, a.SR_3);
	field_u8(out, "fc_lo", e.fc_lo, a.fc_lo);
	field_u8(out, "fc_hi", e.fc_hi, a.fc_hi);
	field_u8(out, "res_filt", e.res_filt, a.res_filt);
	field_u8(out, "mode_vol", e.mode_vol, a.mode_vol);
	field_u8(out, "pot_x", e.pot_x, a.pot_x);
	field_u8(out, "pot_y", e.pot_y, a.pot_y);
}

void print_cia_diff(FILE * out, const char * label, const MOS6526State & e,
                    const MOS6526State & a, uint8_t pra_mask) {
	if (CiaConfigEqual(e, a, pra_mask)) return;
	std::fprintf(out, "  %s config diffs:\n", label);
	if (pra_mask)
		field_u8(out, "pra", uint8_t(e.pra & pra_mask), uint8_t(a.pra & pra_mask));
	field_u8(out, "ddra", e.ddra, a.ddra);
	field_u8(out, "ddrb", e.ddrb, a.ddrb);
	field_u8(out, "cra", e.cra, a.cra);
	field_u8(out, "crb", e.crb, a.crb);
	field_u16(out, "ta_latch", e.ta_latch, a.ta_latch);
	field_u16(out, "tb_latch", e.tb_latch, a.tb_latch);
	field_u8(out, "int_mask", e.int_mask, a.int_mask);
}

} // namespace

void PrintSidConfigDiff(FILE * out, const MOS6581State & expected,
                        const MOS6581State & actual) {
	print_sid_diff(out, expected, actual);
}

size_t CollectMemDiffs(const uint8_t * expected, const uint8_t * actual, size_t size,
                       const RamCompareMask & mask, size_t max_hits,
                       std::vector<MemDiffHit> & out) {
	out.clear();
	if (!expected || !actual) return 0;
	size_t total = 0;
	const size_t n = std::min(size, size_t(0x10000));
	for (size_t i = 0; i < n; ++i) {
		if (expected[i] == actual[i]) continue;
		const uint16_t addr = uint16_t(i);
		if (excluded(mask, addr)) continue;
		++total;
		if (out.size() < max_hits) {
			out.push_back({addr, expected[i], actual[i]});
		}
	}
	return total;
}

void PrintMemDiffs(FILE * out, const char * label, const std::vector<MemDiffHit> & hits,
                   size_t total) {
	if (total == 0) return;
	std::fprintf(out, "  %s: %zu byte(s) differ", label, total);
	if (!hits.empty() && hits.size() < total) {
		std::fprintf(out, " (showing first %zu)", hits.size());
	}
	std::fprintf(out, "\n");
	for (const auto & h : hits) {
		std::fprintf(out, "    $%04X  expected=$%02X  actual=$%02X\n", h.addr, h.expected,
		             h.actual);
	}
}

void PrintCpuDiff(FILE * out, const MOS6510State & e, const MOS6510State & a) {
	if (std::memcmp(&e, &a, sizeof(MOS6510State)) == 0) return;
	std::fprintf(out, "  CPU state diffs:\n");
	field_u8(out, "A", e.a, a.a);
	field_u8(out, "X", e.x, a.x);
	field_u8(out, "Y", e.y, a.y);
	field_u8(out, "P", e.p, a.p);
	field_u16(out, "PC", e.pc, a.pc);
	field_u16(out, "SP", e.sp, a.sp);
	field_u8(out, "ddr", e.ddr, a.ddr);
	field_u8(out, "pr", e.pr, a.pr);
	field_u8(out, "pr_out", e.pr_out, a.pr_out);
	field_bool(out, "irq_pending", e.irq_pending, a.irq_pending);
	field_bool(out, "nmi_pending", e.nmi_pending, a.nmi_pending);
	field_bool(out, "nmi_trig", e.nmi_triggered, a.nmi_triggered);
	field_bool(out, "instr_done", e.instruction_complete, a.instruction_complete);
	field_u8(out, "state", e.state, a.state);
	field_u8(out, "op", e.op, a.op);
	field_u16(out, "ar", e.ar, a.ar);
	field_u16(out, "ar2", e.ar2, a.ar2);
	field_u8(out, "rdbuf", e.rdbuf, a.rdbuf);
	field_u8(out, "irq_delay", e.irq_delay, a.irq_delay);
	field_u8(out, "irq_off_dly", e.irq_off_delay, a.irq_off_delay);
	field_u8(out, "nmi_delay", e.nmi_delay, a.nmi_delay);
	field_bool(out, "INT_VIC", e.int_line[0], a.int_line[0]);
	field_bool(out, "INT_CIA", e.int_line[1], a.int_line[1]);
	field_bool(out, "INT_NMI", e.int_line[2], a.int_line[2]);
}

void PrintChipConfigDiff(FILE * out,
                         const MOS6569State & vic_e, const MOS6569State & vic_a,
                         const MOS6581State & sid_e, const MOS6581State & sid_a,
                         const MOS6526State & cia1_e, const MOS6526State & cia1_a,
                         const MOS6526State & cia2_e, const MOS6526State & cia2_a,
                         bool compare_sid) {
	print_vic_diff(out, vic_e, vic_a);
	if (compare_sid) {
		print_sid_diff(out, sid_e, sid_a);
	} else {
		std::fprintf(out, "  SID compare: skipped\n");
	}
	print_cia_diff(out, "CIA1", cia1_e, cia1_a, 0);
	print_cia_diff(out, "CIA2", cia2_e, cia2_a, 0x03);
}

void PrintVicConfigDiff(FILE * out, const MOS6569State & expected,
                        const MOS6569State & actual) {
	print_vic_diff(out, expected, actual);
}

void PrintCiaConfigDiff(FILE * out, const char * label, const MOS6526State & expected,
                        const MOS6526State & actual, uint8_t pra_mask) {
	print_cia_diff(out, label, expected, actual, pra_mask);
}

void PrintScreenDiff(FILE * out, const ScreenSnapshot & expected,
                     const ScreenSnapshot & actual, size_t max_hits) {
	std::fprintf(out, "  screen diffs (%ux%u palette indices):\n",
	             ScreenSnapshot::kWidth, ScreenSnapshot::kHeight);
	size_t shown = 0;
	size_t total = 0;
	for (size_t i = 0; i < ScreenSnapshot::kBytes; ++i) {
		if (expected.pixels[i] == actual.pixels[i]) continue;
		++total;
		if (shown < max_hits) {
			const unsigned x = unsigned(i % ScreenSnapshot::kWidth);
			const unsigned y = unsigned(i / ScreenSnapshot::kWidth);
			std::fprintf(out,
			             "    (%u,%u)  expected=$%02X  actual=$%02X\n", x, y,
			             unsigned(expected.pixels[i]), unsigned(actual.pixels[i]));
			++shown;
		}
	}
	if (total > shown) {
		std::fprintf(out, "    … %zu more pixel(s)\n", total - shown);
	}
	std::fprintf(out, "  screen mismatch total: %zu pixel(s)\n", total);
}

void PrintFullCompareReport(FILE * out, uint32_t frame, uint32_t cycle, size_t snap_index,
                            const FullSnapshot & expected, const FullSnapshot & actual,
                            const RamCompareMask & ram_mask, bool compare_cpu,
                            bool chip_stream_mismatch, bool compare_sid) {
	std::fprintf(out,
		"---- playback FAIL detail  frame=%u cycle=%u (snap #%zu) ----\n"
		"  expected snap: cycle=%u frame=%u pc=$%04X\n"
		"  actual:        cycle=%u frame=%u pc=$%04X\n",
		frame, cycle, snap_index,
		expected.cycle, expected.frame, expected.cpu.pc,
		actual.cycle, actual.frame, actual.cpu.pc);

	std::vector<MemDiffHit> hits;
	size_t total = CollectMemDiffs(expected.ram, actual.ram, C64_RAM_SIZE, ram_mask, 32, hits);
	PrintMemDiffs(out, "RAM", hits, total);

	RamCompareMask no_excl;
	hits.clear();
	total = CollectMemDiffs(expected.color, actual.color, COLOR_RAM_SIZE, no_excl, 16, hits);
	PrintMemDiffs(out, "color RAM", hits, total);

	if (compare_cpu) {
		PrintCpuDiff(out, expected.cpu, actual.cpu);
	} else {
		std::fprintf(out, "  CPU compare: skipped\n");
	}

	PrintChipConfigDiff(out, expected.vic, actual.vic, expected.sid, actual.sid,
	                    expected.cia1, actual.cia1, expected.cia2, actual.cia2,
	                    compare_sid);
	if (chip_stream_mismatch) {
		std::fprintf(out, "  (chip I/O stream also mismatched — same config fields)\n");
	}
	std::fprintf(out, "---- end FAIL detail ----\n");
}

bool WriteFailDumpPair(const std::string & dir, uint32_t frame, size_t snap_index,
                       const FullSnapshot & expected, const FullSnapshot & actual,
                       const std::string & report_text, std::string & error) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		error = "cannot create dump dir: " + dir;
		return false;
	}

	char base[128];
	std::snprintf(base, sizeof(base), "fail_f%06u_s%zu", frame, snap_index);
	const fs::path root = fs::path(dir) / base;
	const std::string exp_path = root.string() + "_expected.bin";
	const std::string act_path = root.string() + "_actual.bin";
	const std::string rpt_path = root.string() + "_report.txt";

	{
		std::ofstream f(exp_path, std::ios::binary);
		if (!f) {
			error = "cannot write " + exp_path;
			return false;
		}
		f.write(reinterpret_cast<const char *>(&expected), sizeof(expected));
	}
	{
		std::ofstream f(act_path, std::ios::binary);
		if (!f) {
			error = "cannot write " + act_path;
			return false;
		}
		f.write(reinterpret_cast<const char *>(&actual), sizeof(actual));
	}
	{
		std::ofstream f(rpt_path);
		if (!f) {
			error = "cannot write " + rpt_path;
			return false;
		}
		f << report_text;
	}

	REVM_LOG(REVM_DEBUG, "dump-fail → %s_{expected,actual}.bin + _report.txt",
	             root.string().c_str());
	return true;
}

bool WriteScreenFailImages(const std::string & dir, uint32_t frame,
                           const ScreenSnapshot & expected,
                           const ScreenSnapshot & actual, std::string & error) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		error = "cannot create dump dir: " + dir;
		return false;
	}

	char base[128];
	std::snprintf(base, sizeof(base), "fail_f%06u", frame);
	const fs::path root = fs::path(dir) / base;
	const std::string exp_path = root.string() + "_expected.png";
	const std::string act_path = root.string() + "_actual.png";
	const std::string diff_path = root.string() + "_diff.png";

	if (!WriteScreenPng(exp_path, expected, error)) return false;
	if (!WriteScreenPng(act_path, actual, error)) return false;

	uint8_t pal[17 * 3];
	std::memcpy(pal, &kPeptoRgb[0][0], 16 * 3);
	pal[16 * 3 + 0] = 0xff;
	pal[16 * 3 + 1] = 0x00;
	pal[16 * 3 + 2] = 0xff;
	std::vector<uint8_t> diff_idx(ScreenSnapshot::kBytes);
	for (size_t i = 0; i < ScreenSnapshot::kBytes; ++i) {
		if (expected.pixels[i] == actual.pixels[i])
			diff_idx[i] = expected.pixels[i] & 0x0f;
		else
			diff_idx[i] = 16; // hot magenta — easy to spot next to Pepto colors.
	}
	if (!WritePngIndexed(diff_path, ScreenSnapshot::kWidth, ScreenSnapshot::kHeight,
	                     diff_idx.data(), pal, 17, error)) {
		return false;
	}

	REVM_LOG(REVM_DEBUG, "screen dump → %s_{expected,actual,diff}.png",
	             root.string().c_str());
	return true;
}

bool WriteScreenPng(const std::string & path, const ScreenSnapshot & screen,
                    std::string & error) {
	return WritePngPepto(path, ScreenSnapshot::kWidth, ScreenSnapshot::kHeight,
	                     screen.pixels, error);
}

} // namespace revm
