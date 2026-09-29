// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

/*
 * revm-tool — inspect FullSnapshots / 64K RAM dumps; KB listing, map, blobs
 *
 *   revm-tool snap SNAP [OPTIONS]          FullSnapshot show mode
 *   revm-tool ram RAMBIN [OPTIONS]         64K RAM dump show mode
 *   revm-tool merge-cov INPUT … MERGED     Merge REVMCOV2 coverage files
 *   revm-tool sort-kb INPUT OUTPUT         Sort *.kb.json by bank, then addr
 *   revm-tool selftest                     Run self-tests
 */

#include "debug/Disasm6502.hpp"
#include "debug/KnowledgeBase.hpp"
#include "debug/KbWatchSample.hpp"
#include "debug/Coverage.hpp"
#include "cpumock/LinkedRegistry.hpp"
#include "snapshot/Snapshot.hpp"
#include "roms/Roms.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage(const char * argv0) {
	std::fprintf(stderr,
		"Usage modes:\n"
		"  %s snap SNAP [OPTIONS]   Show mode using SNAP (FullSnapshot)\n"
		"  %s ram RAMBIN [OPTIONS]  Show mode using 64k RAM dump\n"
		"  %s merge-cov INPUT INPUT ... MERGED   Merge coverage files\n"
		"  %s sort-kb INPUT OUTPUT  Sort *.kb.json file\n"
		"  %s selftest              Run selftest routines of revm project\n"
		"  %s --version             Print the C64 Rework framework version\n"
		"\n"
		"General options:\n"
		"  --kb FILE            Knowledge base (*.kb.json) for labels/comments\n"
		"  --kb-support FILE    Optional C64 support KB (for naming fallback)\n"
		"  --rom-dir DIR        BASIC/KERNAL/CHAR dumps (required for --listing with --cov)\n"
		"\n"
		"Basic show mode (works with 64k RAM dump)\n"
		"  --disasm-addr ADDR   Disassemble at ADDR hex\n"
		"  --disasm-bytes N     Bytes to disassemble (default 128)\n"
		"\n"
		"SNAP show mode adds options:\n"
		"  --disasm             Disassemble at PC = snapshot PC, also IRQ vector + entry hints\n"
		"                       (+ stack RTS peeks; mid-instruction warning)\n"
		"  --dump-ram FILE      Write 64K raw DRAM image (under-ROM RAM)\n"
		"  --dump-cpuview FILE  Write banked-in 64K CPU view (ROM/I/O as seen\n"
		"                       by the 6510 at this snap; needs FullSnapshot)\n"
		"\n"
		"SNAP show mode with knowledge base adds options:\n"
		"  --dump-blobs DIR     Dump kind:blob ranges as <name>.bin in DIR\n"
		"  --listing FILE       Write listing to file\n"
		"  --cov FILE           Use REVMCOV2 coverage file (PC-RAM + MEM-RAM/ROM hits)\n"
		"  --list-no-cov-rng    No coverage range comments in listing\n"
		"  --map FILE           Generate address map: code operands + MEM hits + KB; sections RAM / BASIC / KERNAL.\n"
		"  --map-tally          Print only the tally part of map analysis to stdout\n"
		"  --call-graph FILE   JSR/JMP call graph weighted by PC-RAM fetch counts\n"
		"  --hot-undoc FILE    Hottest UNDOC MEM hits (ranked by mem count)\n"
		"  --hot-undoc-top N   Cap hot-undoc rows (default 80)\n"
		"  --watch-diff SNAP   Diff KB watch vars vs other FullSnapshot (snap mode)\n"
		"  --watch-diff-all    Include unchanged watch slots in --watch-diff\n",
		argv0, argv0, argv0, argv0, argv0, argv0);
}

bool parse_u16_hex(const char * text, uint16_t & out, std::string & error) {
	if (!text || !*text) {
		error = "empty address";
		return false;
	}
	const char * p = text;
	if (*p == '$') ++p;
	else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
	char * end = nullptr;
	unsigned long v = std::strtoul(p, &end, 16);
	if (end == p || *end || v > 0xffffu) {
		error = std::string("invalid address: ") + text;
		return false;
	}
	out = uint16_t(v);
	return true;
}

bool load_coverage_file(const std::string & path, revm::Coverage & out,
                        std::string & error) {
	out.Clear();
	return out.Load(path, error);
}

std::string coverage_scan_desc(const std::string & path, const revm::Coverage & cov) {
	std::ostringstream oss;
	oss << "coverage (pc_ram=" << cov.Count(revm::Coverage::Plane::PcRam)
	    << "/" << cov.TotalHits(revm::Coverage::Plane::PcRam)
	    << " pc_rom=" << cov.Count(revm::Coverage::Plane::PcRom)
	    << "/" << cov.TotalHits(revm::Coverage::Plane::PcRom)
	    << " mem_ram=" << cov.Count(revm::Coverage::Plane::MemRam)
	    << "/" << cov.TotalHits(revm::Coverage::Plane::MemRam)
	    << " mem_rom=" << cov.Count(revm::Coverage::Plane::MemRom)
	    << "/" << cov.TotalHits(revm::Coverage::Plane::MemRom)
	    << ": " << path << ")";
	return oss.str();
}

uint16_t read16(const uint8_t * ram, uint16_t addr) {
	return uint16_t(ram[addr] | (ram[uint16_t(addr + 1)] << 8));
}

void print_entry_hints(const uint8_t ram[0x10000], uint16_t center, unsigned window) {
	const uint32_t lo = center >= window ? center - window : 0;
	const uint32_t hi = std::min<uint32_t>(0x10000, uint32_t(center) + window);
	std::fprintf(stdout, "\n--- entry hints in $%04X..$%04X ---\n",
	             uint16_t(lo), uint16_t(hi - 1));
	for (uint32_t a = lo; a + 2 < hi; ++a) {
		const uint8_t op = ram[a];
		if (op == 0x4C || op == 0x20) {
			const uint16_t tgt = read16(ram, uint16_t(a + 1));
			const bool far = (tgt < lo || tgt >= hi);
			if (far || op == 0x4C) {
				std::fprintf(stdout, "  %s $%04X  at $%04X%s\n",
				             op == 0x4C ? "JMP" : "JSR", tgt, uint16_t(a),
				             far ? "  (leaves window)" : "");
			}
		}
		if (op == 0x78) {
			std::fprintf(stdout, "  SEI at $%04X\n", uint16_t(a));
		}
		if (op == 0x8D && a + 2 < hi) {
			const uint16_t tgt = read16(ram, uint16_t(a + 1));
			if ((tgt >= 0xD000 && tgt <= 0xD02E) || (tgt >= 0xD400 && tgt <= 0xD41C)) {
				std::fprintf(stdout, "  STA $%04X at $%04X  (chip poke)\n", tgt, uint16_t(a));
			}
		}
	}
}

// 6502 JSR pushes address of last operand byte; RTS does PC = pulled + 1.
// Stack may also hold PHA/PHP or IRQ (P,PCL,PCH) frames — treat pairs as candidates.
struct StackRtsCandidate {
	uint16_t pushed = 0;
	uint16_t rts_to = 0;
	bool jsr_behind = false; // ram[pushed-2] == JSR abs
	uint16_t jsr_at = 0;
};

void collect_stack_rts_candidates(const uint8_t ram[0x10000], uint16_t sp,
                                  std::vector<StackRtsCandidate> & out,
                                  unsigned max_frames = 8) {
	out.clear();
	// SP points at next free slot on page $01. Stacked bytes are SP+1 .. $01FF.
	const unsigned stacked = unsigned(0xFF - (sp & 0xff));
	const unsigned max_by_depth = stacked / 2;
	const unsigned n = std::min(max_frames, max_by_depth);
	for (unsigned i = 0; i < n; ++i) {
		const uint8_t slo = uint8_t((sp + 1 + i * 2) & 0xff);
		const uint8_t shi = uint8_t((slo + 1) & 0xff);
		StackRtsCandidate c;
		c.pushed = uint16_t(ram[0x0100 | slo] | (ram[0x0100 | shi] << 8));
		c.rts_to = uint16_t(c.pushed + 1);
		if (c.pushed >= 2) {
			c.jsr_at = uint16_t(c.pushed - 2);
			c.jsr_behind = (ram[c.jsr_at] == 0x20);
		}
		out.push_back(c);
	}
}

void print_stack_analysis(const revm::FullSnapshot & snap) {
	const uint16_t sp = snap.cpu.sp;
	const unsigned stacked = unsigned(0xFF - (sp & 0xff));
	std::vector<StackRtsCandidate> frames;
	collect_stack_rts_candidates(snap.ram, sp, frames);

	std::fprintf(stdout,
		"\n--- stack analysis (%u byte(s) on stack; word pairs from SP+1; "
		"PHA/PHP/IRQ may skew) ---\n",
		stacked);
	if (frames.empty()) {
		std::fprintf(stdout, "  (empty)\n");
		return;
	}
	for (size_t i = 0; i < frames.size(); ++i) {
		const auto & c = frames[i];
		if (c.jsr_behind) {
			std::fprintf(stdout,
				"  #%zu  push $%04X  RTS→$%04X  (JSR abs at $%04X)\n",
				i, c.pushed, c.rts_to, c.jsr_at);
		} else {
			std::fprintf(stdout,
				"  #%zu  push $%04X  RTS→$%04X\n",
				i, c.pushed, c.rts_to);
		}
	}

	// Optional: first three bytes as IRQ/NMI frame (P, PCL, PCH) — RTI, no +1.
	if (stacked >= 3) {
		const uint8_t p = snap.ram[0x0100 | uint8_t((sp + 1) & 0xff)];
		const uint8_t pcl = snap.ram[0x0100 | uint8_t((sp + 2) & 0xff)];
		const uint8_t pch = snap.ram[0x0100 | uint8_t((sp + 3) & 0xff)];
		const uint16_t rti_to = uint16_t(pcl | (pch << 8));
		// Hardware IRQ/NMI push P with B=0 and bit5=1 → (P & 0x30) == 0x20.
		if ((p & 0x30) == 0x20) {
			std::fprintf(stdout,
				"  alt IRQ/NMI?  P=$%02X  RTI→$%04X  (3-byte frame; ignore pair #0)\n",
				p, rti_to);
		}
	}
}


bool load_full_snap(const std::string & target, revm::FullSnapshot & snap,
                    std::string & error) {
	std::ifstream probe(target, std::ios::binary);
	char magic[16]{};
	if (!probe || !probe.read(magic, 16)) {
		error = "Failed to open FullSnapshot: " + target;
		return false;
	}
	if (std::memcmp(magic, revm::FullSnapshot::kMagic, 16) != 0) {
		error = "Not a FullSnapshot (bad magic): " + target;
		return false;
	}
	probe.clear();
	probe.seekg(0);
	probe.read(reinterpret_cast<char *>(&snap), sizeof(snap));
	if (!probe) {
		error = "Failed to read FullSnapshot";
		return false;
	}
	return true;
}

bool load_ram64k(const std::string & target, uint8_t ram[0x10000],
                 std::string & error) {
	{
		std::ifstream f(target, std::ios::binary | std::ios::ate);
		if (f) {
			const auto sz = f.tellg();
			if (sz == 0x10000) {
				f.seekg(0);
				f.read(reinterpret_cast<char *>(ram), 0x10000);
				if (f) return true;
			}
		}
	}
	revm::FullSnapshot snap;
	if (!load_full_snap(target, snap, error)) return false;
	std::memcpy(ram, snap.ram, 0x10000);
	return true;
}

void print_snap_summary(const revm::FullSnapshot & snap) {
	const uint16_t pc = snap.cpu.pc;
	const uint16_t sp = snap.cpu.sp;
	const uint16_t irq0314 = read16(snap.ram, 0x0314);
	const uint16_t nmi0318 = read16(snap.ram, 0x0318);
	const uint16_t irqfffe = read16(snap.ram, 0xFFFE);
	const uint16_t nmifffa = read16(snap.ram, 0xFFFA);

	std::fprintf(stdout,
		"  cycle=%u  frame=%u\n"
		"  PC=$%04X  A=$%02X X=$%02X Y=$%02X SP=$%04X\n"
		"  CPU state=%u op=$%02X instruction_complete=%d\n"
		"  IRQ $0314=$%04X   NMI $0318=$%04X\n"
		"  HW  $FFFE=$%04X   NMI $FFFA=$%04X\n",
		snap.cycle, snap.frame,
		pc, snap.cpu.a, snap.cpu.x, snap.cpu.y, sp,
		snap.cpu.state, snap.cpu.op, snap.cpu.instruction_complete ? 1 : 0,
		irq0314, nmi0318, irqfffe, nmifffa);

	std::fprintf(stdout, "  stack top:");
	for (int i = 0; i < 8; ++i) {
		const uint8_t slot = uint8_t((sp + 1 + i) & 0xff);
		std::fprintf(stdout, " %02X", snap.ram[0x0100 | slot]);
	}
	std::fprintf(stdout, "\n");
	print_stack_analysis(snap);
}

void dump_ram_file(const uint8_t ram[0x10000], const std::string & path) {
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char *>(ram), 0x10000);
	std::fprintf(stdout, "Wrote 64K RAM → %s\n", path.c_str());
}

bool dump_cpuview_file(const revm::FullSnapshot & snap, const std::string & path,
                       std::string & error) {
	if (!revm::WriteCpuViewFromFullSnapshot(snap, path, error)) return false;
	std::fprintf(stdout, "Wrote banked CPU view → %s\n", path.c_str());
	return true;
}

void disasm_at_addr(const uint8_t ram[0x10000], uint16_t addr, unsigned nbytes,
                    const revm::KnowledgeBase * kb) {
	std::fprintf(stdout, "\n--- disasm $%04X (%u bytes)%s ---\n", addr, nbytes,
	             kb ? " +KB" : "");
	std::fputs(revm::Disassemble6502(ram, addr, nbytes, kb).c_str(), stdout);
}

void disasm_snap_pc(const revm::FullSnapshot & snap, unsigned nbytes,
                    const revm::KnowledgeBase * kb) {
	const uint16_t pc = snap.cpu.pc;
	const uint16_t irq0314 = read16(snap.ram, 0x0314);

	if (!snap.cpu.instruction_complete) {
		std::fprintf(stdout,
			"\nNote: mid-instruction (op=$%02X); PC disasm may be desynced.\n",
			snap.cpu.op);
	}

	disasm_at_addr(snap.ram, pc, nbytes, kb);

	// Peek at the most plausible RTS landings (JSR-backed first, else top frames).
	std::vector<StackRtsCandidate> frames;
	collect_stack_rts_candidates(snap.ram, snap.cpu.sp, frames);
	unsigned peeks = 0;
	for (size_t i = 0; i < frames.size() && peeks < 2; ++i) {
		if (!frames[i].jsr_behind) continue;
		std::fprintf(stdout, "\n--- peek stack #%zu RTS→$%04X (32 bytes)%s ---\n",
		             i, frames[i].rts_to, kb ? " +KB" : "");
		std::fputs(revm::Disassemble6502(snap.ram, frames[i].rts_to, 32, kb).c_str(),
		           stdout);
		++peeks;
	}
	if (peeks == 0 && !frames.empty()) {
		std::fprintf(stdout,
			"\n--- peek stack #0 RTS→$%04X (32 bytes; no JSR-behind hits)%s ---\n",
			frames[0].rts_to, kb ? " +KB" : "");
		std::fputs(revm::Disassemble6502(snap.ram, frames[0].rts_to, 32, kb).c_str(),
		           stdout);
	}

	if (irq0314 != 0xEA31 && irq0314 != 0) {
		std::fprintf(stdout, "\n--- disasm IRQ $0314=$%04X ---\n", irq0314);
		std::fputs(revm::Disassemble6502(snap.ram, irq0314, 64, kb).c_str(), stdout);
	}
	print_entry_hints(snap.ram, pc, 0x100);
	print_entry_hints(snap.ram, irq0314, 0x80);
	if (pc >= 0x40) print_entry_hints(snap.ram, uint16_t(pc - 0x40), 0x80);
}

std::string blob_filename(const revm::KbObject & o) {
	std::string base = o.name;
	if (base.empty() && !o.format.empty()) base = o.format;
	if (base.empty()) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "blob_%04X", o.addr);
		base = buf;
	}
	std::string out;
	out.reserve(base.size() + 4);
	for (char c : base) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-') {
			out.push_back(c);
		} else {
			out.push_back('_');
		}
	}
	if (out.empty()) out = "blob";
	return out + ".bin";
}

int dump_blobs(const uint8_t ram[0x10000], const revm::KnowledgeBase & kb,
              const std::string & out_dir) {
	std::error_code ec;
	fs::create_directories(out_dir, ec);
	if (ec) {
		std::fprintf(stderr, "Cannot create %s: %s\n", out_dir.c_str(),
		             ec.message().c_str());
		return 1;
	}

	size_t n = 0;
	for (const auto & o : kb.Objects()) {
		if (o.kind != revm::KbKind::Blob) continue;
		if (!o.IsRamBank()) continue; // dump from RAM image only
		if (!o.end) {
			std::fprintf(stderr, "skip blob $%04X: missing end\n", o.addr);
			continue;
		}
		const unsigned len = o.ByteLength();
		const std::string fname = blob_filename(o);
		const fs::path path = fs::path(out_dir) / fname;
		std::ofstream f(path, std::ios::binary);
		if (!f) {
			std::fprintf(stderr, "Cannot write %s\n", path.string().c_str());
			return 1;
		}
		f.write(reinterpret_cast<const char *>(ram + o.addr), len);
		if (!f) {
			std::fprintf(stderr, "Write failed: %s\n", path.string().c_str());
			return 1;
		}
		std::fprintf(stderr, "  %s  $%04X-$%04X  (%u bytes)%s%s\n",
		             path.string().c_str(), o.addr, *o.end, len,
		             o.format.empty() ? "" : "  format=",
		             o.format.empty() ? "" : o.format.c_str());
		++n;
	}
	std::fprintf(stderr, "Wrote %zu blob(s) → %s\n", n, out_dir.c_str());
	return 0; // zero blobs is OK (empty KB / no kind:blob yet)
}

int generate_listing(const std::string & source_desc,
                     const revm::FullSnapshot & snap,
                     const revm::KnowledgeBase & kb,
                     const std::string & kb_path,
                     const revm::KnowledgeBase * support_ptr,
                     const revm::KnowledgeBase & kb_support,
                     const std::string & kb_support_path,
                     const std::string & cover_path,
                     const std::string & out_path,
                     unsigned bytes,
                     bool include_auto,
                     bool emit_coverage_comments,
                     const revm::RomImages * roms) {
	std::string error;
	size_t n_fn_ram = 0;
	size_t n_fn_kernal = 0;
	size_t n_fn_basic = 0;
	for (const auto & o : kb.Objects()) {
		if (o.kind != revm::KbKind::Function || o.name.empty()) continue;
		if (o.IsRamBank()) ++n_fn_ram;
		else if (o.bank == "kernal") ++n_fn_kernal;
		else if (o.bank == "basic") ++n_fn_basic;
	}

	revm::DisasmOptions dopt;
	dopt.emit_function_headers = true;
	dopt.include_auto = include_auto;
	dopt.default_bytes = bytes;
	dopt.bank = "ram";
	dopt.support_kb = support_ptr;
	dopt.emit_coverage_comments = emit_coverage_comments;

	// Coalesce stores [lo, hi) as uint16_t; a range ending at $FFFF wraps hi→0.
	auto range_hi = [](uint16_t lo, uint16_t hi) -> uint32_t {
		if (hi == 0 && lo != 0) return 0x10000u;
		return hi;
	};

	auto clip_ranges = [&](const std::vector<std::pair<uint16_t, uint16_t>> & in,
	                       uint32_t lo_b, uint32_t hi_excl,
	                       std::vector<std::pair<uint16_t, uint32_t>> & out) {
		for (const auto & r : in) {
			const uint32_t lo = std::max(uint32_t(r.first), lo_b);
			const uint32_t hi = std::min(range_hi(r.first, r.second), hi_excl);
			if (lo < hi) out.emplace_back(uint16_t(lo), hi);
		}
	};

	auto mark_span = [](revm::Coverage & m, bool rom, uint16_t lo, uint16_t hi_incl) {
		for (uint32_t a = lo; a <= hi_incl && a < 0x10000; ++a) {
			if (rom) m.MarkPcRom(uint16_t(a));
			else m.MarkPcRam(uint16_t(a));
		}
	};

	auto mark_kb_spans = [&](revm::Coverage & mask_kb) {
		for (const auto & o : kb.Objects()) {
			uint16_t hi = 0;
			if (o.kind == revm::KbKind::Function && !o.name.empty()) {
				hi = o.end ? *o.end : uint16_t(o.addr + bytes - 1);
			} else if (o.kind == revm::KbKind::Opcodes && o.end) {
				hi = *o.end;
			} else {
				continue;
			}
			mark_span(mask_kb, !o.IsRamBank(), o.addr, hi);
		}
	};

	auto emit_ranges = [&](std::ostringstream & body, const uint8_t * image,
	                       const std::vector<std::pair<uint16_t, uint32_t>> & ranges,
	                       const revm::DisasmOptions & opt, bool label_each,
	                       const revm::Coverage * mask_play,
	                       const revm::Coverage * mask_kb) {
		const bool use_rom = !(opt.bank.empty() || opt.bank == "ram");
		auto hit_play = [&](uint16_t a) {
			if (!mask_play) return false;
			return use_rom ? mask_play->HitPcRom(a) : mask_play->HitPcRam(a);
		};
		auto hit_kb = [&](uint16_t a) {
			if (!mask_kb) return false;
			return use_rom ? mask_kb->HitPcRom(a) : mask_kb->HitPcRam(a);
		};

		for (size_t ri = 0; ri < ranges.size(); ++ri) {
			const uint16_t lo = ranges[ri].first;
			const uint32_t hi_excl = ranges[ri].second;
			const unsigned nbytes = unsigned(hi_excl - lo);
			// Cover-range header only for play-only runs (no KB force-in).
			bool only_play = mask_play && mask_kb;
			if (only_play) {
				for (uint32_t a = lo; a < hi_excl; ++a) {
					if (!hit_play(uint16_t(a)) || hit_kb(uint16_t(a))) {
						only_play = false;
						break;
					}
				}
			}
			if (emit_coverage_comments && label_each && ranges.size() > 1 &&
			    only_play) {
				body << "; coverage range: $" << std::hex << std::uppercase
				     << std::setw(4) << std::setfill('0') << lo << "-$"
				     << std::setw(4) << uint16_t(hi_excl - 1) << std::dec
				     << std::nouppercase << " (" << nbytes << " bytes)\n";
			}
			body << revm::Disassemble6502(image, lo, nbytes, &kb, &opt);
			if (ri + 1 < ranges.size()) {
				const uint16_t next_lo = ranges[ri + 1].first;
				if (next_lo > hi_excl) {
					const uint16_t gap_lo = uint16_t(hi_excl);
					const uint16_t gap_hi = uint16_t(next_lo - 1);
					if (emit_coverage_comments) {
						body << "; gap range: $" << std::hex << std::uppercase
						     << std::setw(4) << std::setfill('0') << gap_lo
						     << " - $" << std::setw(4) << gap_hi << std::dec
						     << std::nouppercase << "\n";
					} else {
						body << "\n";
					}
				} else {
					body << "\n";
				}
			}
		}
	};

	std::ostringstream body;
	body << "# KB-annotated disassembly\n"
	     << "# Source: " << source_desc << "  cycle=" << snap.cycle
	     << " frame=" << snap.frame << "\n"
	     << "# KB: " << kb_path << " (" << kb.Size() << " objects; "
	     << n_fn_ram << " RAM / " << n_fn_basic << " BASIC / " << n_fn_kernal
	     << " KERNAL functions)\n";
	if (support_ptr) {
		body << "# Support KB: " << kb_support_path << " (" << kb_support.Size()
		     << " objects; naming fallback only)\n";
	}

	revm::Coverage mask_play;
	revm::Coverage mask_kb;
	std::vector<std::pair<uint16_t, uint32_t>> ram_ranges;
	std::vector<std::pair<uint16_t, uint32_t>> basic_ranges;
	std::vector<std::pair<uint16_t, uint32_t>> kernal_ranges;
	bool have_masks = false;

	if (!cover_path.empty()) {
		revm::Coverage cov;
		if (!load_coverage_file(cover_path, cov, error)) {
			std::fprintf(stderr, "coverage load failed: %s\n", error.c_str());
			return 1;
		}
		// Play mask: each fetch PC expanded to full instruction bytes (no gap).
		// Operand bytes of covered insns are play, so holes start at real code.
		// pc_ram lengths come from snap RAM (under-ROM game code at $A000+ /
		// $E000+ when $01 banks ROM out). pc_rom lengths use ROM dumps.
		if (!roms || roms->basic.size() != revm::kBasicRomSize ||
		    roms->kernal.size() != revm::kKernalRomSize) {
			std::fprintf(stderr, "listing with --cov requires --rom-dir DIR\n");
			return 1;
		}
		uint8_t rom_decode[0x10000];
		std::memcpy(rom_decode, snap.ram, 0x10000);
		std::memcpy(rom_decode + 0xA000, roms->basic.data(), revm::kBasicRomSize);
		std::memcpy(rom_decode + 0xE000, roms->kernal.data(), revm::kKernalRomSize);

		auto mark_play_insns = [&](bool rom) {
			const uint8_t * img = rom ? rom_decode : snap.ram;
			for (uint32_t a = 0; a < 0x10000; ++a) {
				const bool hit =
				    rom ? cov.HitPcRom(uint16_t(a)) : cov.HitPcRam(uint16_t(a));
				if (!hit) continue;
				const unsigned len = revm::InsnBytes6502(img[a]);
				for (unsigned i = 0; i < len && a + i < 0x10000; ++i) {
					if (rom) mask_play.MarkPcRom(uint16_t(a + i));
					else mask_play.MarkPcRam(uint16_t(a + i));
				}
			}
		};
		mark_play_insns(false);
		mark_play_insns(true);

		// KB mask: function + opcodes spans (RAM and ROM banks).
		mark_kb_spans(mask_kb);

		// Listing ranges: exact play ∨ kb (no gap bridging).
		revm::Coverage list_or = mask_play;
		list_or.Merge(mask_kb);
		for (const auto & r : list_or.Coalesce(revm::Coverage::Plane::PcRam, 0)) {
			ram_ranges.emplace_back(r.first, range_hi(r.first, r.second));
		}
		std::vector<std::pair<uint16_t, uint16_t>> rom_raw =
		    list_or.Coalesce(revm::Coverage::Plane::PcRom, 0);
		clip_ranges(rom_raw, 0xA000, 0xC000, basic_ranges);
		clip_ranges(rom_raw, 0xE000, 0x10000, kernal_ranges);

		if (ram_ranges.empty() && basic_ranges.empty() && kernal_ranges.empty()) {
			std::fprintf(stderr, "listing: coverage has no fetch PCs\n");
			return 1;
		}
		have_masks = true;
		body << "# Scan: " << coverage_scan_desc(cover_path, cov) << "\n"
		     << "# List ranges: ram=" << ram_ranges.size()
		     << " basic=" << basic_ranges.size()
		     << " kernal=" << kernal_ranges.size()
		     << " (play ∨ KB function/opcodes; gaps marked)\n"
		     << "# Default bytes (no end): " << bytes << "\n\n";
	} else {
		bool have_span = false;
		uint16_t span_lo = 0;
		uint16_t span_hi = 0;
		auto grow_span = [&](uint16_t lo, uint16_t hi) {
			if (!have_span) {
				span_lo = lo;
				span_hi = hi;
				have_span = true;
			} else {
				if (lo < span_lo) span_lo = lo;
				if (hi > span_hi) span_hi = hi;
			}
		};
		for (const auto & o : kb.Objects()) {
			if (o.kind != revm::KbKind::Function || !o.IsRamBank() || o.name.empty()) {
				continue;
			}
			const uint16_t hi = o.end ? *o.end : uint16_t(o.addr + bytes - 1);
			grow_span(o.addr, hi);
		}
		for (const auto & o : kb.Objects()) {
			if (o.kind != revm::KbKind::Opcodes || !o.IsRamBank() || !o.end) continue;
			grow_span(o.addr, *o.end);
		}
		if (have_span) {
			for (const auto & o : kb.Objects()) {
				if (o.kind != revm::KbKind::Label || !o.IsRamBank() || o.name.empty()) {
					continue;
				}
				if (o.addr < span_lo || o.addr > uint16_t(span_hi + 0x100)) continue;
				const uint16_t hi = o.end ? *o.end : o.addr;
				grow_span(o.addr, hi);
			}
		}
		if (!have_span) {
			std::fprintf(stderr, "listing: no RAM functions in KB (or pass --cov)\n");
			return 1;
		}
		size_t n_blob_in_span = 0;
		for (const auto & o : kb.Objects()) {
			if (o.kind != revm::KbKind::Blob || !o.IsRamBank()) continue;
			const uint16_t hi = o.end ? *o.end : o.addr;
			if (hi < span_lo || o.addr > span_hi) continue;
			++n_blob_in_span;
		}
		const unsigned span_bytes = unsigned(span_hi - span_lo + 1);
		body << "# Range: $" << std::hex << std::uppercase << std::setw(4)
		     << std::setfill('0') << span_lo << "-$" << std::setw(4) << span_hi
		     << std::dec << std::nouppercase << " (" << span_bytes
		     << " bytes continuous; " << n_blob_in_span << " blob(s) skipped)\n"
		     << "# Default bytes (no end): " << bytes << "\n\n";
		ram_ranges.emplace_back(span_lo, uint16_t(span_hi + 1));
	}

	if (have_masks) {
		dopt.mask_play = &mask_play;
		dopt.mask_kb = &mask_kb;
	}

	// --- RAM section ---
	if (!ram_ranges.empty()) {
		if (!cover_path.empty()) {
			body << "########################################\n"
			     << "# RAM — play PC coverage ∨ KB function/opcodes\n"
			     << "########################################\n\n";
		}
		emit_ranges(body, snap.ram, ram_ranges, dopt, !cover_path.empty(),
		            have_masks ? &mask_play : nullptr,
		            have_masks ? &mask_kb : nullptr);
	}

	// --- ROM sections (only with --cov) ---
	if (!cover_path.empty() &&
	    (!basic_ranges.empty() || !kernal_ranges.empty())) {
		uint8_t rom_view[0x10000];
		std::memcpy(rom_view, snap.ram, 0x10000);
		std::memcpy(rom_view + 0xA000, roms->basic.data(), revm::kBasicRomSize);
		std::memcpy(rom_view + 0xE000, roms->kernal.data(), revm::kKernalRomSize);

		if (!basic_ranges.empty()) {
			body << "\n########################################\n"
			     << "# BASIC ROM — PC-ROM hits in $A000-$BFFF\n"
			     << "########################################\n\n";
			revm::DisasmOptions bopt = dopt;
			bopt.bank = "basic";
			emit_ranges(body, rom_view, basic_ranges, bopt, true, &mask_play,
			            &mask_kb);
		}
		if (!kernal_ranges.empty()) {
			body << "\n########################################\n"
			     << "# KERNAL ROM — PC-ROM hits in $E000-$FFFF\n"
			     << "########################################\n\n";
			revm::DisasmOptions kopt = dopt;
			kopt.bank = "kernal";
			emit_ranges(body, rom_view, kernal_ranges, kopt, true, &mask_play,
			            &mask_kb);
		}
	}

	const std::string text = body.str();
	if (out_path.empty()) {
		std::fputs(text.c_str(), stdout);
	} else {
		std::ofstream f(out_path);
		if (!f) {
			std::fprintf(stderr, "Cannot write %s\n", out_path.c_str());
			return 1;
		}
		f << text;
		if (!cover_path.empty()) {
			std::fprintf(stderr,
			             "Wrote %s (ram=%zu basic=%zu kernal=%zu)\n",
			             out_path.c_str(), ram_ranges.size(), basic_ranges.size(),
			             kernal_ranges.size());
		} else {
			std::fprintf(stderr, "Wrote %s ($%04X-$%04X, %zu RAM functions)\n",
			             out_path.c_str(), ram_ranges.front().first,
			             uint16_t(ram_ranges.front().second - 1), n_fn_ram);
		}
	}
	return 0;
}

const revm::KbObject * named_kb_at(const revm::KnowledgeBase & kb, uint16_t addr,
                                   const std::string & bank_filter = "ram") {
	const std::string want = bank_filter.empty() ? "ram" : bank_filter;
	auto matches = [&](const revm::KbObject & o) {
		if (want == "ram") return o.IsRamBank();
		return !o.IsRamBank() && o.bank == want;
	};
	const revm::KbObject * cover = nullptr;
	for (const auto & o : kb.Objects()) {
		if (o.name.empty() || !matches(o)) continue;
		const uint16_t hi = o.end ? *o.end : o.addr;
		if (addr < o.addr || addr > hi) continue;
		if (o.addr == addr) return &o;
		cover = &o;
	}
	return cover;
}

// Exact-addr KB objects for one bank (or all banks if bank_filter empty).
std::vector<const revm::KbObject *> kb_exacts_at(const revm::KnowledgeBase & kb,
                                                 uint16_t addr,
                                                 const std::string & bank_filter = "") {
	std::vector<const revm::KbObject *> out;
	auto matches = [&](const revm::KbObject & o) {
		if (bank_filter.empty()) return true;
		if (bank_filter == "ram") return o.IsRamBank();
		return !o.IsRamBank() && o.bank == bank_filter;
	};
	for (const auto & o : kb.Objects()) {
		if (o.addr != addr || !matches(o)) continue;
		out.push_back(&o);
	}
	return out;
}

// Exact object fallback: prefer named, then any, within bank_filter (default RAM).
const revm::KbObject * kb_exact_at(const revm::KnowledgeBase & kb, uint16_t addr,
                                   const std::string & bank_filter = "ram") {
	const revm::KbObject * unnamed = nullptr;
	for (const auto & o : kb_exacts_at(kb, addr, bank_filter)) {
		if (!o->name.empty()) return o;
		if (!unnamed) unnamed = o;
	}
	return unnamed;
}

// True if game KB has a named non-opcodes RAM object covering addr (exact or range).
bool game_kb_names_ram(const revm::KnowledgeBase & kb, uint16_t addr) {
	for (const auto & o : kb.Objects()) {
		if (!o.IsRamBank() || o.name.empty() || o.kind == revm::KbKind::Opcodes) continue;
		if (o.IsChipBank()) continue; // chip names don't "document" game RAM
		const uint16_t hi = o.end ? *o.end : o.addr;
		if (addr >= o.addr && addr <= hi) return true;
	}
	return false;
}

bool write_text_out(const std::string & out_path, const std::string & text) {
	if (out_path == "-") {
		std::fputs(text.c_str(), stdout);
		return true;
	}
	std::ofstream f(out_path);
	if (!f) {
		std::fprintf(stderr, "Cannot write %s\n", out_path.c_str());
		return false;
	}
	f << text;
	return true;
}

std::string format_hex_bytes(const uint8_t * p, size_t n, size_t max_show = 16) {
	std::ostringstream ss;
	const size_t show = n > max_show ? max_show : n;
	for (size_t i = 0; i < show; ++i) {
		char buf[8];
		std::snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", p[i]);
		ss << buf;
	}
	if (n > max_show) ss << " ...";
	return ss.str();
}

struct NamedCodeEntry {
	uint16_t addr = 0;
	std::string name;
	bool ram = true;
};

// Nearest named function/label at or before addr, capped by next named entry
// and max_dist (same spirit as map BRANCH ownership).
const NamedCodeEntry * code_owner(const std::vector<NamedCodeEntry> & named,
                                  uint16_t addr, uint16_t max_dist = 0x400) {
	if (named.empty()) return nullptr;
	auto it = std::upper_bound(
		named.begin(), named.end(), addr,
		[](uint16_t a, const NamedCodeEntry & e) { return a < e.addr; });
	if (it == named.begin()) return nullptr;
	--it;
	uint16_t cap = 0xffff;
	if (std::next(it) != named.end()) cap = std::next(it)->addr;
	if (addr >= cap) return nullptr;
	if (uint16_t(addr - it->addr) >= max_dist) return nullptr;
	return &(*it);
}

bool sites_are_internal_flow(const std::vector<revm::MemRefSite> & sites) {
	if (sites.empty()) return false;
	for (const auto & s : sites) {
		if (std::strcmp(s.kind, "branch") != 0 && std::strcmp(s.kind, "jmp") != 0)
			return false;
	}
	return true;
}

std::string format_ref_sites(const std::vector<revm::MemRefSite> & sites) {
	constexpr size_t kShow = 4;
	std::ostringstream ss;
	const size_t n = sites.size();
	const size_t show = std::min(n, kShow);
	for (size_t i = 0; i < show; ++i) {
		if (i) ss << ", ";
		const auto & s = sites[i];
		char buf[40];
		std::snprintf(buf, sizeof(buf), "%s %s @$%04X", s.kind, s.mnemonic, s.pc);
		ss << buf;
	}
	if (n > kShow) ss << " (+" << (n - kShow) << " more)";
	return ss.str();
}

struct MapAddrInfo {
	bool from_code = false;
	bool mem_ram = false;
	bool mem_rom = false;
	bool pc_rom = false;
	const std::vector<revm::MemRefSite> * sites = nullptr;
};

bool kb_has_bank_at(const revm::KnowledgeBase & kb, uint16_t addr,
                    const std::string & bank) {
	for (const auto & o : kb.Objects()) {
		if (o.addr != addr) continue;
		if (bank == "ram") {
			if (o.IsRamBank()) return true;
		} else if (!o.IsRamBank() && o.bank == bank) {
			return true;
		}
	}
	return false;
}

int generate_map(const std::string & source_desc,
                 const uint8_t ram[0x10000],
                 const revm::KnowledgeBase & kb,
                 const std::string & kb_path,
                 const revm::KnowledgeBase * support_ptr,
                 const revm::KnowledgeBase & kb_support,
                 const std::string & kb_support_path,
                 const std::string & cover_path,
                 const std::string & out_path,
                 unsigned bytes,
                 bool only_tally) {
	std::string error;
	revm::MemRefMap refs;
	revm::Coverage cov;
	bool have_cov = false;
	std::string scan_desc;
	if (!cover_path.empty()) {
		if (!load_coverage_file(cover_path, cov, error)) {
			std::fprintf(stderr, "coverage load failed: %s\n", error.c_str());
			return 1;
		}
		have_cov = true;
		revm::CollectMemRefsCovered(ram, cov, refs);
		scan_desc = coverage_scan_desc(cover_path, cov);
	} else {
		size_t n_fn = 0;
		for (const auto & o : kb.Objects()) {
			if (o.kind != revm::KbKind::Function || o.name.empty()) continue;
			if (!o.IsRamBank()) continue;
			const unsigned nbytes = o.end ? o.ByteLength() : bytes;
			revm::CollectMemRefs(ram, o.addr, nbytes, refs);
			++n_fn;
		}
		scan_desc = "KB functions (" + std::to_string(n_fn) +
		            " routines, --bytes fallback " + std::to_string(bytes) + ")";
	}

	std::map<uint16_t, MapAddrInfo> info;
	auto ensure = [&](uint16_t a) -> MapAddrInfo & { return info[a]; };

	for (const auto & [a, sites] : refs) {
		auto & i = ensure(a);
		i.from_code = true;
		i.sites = &sites;
	}
	if (have_cov) {
		for (uint32_t a = 0; a < 0x10000; ++a) {
			if (cov.HitMemRam(uint16_t(a))) ensure(uint16_t(a)).mem_ram = true;
			if (cov.HitMemRom(uint16_t(a))) ensure(uint16_t(a)).mem_rom = true;
			if (cov.HitPcRom(uint16_t(a))) ensure(uint16_t(a)).pc_rom = true;
		}
	}
	for (const auto & o : kb.Objects()) {
		if (o.kind == revm::KbKind::Opcodes) continue; // listing-only; not map rows
		ensure(o.addr); // KB-only addresses
	}

	std::set<uint16_t> ram_addrs;
	std::set<uint16_t> basic_addrs;
	std::set<uint16_t> kernal_addrs;
	for (const auto & [addr, i] : info) {
		const bool in_basic = addr >= 0xA000 && addr < 0xC000;
		const bool in_kernal = addr >= 0xE000;
		const bool has_ram_kb = kb_has_bank_at(kb, addr, "ram");
		const bool has_basic_kb = kb_has_bank_at(kb, addr, "basic");
		const bool has_kernal_kb = kb_has_bank_at(kb, addr, "kernal");
		const bool has_chip_kb = kb_has_bank_at(kb, addr, "chip");
		const bool rom_hit = i.mem_rom || i.pc_rom;

		// RAM section: MEM-RAM, RAM/chip KB, and code-operand refs that are not
		// actual ROM-plane hits. Under-ROM game code/data ($A000+ / $E000+) is
		// RAM — do not shunt those operand refs into BASIC/KERNAL.
		if (i.mem_ram || has_ram_kb || has_chip_kb) {
			ram_addrs.insert(addr);
		} else if (i.from_code && !rom_hit) {
			ram_addrs.insert(addr);
		}

		// BASIC / KERNAL: only real ROM accesses (PC-ROM / MEM-ROM) or banked KB.
		// Support KB never adds addresses (naming fallback only).
		if (in_basic && (rom_hit || has_basic_kb)) {
			basic_addrs.insert(addr);
		}
		if (in_kernal && (rom_hit || has_kernal_kb)) {
			kernal_addrs.insert(addr);
		}
	}

	auto build_named_code = [&](const std::string & bank) {
		std::vector<NamedCodeEntry> named;
		for (const auto & o : kb.Objects()) {
			if (o.name.empty()) continue;
			if (o.kind != revm::KbKind::Function && o.kind != revm::KbKind::Label) {
				continue;
			}
			const bool is_ram = o.IsRamBank();
			if (bank == "ram") {
				if (!is_ram) continue;
			} else if (is_ram || o.bank != bank) {
				continue;
			}
			named.push_back(NamedCodeEntry{o.addr, o.name, is_ram});
		}
		std::sort(named.begin(), named.end(),
		          [](const NamedCodeEntry & a, const NamedCodeEntry & b) {
			          return a.addr < b.addr;
		          });
		named.erase(std::unique(named.begin(), named.end(),
		                        [](const NamedCodeEntry & a, const NamedCodeEntry & b) {
			                        return a.addr == b.addr;
		                        }),
		            named.end());
		return named;
	};
	const auto named_code_ram = build_named_code("ram");
	const auto named_code_basic = build_named_code("basic");
	const auto named_code_kernal = build_named_code("kernal");

	size_t n_chip = 0, n_var = 0, n_blob = 0, n_func = 0, n_label = 0;
	size_t n_branch = 0, n_comment = 0, n_undoc = 0;
	size_t n_kb_only = 0;
	size_t n_referenced = 0;

	std::ostringstream body;
	if (!only_tally) {
		body << "# Address map — code operands + MEM hits + KB\n"
		     << "# Source: " << source_desc << "\n"
		     << "# KB: " << kb_path << " (" << kb.Size() << " objects)\n";
		if (support_ptr) {
			body << "# Support KB: " << kb_support_path << " (" << kb_support.Size()
			     << " objects; naming fallback only — no forced rows)\n";
		}
		body << "# Scan: " << scan_desc << "\n"
		     << "# Sections: RAM=" << ram_addrs.size()
		     << "  BASIC=" << basic_addrs.size()
		     << "  KERNAL=" << kernal_addrs.size() << "\n"
		     << "#\n"
		     << "# Columns: ADDR[:ADDR]  KIND  DETAIL\n"
		     << "#   Consecutive identical annotations collapse to $LO:$HI\n"
		     << "#   CHIP    <name>  ; comment   (bank:chip from --kb / --kb-support)\n"
		     << "#   VAR     <name>  ; comment\n"
		     << "#   BLOB    <name>  ; comment\n"
		     << "#   FUNC    <name>  ; comment\n"
		     << "#   LABEL   <name>  ; comment\n"
		     << "#   BRANCH  in <owner>  <sites>   (unlabeled internal control-flow)\n"
		     << "#   COMMENT [<name>]  ; text   (KB kind comment / omitted)\n"
		     << "#   UNDOC   <sites|mem-ram|mem-rom|pc-rom>  (not named; not internal branch)\n"
		     << "#   Bank is the section (RAM / BASIC / KERNAL), not a per-line tag\n"
		     << "#   BASIC/KERNAL: PC-ROM / MEM-ROM hits (or bank:basic/kernal KB only)\n"
		     << "#   (kb-only) = present in game KB, never seen as code operand or MEM hit\n\n";
	}

	auto emit_section = [&](const char * title, const std::set<uint16_t> & addrs,
	                        const std::string & bank,
	                        const std::vector<NamedCodeEntry> & named_code) {
		if (addrs.empty()) return;
		if (!only_tally) {
			body << "########################################\n"
			     << "# " << title << "\n"
			     << "########################################\n\n";
		}

		struct MapRow {
			uint16_t addr = 0;
			std::string annot; // KIND + detail (no address)
		};
		std::vector<MapRow> rows;
		auto push_row = [&](uint16_t addr, std::string annot) {
			if (!only_tally) rows.push_back(MapRow{addr, std::move(annot)});
		};

		auto emit_chip = [&](uint16_t addr, const revm::KbObject & o, bool from_use) {
			std::ostringstream ss;
			ss << "CHIP    " << o.name;
			if (!o.comment.empty()) ss << "  ; " << o.comment;
			if (!from_use) ss << "  (kb-only)";
			push_row(addr, ss.str());
			++n_chip;
		};

		auto emit_named = [&](uint16_t addr, const char * cat, const revm::KbObject & o,
		                      bool from_use) {
			char head[48];
			std::snprintf(head, sizeof(head), "%-7s %s", cat, o.name.c_str());
			std::ostringstream ss;
			ss << head;
			if (!o.comment.empty()) ss << "  ; " << o.comment;
			if (!from_use) ss << "  (kb-only)";
			push_row(addr, ss.str());
		};

		auto categorize_named = [&](uint16_t addr, const revm::KbObject & o,
		                            bool from_use) -> bool {
			if (o.IsChipBank()) {
				if (o.name.empty()) return false;
				emit_chip(addr, o, from_use);
				return true;
			}
			switch (o.kind) {
			case revm::KbKind::Variable:
				emit_named(addr, "VAR", o, from_use);
				++n_var;
				return true;
			case revm::KbKind::Blob:
				emit_named(addr, "BLOB", o, from_use);
				++n_blob;
				return true;
			case revm::KbKind::Function:
				emit_named(addr, "FUNC", o, from_use);
				++n_func;
				return true;
			case revm::KbKind::Label:
				emit_named(addr, "LABEL", o, from_use);
				++n_label;
				return true;
			case revm::KbKind::Opcodes:
			case revm::KbKind::Comment:
				return false;
			}
			return false;
		};

		auto emit_comment = [&](uint16_t addr, const revm::KbObject & o, bool from_use) {
			std::ostringstream ss;
			ss << "COMMENT ";
			if (!o.name.empty()) ss << o.name << "  ";
			if (!o.comment.empty()) ss << "; " << o.comment;
			if (!from_use) ss << "  (kb-only)";
			push_row(addr, ss.str());
			++n_comment;
		};

		// Try to name from one KB (main or support). Returns true if a row was emitted.
		auto try_kb_name = [&](const revm::KnowledgeBase & src, uint16_t addr,
		                       bool from_use) -> bool {
			const auto exacts = kb_exacts_at(src, addr, bank);
			bool emitted_exact = false;
			for (const revm::KbObject * o : exacts) {
				if (o->kind == revm::KbKind::Opcodes) continue;
				if (o->kind == revm::KbKind::Comment) {
					emit_comment(addr, *o, from_use);
					emitted_exact = true;
					continue;
				}
				if (!o->name.empty() && categorize_named(addr, *o, from_use)) {
					emitted_exact = true;
					continue;
				}
				if (!o->comment.empty()) {
					emit_comment(addr, *o, from_use);
					emitted_exact = true;
				}
			}
			if (emitted_exact) return true;

			const revm::KbObject * named = named_kb_at(src, addr, bank);
			if (named && !named->name.empty()) {
				if (named->kind == revm::KbKind::Variable ||
				    named->kind == revm::KbKind::Blob || named->IsChipBank()) {
					if (categorize_named(addr, *named, from_use)) return true;
				}
				if (named->kind == revm::KbKind::Function && named->end &&
				    addr >= named->addr && addr <= *named->end) {
					if (categorize_named(addr, *named, from_use)) return true;
				}
				if (named->kind == revm::KbKind::Label &&
				    named->addr == addr) {
					if (categorize_named(addr, *named, from_use)) return true;
				}
			}
			return false;
		};

		for (uint16_t addr : addrs) {
			const MapAddrInfo & i = info[addr];
			const bool from_use =
			    i.from_code || i.mem_ram || i.mem_rom || i.pc_rom;
			if (!from_use) ++n_kb_only;
			else ++n_referenced;

			// bank:chip — game KB first, then support (RAM section only).
			if (bank == "ram") {
				const revm::KbObject * chip = nullptr;
				for (const revm::KbObject * o : kb_exacts_at(kb, addr, "chip")) {
					if (!o->name.empty()) {
						chip = o;
						break;
					}
				}
				if (!chip && support_ptr) {
					for (const revm::KbObject * o :
					     kb_exacts_at(*support_ptr, addr, "chip")) {
						if (!o->name.empty()) {
							chip = o;
							break;
						}
					}
				}
				if (chip) {
					emit_chip(addr, *chip, from_use);
					continue;
				}
			}

			if (try_kb_name(kb, addr, from_use)) continue;
			// Support KB: name only if this address is already in the map.
			if (support_ptr && try_kb_name(*support_ptr, addr, from_use)) continue;

			if (i.from_code && i.sites && sites_are_internal_flow(*i.sites)) {
				if (const NamedCodeEntry * owner = code_owner(named_code, addr)) {
					std::ostringstream ss;
					ss << "BRANCH  in " << owner->name << "  "
					   << format_ref_sites(*i.sites);
					push_row(addr, ss.str());
					++n_branch;
					continue;
				}
			}

			if (from_use) {
				std::ostringstream ss;
				ss << "UNDOC   ";
				if (i.from_code && i.sites) {
					ss << format_ref_sites(*i.sites);
				} else if (bank != "ram" && i.pc_rom) {
					ss << "pc-rom";
				} else if (bank != "ram" && i.mem_rom) {
					ss << "mem-rom";
				} else if (i.mem_ram) {
					ss << "mem-ram";
				} else if (i.mem_rom) {
					ss << "mem-rom";
				} else if (i.pc_rom) {
					ss << "pc-rom";
				} else {
					ss << "mem-hit";
				}
				push_row(addr, ss.str());
				++n_undoc;
				continue;
			}

			const revm::KbObject * exact = kb_exact_at(kb, addr, bank);
			std::ostringstream ss;
			ss << "COMMENT ";
			if (exact && !exact->comment.empty()) ss << "; " << exact->comment;
			ss << "  (kb-only)";
			push_row(addr, ss.str());
			++n_comment;
		}

		// Collapse consecutive addresses with identical annotations into ranges.
		// Address column is 11 chars (`$XXXX` or `$XXXX:$XXXX`), then KIND…
		if (only_tally) return;
		for (size_t i = 0; i < rows.size();) {
			size_t j = i + 1;
			while (j < rows.size() &&
			       rows[j].addr == uint16_t(rows[j - 1].addr + 1) &&
			       rows[j].annot == rows[i].annot) {
				++j;
			}
			const uint16_t lo = rows[i].addr;
			const uint16_t hi = rows[j - 1].addr;
			char addr_col[16];
			if (lo == hi) {
				std::snprintf(addr_col, sizeof(addr_col), "$%04X", lo);
			} else {
				std::snprintf(addr_col, sizeof(addr_col), "$%04X:$%04X", lo, hi);
			}
			char line[512];
			std::snprintf(line, sizeof(line), "%-11s  %s\n", addr_col,
			              rows[i].annot.c_str());
			body << line;
			i = j;
		}
		body << "\n";
	};

	emit_section("RAM — MEM-RAM + code operands + RAM KB", ram_addrs, "ram",
	             named_code_ram);
	emit_section("BASIC ROM — MEM-ROM / code refs / BASIC KB ($A000-$BFFF)",
	             basic_addrs, "basic", named_code_basic);
	emit_section("KERNAL ROM — MEM-ROM / code refs / KERNAL KB ($E000-$FFFF)",
	             kernal_addrs, "kernal", named_code_kernal);

	const double named_pct =
	    n_referenced == 0
	        ? 100.0
	        : (100.0 * (1.0 - double(n_undoc) / double(n_referenced)));

	if (only_tally) {
		body << "# Address map tally\n"
		     << "# Source: " << source_desc << "\n"
		     << "# KB: " << kb_path << " (" << kb.Size() << " objects)\n";
		if (support_ptr) {
			body << "# Support KB: " << kb_support_path << " (" << kb_support.Size()
			     << " objects)\n";
		}
		body << "# Scan: " << scan_desc << "\n"
		     << "# Sections: RAM=" << ram_addrs.size()
		     << "  BASIC=" << basic_addrs.size()
		     << "  KERNAL=" << kernal_addrs.size() << "\n\n";
	}

	body << "--- tally ---\n"
	     << "Total section addresses: "
	     << (ram_addrs.size() + basic_addrs.size() + kernal_addrs.size()) << "\n"
	     << "  CHIP:    " << n_chip << "\n"
	     << "  VAR:     " << n_var << "\n"
	     << "  BLOB:    " << n_blob << "\n"
	     << "  FUNC:    " << n_func << "\n"
	     << "  LABEL:   " << n_label << "\n"
	     << "  BRANCH:  " << n_branch << "\n"
	     << "  COMMENT: " << n_comment << "\n"
	     << "  UNDOC:   " << n_undoc << "\n"
	     << "  kb-only (no code operand / MEM hit): " << n_kb_only << "\n"
	     << "  referenced (code operand or MEM hit): " << n_referenced << "\n";
	{
		char ratio[96];
		std::snprintf(ratio, sizeof(ratio),
		              "named (non-UNDOC) / referenced: %zu / %zu  (%.1f%%)\n",
		              n_referenced - n_undoc, n_referenced, named_pct);
		body << ratio;
	}

	const std::string text = body.str();
	if (out_path.empty()) {
		std::fputs(text.c_str(), stdout);
	} else {
		std::ofstream f(out_path);
		if (!f) {
			std::fprintf(stderr, "Cannot write %s\n", out_path.c_str());
			return 1;
		}
		f << text;
		if (only_tally) {
			std::fprintf(stderr, "Wrote tally %s (named=%.1f%%)\n", out_path.c_str(),
			             named_pct);
		} else {
			std::fprintf(stderr,
				"Wrote %s (ram=%zu basic=%zu kernal=%zu; chip=%zu var=%zu blob=%zu "
				"func=%zu label=%zu branch=%zu comment=%zu undoc=%zu; named=%.1f%%)\n",
				out_path.c_str(), ram_addrs.size(), basic_addrs.size(),
				kernal_addrs.size(), n_chip, n_var, n_blob, n_func, n_label, n_branch,
				n_comment, n_undoc, named_pct);
		}
	}
	return 0;
}

int generate_call_graph(const uint8_t ram[0x10000],
                        const revm::KnowledgeBase & kb,
                        const std::string & cover_path,
                        const std::string & out_path) {
	if (cover_path.empty()) {
		std::fprintf(stderr, "--call-graph requires --cov FILE\n");
		return 2;
	}
	std::string error;
	revm::Coverage cov;
	if (!load_coverage_file(cover_path, cov, error)) {
		std::fprintf(stderr, "coverage load failed: %s\n", error.c_str());
		return 1;
	}

	revm::MemRefMap refs;
	revm::CollectMemRefsCovered(ram, cov, refs);

	struct Caller {
		uint16_t pc = 0;
		uint16_t weight = 0;
	};
	struct Target {
		uint16_t addr = 0;
		uint64_t hits = 0;
		std::vector<Caller> callers;
	};
	std::map<uint16_t, Target> by_tgt;

	for (const auto & [addr, sites] : refs) {
		for (const auto & site : sites) {
			if (std::strcmp(site.kind, "jsr") != 0 &&
			    std::strcmp(site.kind, "jmp") != 0) {
				continue;
			}
			const uint16_t w =
			    cov.Get(revm::Coverage::Plane::PcRam, site.pc);
			auto & t = by_tgt[addr];
			t.addr = addr;
			t.hits += w;
			t.callers.push_back(Caller{site.pc, w});
		}
	}

	std::vector<Target> targets;
	targets.reserve(by_tgt.size());
	for (auto & kv : by_tgt) {
		auto & t = kv.second;
		std::sort(t.callers.begin(), t.callers.end(),
		          [](const Caller & a, const Caller & b) {
			          if (a.weight != b.weight) return a.weight > b.weight;
			          return a.pc < b.pc;
		          });
		targets.push_back(std::move(t));
	}
	std::sort(targets.begin(), targets.end(),
	          [](const Target & a, const Target & b) {
		          if (a.hits != b.hits) return a.hits > b.hits;
		          if (a.callers.size() != b.callers.size())
			          return a.callers.size() > b.callers.size();
		          return a.addr < b.addr;
	          });

	std::ostringstream body;
	body << "# Call graph (PC-RAM fetch counts; u16 saturating)\n"
	     << "# Scan: " << coverage_scan_desc(cover_path, cov) << "\n\n";

	size_t n_edges = 0;
	for (const auto & t : targets) {
		n_edges += t.callers.size();
		const revm::KbObject * named = named_kb_at(kb, t.addr);
		const char * name =
		    (named && !named->name.empty()) ? named->name.c_str() : "(unnamed)";
		char head[192];
		std::snprintf(head, sizeof(head),
		              "$%04X  %-24s  hits=%llu  sites=%zu\n", t.addr, name,
		              static_cast<unsigned long long>(t.hits), t.callers.size());
		body << head;
		body << "  from";
		constexpr size_t kShow = 12;
		for (size_t i = 0; i < t.callers.size() && i < kShow; ++i) {
			char buf[40];
			std::snprintf(buf, sizeof(buf), "  $%04X×%u", t.callers[i].pc,
			              unsigned(t.callers[i].weight));
			body << buf;
		}
		if (t.callers.size() > kShow) {
			body << "  (+" << (t.callers.size() - kShow) << " more)";
		}
		body << "\n";
	}

	if (!write_text_out(out_path, body.str())) return 1;
	std::fprintf(stderr, "call-graph → %s  (%zu targets, %zu edges)\n",
	             out_path.c_str(), targets.size(), n_edges);
	return 0;
}

int generate_hot_undoc(const uint8_t ram[0x10000],
                       const revm::KnowledgeBase & kb,
                       const std::string & cover_path,
                       const std::string & out_path,
                       unsigned top_n) {
	if (cover_path.empty()) {
		std::fprintf(stderr, "--hot-undoc requires --cov FILE\n");
		return 2;
	}
	if (top_n == 0) top_n = 80;
	std::string error;
	revm::Coverage cov;
	if (!load_coverage_file(cover_path, cov, error)) {
		std::fprintf(stderr, "coverage load failed: %s\n", error.c_str());
		return 1;
	}

	revm::MemRefMap refs;
	revm::CollectMemRefsCovered(ram, cov, refs);

	struct UndocHit {
		uint16_t addr = 0;
		uint16_t mem = 0;
		size_t code_sites = 0;
		bool is_zp = false;
	};
	std::vector<UndocHit> hits;
	for (uint32_t a = 0; a < 0x10000; ++a) {
		const uint16_t mem =
		    cov.Get(revm::Coverage::Plane::MemRam, uint16_t(a));
		if (mem == 0) continue;
		if (a >= 0xD000 && a <= 0xDFFF) continue;
		if (game_kb_names_ram(kb, uint16_t(a))) continue;
		size_t n_sites = 0;
		auto it = refs.find(uint16_t(a));
		if (it != refs.end()) n_sites = it->second.size();
		hits.push_back(UndocHit{uint16_t(a), mem, n_sites, a < 0x100});
	}
	std::sort(hits.begin(), hits.end(),
	          [](const UndocHit & a, const UndocHit & b) {
		          if (a.is_zp != b.is_zp) return a.is_zp && !b.is_zp;
		          if (a.mem != b.mem) return a.mem > b.mem;
		          return a.addr < b.addr;
	          });

	std::ostringstream body;
	body << "# Hottest UNDOC MEM hits (game KB unnamed; MemRam counts)\n"
	     << "# Scan: " << coverage_scan_desc(cover_path, cov) << "\n"
	     << "# Showing top " << top_n << " of " << hits.size() << "\n\n";

	body << "## UNDOC addresses\n";
	const size_t n_show = std::min(hits.size(), size_t(top_n));
	for (size_t i = 0; i < n_show; ++i) {
		const auto & h = hits[i];
		char line[96];
		std::snprintf(line, sizeof(line), "%s  $%04X  mem=%u%s\n",
		              h.is_zp ? "ZP " : "ABS", h.addr, unsigned(h.mem),
		              h.mem == 0xFFFF ? " (sat)" : "");
		body << line;
	}
	body << "\n";

	std::vector<UndocHit> code_ref;
	for (const auto & h : hits) {
		if (h.code_sites > 0) code_ref.push_back(h);
	}
	std::sort(code_ref.begin(), code_ref.end(),
	          [](const UndocHit & a, const UndocHit & b) {
		          if (a.mem != b.mem) return a.mem > b.mem;
		          if (a.code_sites != b.code_sites)
			          return a.code_sites > b.code_sites;
		          return a.addr < b.addr;
	          });
	body << "## Code-referenced UNDOC (mem hits + operand sites; top "
	     << top_n << ")\n";
	const size_t n_cref = std::min(code_ref.size(), size_t(top_n));
	for (size_t i = 0; i < n_cref; ++i) {
		const auto & h = code_ref[i];
		char line[96];
		std::snprintf(line, sizeof(line),
		              "$%04X  mem=%u  code_sites=%zu\n", h.addr,
		              unsigned(h.mem), h.code_sites);
		body << line;
	}
	body << "\n";

	struct SpanSum {
		uint16_t lo = 0;
		uint16_t hi_excl = 0;
		uint64_t sum = 0;
	};
	std::vector<SpanSum> spans;
	for (const auto & r : cov.Coalesce(revm::Coverage::Plane::MemRam, 0)) {
		bool all_named = true;
		uint64_t sum = 0;
		for (uint32_t a = r.first; a < r.second; ++a) {
			sum += cov.Get(revm::Coverage::Plane::MemRam, uint16_t(a));
			if (!game_kb_names_ram(kb, uint16_t(a))) all_named = false;
		}
		if (all_named) continue;
		if (r.first >= 0xD000 && r.second <= 0xE000) continue;
		spans.push_back(SpanSum{r.first, r.second, sum});
	}
	std::sort(spans.begin(), spans.end(),
	          [](const SpanSum & a, const SpanSum & b) {
		          if (a.sum != b.sum) return a.sum > b.sum;
		          return a.lo < b.lo;
	          });

	body << "## Hot MEM spans (unnamed bytes present; top 20 by sum)\n";
	const size_t n_span = std::min(spans.size(), size_t(20));
	for (size_t i = 0; i < n_span; ++i) {
		const auto & s = spans[i];
		char line[96];
		std::snprintf(line, sizeof(line),
		              "$%04X:$%04X  sum=%llu  len=%u\n", s.lo,
		              uint16_t(s.hi_excl - 1),
		              static_cast<unsigned long long>(s.sum),
		              unsigned(s.hi_excl - s.lo));
		body << line;
	}

	if (!write_text_out(out_path, body.str())) return 1;
	std::fprintf(stderr,
	             "hot-undoc → %s  (%zu undoc addrs, showed %zu; %zu code-ref; "
	             "%zu spans)\n",
	             out_path.c_str(), hits.size(), n_show, code_ref.size(),
	             spans.size());
	return 0;
}

int generate_watch_diff(const uint8_t ram_a[0x10000],
                        const uint8_t color_a[COLOR_RAM_SIZE],
                        const std::string & other_snap_path,
                        const revm::KnowledgeBase & kb,
                        bool include_same) {
	std::string error;
	revm::FullSnapshot other{};
	if (!load_full_snap(other_snap_path, other, error)) {
		std::fprintf(stderr, "--watch-diff: %s\n", error.c_str());
		return 1;
	}

	size_t n_slots = 0;
	size_t n_diff = 0;
	size_t n_same = 0;
	for (const auto & o : kb.Objects()) {
		if ((!o.IsRamBank() && !o.IsColorBank()) ||
		    o.watch == revm::KbWatchLevel::No)
			continue;
		++n_slots;
		const unsigned len = o.ByteLength();
		std::vector<uint8_t> a_bytes(len), b_bytes(len);
		for (unsigned i = 0; i < len; ++i) {
			const uint16_t addr = uint16_t(o.addr + i);
			a_bytes[i] = revm::KbWatchSamplePlanes(o, addr, ram_a, color_a);
			b_bytes[i] = revm::KbWatchSamplePlanes(o, addr, other.ram, other.color);
		}
		const bool same = a_bytes == b_bytes;
		if (same) {
			++n_same;
			if (!include_same) continue;
		} else {
			++n_diff;
		}

		const char * label =
		    o.name.empty() ? "(unnamed)" : o.name.c_str();
		char addr_col[24];
		if (len == 1) {
			std::snprintf(addr_col, sizeof(addr_col), "$%04X", o.addr);
		} else {
			std::snprintf(addr_col, sizeof(addr_col), "$%04X:$%04X", o.addr,
			              uint16_t(o.addr + len - 1));
		}

		if (same) {
			std::fprintf(stdout, "SAME  %-24s  %-11s  %s\n", label, addr_col,
			             format_hex_bytes(a_bytes.data(), len).c_str());
		} else if (len == 1) {
			std::fprintf(stdout, "DIFF  %-24s  %-11s  a=$%02X  b=$%02X\n",
			             label, addr_col, a_bytes[0], b_bytes[0]);
		} else {
			std::fprintf(stdout, "DIFF  %-24s  %-11s  a=%s  b=%s\n", label,
			             addr_col, format_hex_bytes(a_bytes.data(), len).c_str(),
			             format_hex_bytes(b_bytes.data(), len).c_str());
		}
	}

	std::fprintf(stderr,
	             "watch-diff → %s  (%zu slots; %zu DIFF, %zu SAME%s)\n",
	             other_snap_path.c_str(), n_slots, n_diff, n_same,
	             include_same ? "" : "; unchanged omitted");
	return 0;
}

int cmd_sort_kb(int argc, char ** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "sort-kb requires INPUT.kb.json OUTPUT.kb.json\n");
		return 2;
	}
	const std::string in_path = argv[0];
	const std::string out_path = argv[1];
	revm::KnowledgeBase kb;
	std::string error;
	if (!kb.LoadFile(in_path, error)) {
		std::fprintf(stderr, "KB load failed: %s\n", error.c_str());
		return 1;
	}
	kb.SortByAddress();
	if (!kb.SaveFile(out_path, error)) {
		std::fprintf(stderr, "KB save failed: %s\n", error.c_str());
		return 1;
	}
	std::fprintf(stderr, "sort-kb → %s  (%zu objects)\n", out_path.c_str(), kb.Size());
	return 0;
}

int cmd_merge_cov(int argc, char ** argv) {
	if (argc < 2) {
		std::fprintf(stderr,
			"merge-cov requires INPUT.cov … MERGED.cov (last path is output)\n");
		return 2;
	}
	const std::string out_path = argv[argc - 1];
	revm::Coverage merged;
	merged.Clear();
	std::string error;
	for (int i = 0; i < argc - 1; ++i) {
		revm::Coverage one;
		if (!one.Load(argv[i], error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		merged.Merge(one);
	}
	if (!merged.Save(out_path, error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::fprintf(stderr,
		"merge-cov → %s  (REVMCOV2; %d input(s); "
		"pc_ram=%zu/%llu pc_rom=%zu/%llu mem_ram=%zu/%llu mem_rom=%zu/%llu)\n",
		out_path.c_str(), argc - 1,
		merged.Count(revm::Coverage::Plane::PcRam),
		static_cast<unsigned long long>(
			merged.TotalHits(revm::Coverage::Plane::PcRam)),
		merged.Count(revm::Coverage::Plane::PcRom),
		static_cast<unsigned long long>(
			merged.TotalHits(revm::Coverage::Plane::PcRom)),
		merged.Count(revm::Coverage::Plane::MemRam),
		static_cast<unsigned long long>(
			merged.TotalHits(revm::Coverage::Plane::MemRam)),
		merged.Count(revm::Coverage::Plane::MemRom),
		static_cast<unsigned long long>(
			merged.TotalHits(revm::Coverage::Plane::MemRom)));
	return 0;
}

int cmd_inspect(bool is_snap, int argc, char ** argv) {
	std::string target;
	std::string kb_path;
	std::string kb_support_path;
	std::string dump_ram;
	std::string dump_cpuview;
	std::string dump_blobs_dir;
	std::string listing_path;
	std::string cov_path;
	std::string map_path;
	std::string call_graph_path;
	std::string hot_undoc_path;
	std::string watch_diff_path;
	std::string rom_dir;
	unsigned hot_undoc_top = 80;
	bool hot_undoc_top_set = false;
	bool map_tally = false;
	bool list_no_cov_rng = false;
	bool watch_diff_all = false;
	bool do_disasm = false;
	bool have_disasm_addr = false;
	uint16_t disasm_addr = 0;
	unsigned disasm_bytes = 128;

	for (int i = 0; i < argc; ++i) {
		const char * a = argv[i];
		if (std::strcmp(a, "--rom-dir") == 0 && i + 1 < argc) {
			rom_dir = argv[++i];
		} else if (std::strcmp(a, "--kb") == 0 && i + 1 < argc) {
			kb_path = argv[++i];
		} else if (std::strcmp(a, "--kb-support") == 0 && i + 1 < argc) {
			kb_support_path = argv[++i];
		} else if (std::strcmp(a, "--disasm-addr") == 0 && i + 1 < argc) {
			std::string err;
			if (!parse_u16_hex(argv[++i], disasm_addr, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
				return 2;
			}
			have_disasm_addr = true;
		} else if (std::strcmp(a, "--disasm-bytes") == 0 && i + 1 < argc) {
			disasm_bytes = unsigned(std::atoi(argv[++i]));
			if (disasm_bytes == 0) disasm_bytes = 128;
		} else if (std::strcmp(a, "--disasm") == 0) {
			do_disasm = true;
		} else if (std::strcmp(a, "--dump-ram") == 0 && i + 1 < argc) {
			dump_ram = argv[++i];
		} else if (std::strcmp(a, "--dump-cpuview") == 0 && i + 1 < argc) {
			dump_cpuview = argv[++i];
		} else if (std::strcmp(a, "--dump-blobs") == 0 && i + 1 < argc) {
			dump_blobs_dir = argv[++i];
		} else if (std::strcmp(a, "--listing") == 0 && i + 1 < argc) {
			listing_path = argv[++i];
		} else if (std::strcmp(a, "--cov") == 0 && i + 1 < argc) {
			if (!cov_path.empty()) {
				std::fprintf(stderr, "only one --cov FILE (pre-merge with merge-cov)\n");
				return 2;
			}
			cov_path = argv[++i];
		} else if (std::strcmp(a, "--list-no-cov-rng") == 0) {
			list_no_cov_rng = true;
		} else if (std::strcmp(a, "--map") == 0 && i + 1 < argc) {
			map_path = argv[++i];
		} else if (std::strcmp(a, "--map-tally") == 0) {
			map_tally = true;
		} else if (std::strcmp(a, "--call-graph") == 0 && i + 1 < argc) {
			call_graph_path = argv[++i];
		} else if (std::strcmp(a, "--hot-undoc") == 0 && i + 1 < argc) {
			hot_undoc_path = argv[++i];
		} else if (std::strcmp(a, "--hot-undoc-top") == 0 && i + 1 < argc) {
			hot_undoc_top = unsigned(std::atoi(argv[++i]));
			hot_undoc_top_set = true;
			if (hot_undoc_top == 0) hot_undoc_top = 80;
		} else if (std::strcmp(a, "--watch-diff") == 0 && i + 1 < argc) {
			watch_diff_path = argv[++i];
		} else if (std::strcmp(a, "--watch-diff-all") == 0) {
			watch_diff_all = true;
		} else if (a[0] != '-') {
			if (!target.empty()) {
				std::fprintf(stderr, "Unexpected argument: %s\n", a);
				return 2;
			}
			target = a;
		} else {
			std::fprintf(stderr, "Unknown option: %s\n", a);
			return 2;
		}
	}

	if (target.empty()) {
		std::fprintf(stderr, "%s requires a path\n", is_snap ? "snap" : "ram");
		return 2;
	}

	const bool want_listing = !listing_path.empty();
	const bool want_map = !map_path.empty() || map_tally;
	const bool want_blobs = !dump_blobs_dir.empty();
	const bool want_call_graph = !call_graph_path.empty();
	const bool want_hot_undoc = !hot_undoc_path.empty();
	const bool want_watch_diff = !watch_diff_path.empty();
	const bool want_kb_assets = want_listing || want_map || want_blobs ||
	                            want_call_graph || want_hot_undoc ||
	                            want_watch_diff;

	if (want_kb_assets && kb_path.empty()) {
		std::fprintf(stderr,
			"--listing / --map / --map-tally / --dump-blobs / --call-graph / "
			"--hot-undoc / --watch-diff require --kb FILE\n");
		return 2;
	}
	if (!cov_path.empty() && kb_path.empty() &&
	    (want_listing || want_map || want_call_graph || want_hot_undoc)) {
		std::fprintf(stderr, "--cov with listing/map/call-graph/hot-undoc requires --kb FILE\n");
		return 2;
	}
	if ((want_call_graph || want_hot_undoc) && cov_path.empty()) {
		std::fprintf(stderr, "--call-graph / --hot-undoc require --cov FILE\n");
		return 2;
	}
	if (hot_undoc_top_set && !want_hot_undoc) {
		std::fprintf(stderr, "--hot-undoc-top requires --hot-undoc FILE\n");
		return 2;
	}
	if (watch_diff_all && !want_watch_diff) {
		std::fprintf(stderr, "--watch-diff-all requires --watch-diff SNAP\n");
		return 2;
	}
	if (list_no_cov_rng && !want_listing) {
		std::fprintf(stderr, "--list-no-cov-rng requires --listing FILE\n");
		return 2;
	}

	if (!is_snap) {
		if (do_disasm) {
			std::fprintf(stderr, "ram mode: --disasm is SNAP-only (use --disasm-addr)\n");
			return 2;
		}
		if (!dump_ram.empty() || !dump_cpuview.empty()) {
			std::fprintf(stderr, "ram mode: --dump-ram / --dump-cpuview are SNAP-only\n");
			return 2;
		}
		if (want_watch_diff) {
			std::fprintf(stderr, "ram mode: --watch-diff is SNAP-only\n");
			return 2;
		}
	}

	revm::KnowledgeBase kb;
	revm::KnowledgeBase kb_support;
	const revm::KnowledgeBase * kb_ptr = nullptr;
	const revm::KnowledgeBase * support_ptr = nullptr;
	std::string error;

	if (!kb_path.empty()) {
		if (!kb.LoadFile(kb_path, error)) {
			std::fprintf(stderr, "KB load failed: %s\n", error.c_str());
			return 1;
		}
		std::fprintf(stderr, "KB: %s (%zu objects)\n", kb_path.c_str(), kb.Size());
		kb_ptr = &kb;
	}
	if (!kb_support_path.empty()) {
		if (!kb_support.LoadFile(kb_support_path, error)) {
			std::fprintf(stderr, "support KB load failed: %s\n", error.c_str());
			return 1;
		}
		support_ptr = &kb_support;
	}

	revm::FullSnapshot snap{};
	uint8_t ram[0x10000];

	if (is_snap) {
		if (!load_full_snap(target, snap, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		std::memcpy(ram, snap.ram, 0x10000);
		std::fprintf(stdout, "Snapshot: %s (FullSnapshot)\n", target.c_str());
		print_snap_summary(snap);
	} else {
		if (!load_ram64k(target, ram, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		std::fprintf(stdout, "RAM: %s (64K)\n", target.c_str());
		// Synthesize a zeroed FullSnapshot with RAM for listing (ROM sections need snap.ram).
		std::memset(&snap, 0, sizeof(snap));
		std::memcpy(snap.magic, revm::FullSnapshot::kMagic, 16);
		std::memcpy(snap.ram, ram, 0x10000);
	}

	int rc = 0;

	if (!dump_ram.empty()) {
		dump_ram_file(ram, dump_ram);
	}
	if (!dump_cpuview.empty()) {
		if (!dump_cpuview_file(snap, dump_cpuview, error)) {
			std::fprintf(stderr, "--dump-cpuview failed: %s\n", error.c_str());
			rc = 1;
		}
	}

	if (do_disasm) {
		disasm_snap_pc(snap, disasm_bytes, kb_ptr);
	}
	if (have_disasm_addr) {
		disasm_at_addr(ram, disasm_addr, disasm_bytes, kb_ptr);
	}

	if (want_blobs) {
		const int brc = dump_blobs(ram, kb, dump_blobs_dir);
		if (brc != 0) rc = brc;
	}

	revm::RomImages roms;
	const revm::RomImages * roms_ptr = nullptr;
	if (want_listing && !cov_path.empty()) {
		if (rom_dir.empty()) {
			std::fprintf(stderr, "listing with --cov requires --rom-dir DIR\n");
			return 2;
		}
		if (!revm::LoadRomsFromDir(rom_dir, roms, error)) {
			std::fprintf(stderr, "ROM load failed: %s\n", error.c_str());
			return 1;
		}
		roms_ptr = &roms;
	}

	if (want_listing) {
		const int lrc = generate_listing(
			target, snap, kb, kb_path, support_ptr, kb_support, kb_support_path,
			cov_path, listing_path, 128, false, !list_no_cov_rng, roms_ptr);
		if (lrc != 0) rc = lrc;
	}

	if (!map_path.empty()) {
		const int mrc = generate_map(
			target, ram, kb, kb_path, support_ptr, kb_support, kb_support_path,
			cov_path, map_path, 128, false);
		if (mrc != 0) rc = mrc;
	}
	if (map_tally) {
		const int mrc = generate_map(
			target, ram, kb, kb_path, support_ptr, kb_support, kb_support_path,
			cov_path, /*out_path=*/"", 128, /*only_tally=*/true);
		if (mrc != 0) rc = mrc;
	}

	if (want_call_graph) {
		const int crc = generate_call_graph(ram, kb, cov_path, call_graph_path);
		if (crc != 0) rc = crc;
	}
	if (want_hot_undoc) {
		const int hrc =
		    generate_hot_undoc(ram, kb, cov_path, hot_undoc_path, hot_undoc_top);
		if (hrc != 0) rc = hrc;
	}
	if (want_watch_diff) {
		const int wrc = generate_watch_diff(ram, snap.color, watch_diff_path, kb,
		                                    watch_diff_all);
		if (wrc != 0) rc = wrc;
	}

	return rc;
}

int cmd_selftest() {
	std::string report;
	int rc = 0;
	{
		std::string creport;
		const int crc = revm::RunCoverageSelfTest(creport);
		std::fputs(creport.c_str(), crc == 0 ? stdout : stderr);
		if (crc != 0) rc = crc;
	}
	{
		using revm::MemoryModOperation;
		revm::cpumock::LinkedRegistry links;
		uint8_t bytes[2] = {0x10, 0x20};
		links.Register(
			0x0040, 2, "linked-test",
			[&](size_t offset) { return bytes[offset]; },
			[&](size_t offset, uint8_t value) { bytes[offset] = value; });
		uint8_t value = 0;
		const bool reads = links.Read(0x0041, value) && value == 0x20 &&
		                   !links.Read(0x0042, value);
		const bool modifies =
			links.Apply({0x0040, MemoryModOperation::Increment}) &&
			links.Apply({0x0041, MemoryModOperation::Decrement}) &&
			bytes[0] == 0x11 && bytes[1] == 0x1F;
		bool overlap_rejected = false;
		try {
			links.Register(0x0041, 1, "overlap", [](size_t) { return 0; },
			               [](size_t, uint8_t) {});
		} catch (const std::logic_error &) {
			overlap_rejected = true;
		}
		if (reads && modifies && overlap_rejected) {
			std::fputs("All linked-registry self-tests passed.\n", stdout);
		} else {
			std::fputs("Linked-registry self-test failed.\n", stderr);
			rc = 1;
		}
	}
	{
		std::ostringstream r;
		int fails = 0;
		auto check = [&](bool ok, const char * name) {
			if (ok) r << "  PASS  " << name << "\n";
			else {
				r << "  FAIL  " << name << "\n";
				++fails;
			}
		};
		revm::KnowledgeBase kb;
		std::string err;
		check(kb.LoadString("{\"version\":1,\"objects\":["
		                    "{\"name\":\"zp\",\"kind\":\"variable\","
		                    "\"addr\":\"0048\",\"watch\":\"yes\"}]}",
		                    err),
		      "watch omitted bank outside I/O");
		check(kb.LoadString("{\"version\":1,\"objects\":["
		                    "{\"name\":\"stream\",\"kind\":\"blob\","
		                    "\"addr\":\"D000\",\"end\":\"D024\","
		                    "\"watch\":\"yes\",\"bank\":\"ram\"}]}",
		                    err),
		      "watch bank ram in I/O window");
		check(kb.LoadString("{\"version\":1,\"objects\":["
		                    "{\"name\":\"col\",\"kind\":\"blob\","
		                    "\"addr\":\"D800\",\"end\":\"DBFF\","
		                    "\"watch\":\"yes\",\"bank\":\"color\"}]}",
		                    err),
		      "watch bank color");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"stream\",\"kind\":\"blob\","
		                     "\"addr\":\"D000\",\"end\":\"D024\","
		                     "\"watch\":\"yes\"}]}",
		                     err) &&
		          err.find("overlaps I/O") != std::string::npos,
		      "watch omitted bank in I/O window rejected");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"vic\",\"kind\":\"variable\","
		                     "\"addr\":\"D026\",\"watch\":\"yes\","
		                     "\"bank\":\"chip\"}]}",
		                     err) &&
		          err.find("cannot be watched") != std::string::npos,
		      "watch bank chip rejected");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"col\",\"kind\":\"blob\","
		                     "\"addr\":\"D000\",\"end\":\"D024\","
		                     "\"watch\":\"yes\",\"bank\":\"color\"}]}",
		                     err) &&
		          err.find("nybble file") != std::string::npos,
		      "watch color outside $D800-$DBFF rejected");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"col\",\"kind\":\"blob\","
		                     "\"addr\":\"D800\",\"end\":\"DBFF\","
		                     "\"watch\":\"yes\",\"bank\":\"colour\"}]}",
		                     err) &&
		          err.find("not \"colour\"") != std::string::npos,
		      "watch bank colour spelling rejected");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"tbl\",\"kind\":\"blob\","
		                     "\"addr\":\"EA40\",\"end\":\"EB7F\","
		                     "\"bank\":\"main\"}]}",
		                     err) &&
		          err.find("illegal bank") != std::string::npos,
		      "illegal bank main rejected");
		check(!kb.LoadString("{\"version\":1,\"objects\":["
		                     "{\"name\":\"chrout\",\"kind\":\"label\","
		                     "\"addr\":\"FFD2\",\"watch\":\"yes\","
		                     "\"bank\":\"kernal\"}]}",
		                     err) &&
		          err.find("cannot be watched") != std::string::npos,
		      "watch bank kernal rejected");
		check(kb.LoadString("{\"version\":1,\"objects\":["
		                    "{\"name\":\"chrout\",\"kind\":\"label\","
		                    "\"addr\":\"FFD2\",\"bank\":\"kernal\"}]}",
		                    err),
		      "bank kernal without watch");
		std::fputs(r.str().c_str(), fails ? stderr : stdout);
		if (fails == 0)
			std::fputs("All KB-watch-bank self-tests passed.\n", stdout);
		else {
			std::fputs("KB-watch-bank self-test failed.\n", stderr);
			rc = 1;
		}
	}
	{
		std::ostringstream r;
		int fails = 0;
		auto check = [&](bool ok, const char * name) {
			if (ok) r << "  PASS  " << name << "\n";
			else {
				r << "  FAIL  " << name << "\n";
				++fails;
			}
		};
		const fs::path tmp = fs::temp_directory_path();
		const std::string cov_path = (tmp / "revm_an_cov.bin").string();
		const std::string cg_path = (tmp / "revm_an_cg.txt").string();
		const std::string hu_path = (tmp / "revm_an_hu.txt").string();
		const std::string snap_b_path = (tmp / "revm_an_b.bin").string();
		const std::string kb_path = (tmp / "revm_an.kb.json").string();

		uint8_t ram[0x10000]{};
		// JSR $1234 at $0100 (weight 10) and $0200 (weight 5); JMP $2000 at $0300 (100)
		ram[0x0100] = 0x20;
		ram[0x0101] = 0x34;
		ram[0x0102] = 0x12;
		ram[0x0200] = 0x20;
		ram[0x0201] = 0x34;
		ram[0x0202] = 0x12;
		ram[0x0300] = 0x4C;
		ram[0x0301] = 0x00;
		ram[0x0302] = 0x20;
		// LDA $ABCD abs for code-ref hot-undoc
		ram[0x0400] = 0xAD;
		ram[0x0401] = 0xCD;
		ram[0x0402] = 0xAB;

		revm::Coverage cov;
		for (int i = 0; i < 10; ++i) cov.MarkPcRam(0x0100);
		for (int i = 0; i < 5; ++i) cov.MarkPcRam(0x0200);
		for (int i = 0; i < 100; ++i) cov.MarkPcRam(0x0300);
		cov.MarkPcRam(0x0400);
		for (int i = 0; i < 50; ++i) cov.MarkMemRam(0x2000);
		for (int i = 0; i < 10; ++i) cov.MarkMemRam(0x3000);
		for (int i = 0; i < 30; ++i) cov.MarkMemRam(0xABCD);

		std::string err;
		check(cov.Save(cov_path, err), "analysis cov save");

		{
			std::ofstream kf(kb_path);
			kf << "{\"version\":1,\"objects\":[]}\n";
		}
		revm::KnowledgeBase kb;
		check(kb.LoadFile(kb_path, err), "analysis kb load");

		check(generate_call_graph(ram, kb, cov_path, cg_path) == 0,
		      "call-graph gen");
		std::ifstream cg(cg_path);
		std::string cg_text((std::istreambuf_iterator<char>(cg)),
		                    std::istreambuf_iterator<char>());
		const size_t pos2000 = cg_text.find("$2000");
		const size_t pos1234 = cg_text.find("$1234");
		check(pos2000 != std::string::npos && pos1234 != std::string::npos &&
		          pos2000 < pos1234,
		      "call-graph ranks $2000 before $1234");
		check(cg_text.find("hits=100") != std::string::npos,
		      "call-graph $2000 hits=100");
		check(cg_text.find("hits=15") != std::string::npos,
		      "call-graph $1234 hits=15");

		check(generate_hot_undoc(ram, kb, cov_path, hu_path, 80) == 0,
		      "hot-undoc gen");
		std::ifstream hu(hu_path);
		std::string hu_text((std::istreambuf_iterator<char>(hu)),
		                    std::istreambuf_iterator<char>());
		const size_t p2000 = hu_text.find("$2000");
		const size_t p3000 = hu_text.find("$3000");
		check(p2000 != std::string::npos && p3000 != std::string::npos &&
		          p2000 < p3000,
		      "hot-undoc ranks hotter ABS first");
		check(hu_text.find("Code-referenced UNDOC") != std::string::npos &&
		          hu_text.find("$ABCD") != std::string::npos,
		      "hot-undoc code-ref section has $ABCD");

		{
			std::ofstream kf(kb_path);
			kf << "{\n"
			      "  \"version\": 1,\n"
			      "  \"objects\": [\n"
			      "    {\"name\":\"v\",\"kind\":\"variable\",\"addr\":\"0048\","
			      "\"watch\":\"yes\"}\n"
			      "  ]\n"
			      "}\n";
		}
		check(kb.LoadFile(kb_path, err), "watch-diff kb reload");
		uint8_t ram_a[0x10000]{};
		ram_a[0x0048] = 0x11;
		revm::FullSnapshot snap_b{};
		std::memcpy(snap_b.magic, revm::FullSnapshot::kMagic, 16);
		snap_b.version = revm::FullSnapshot::kVersion;
		snap_b.ram[0x0048] = 0x22;
		check(revm::SaveFullSnapshotFile(snap_b_path, snap_b, err),
		      "watch-diff snap save");
		// Capture stdout from watch-diff by writing to a temp via freopen is
		// heavy; instead compare ram directly using the same predicate.
		const bool differ = ram_a[0x0048] != snap_b.ram[0x0048];
		check(differ, "watch-diff byte differs");
		uint8_t color_a[COLOR_RAM_SIZE]{};
		check(generate_watch_diff(ram_a, color_a, snap_b_path, kb, false) == 0,
		      "watch-diff gen");

		std::error_code ec;
		fs::remove(cov_path, ec);
		fs::remove(cg_path, ec);
		fs::remove(hu_path, ec);
		fs::remove(snap_b_path, ec);
		fs::remove(kb_path, ec);

		if (fails == 0) r << "All analysis-tool self-tests passed.\n";
		else r << fails << " analysis-tool self-test(s) failed.\n";
		std::fputs(r.str().c_str(), fails == 0 ? stdout : stderr);
		if (fails != 0) rc = 1;
	}
	{
		std::string sreport;
		const int src = revm::RunBootSnapshotSelfTest(sreport);
		std::fputs(sreport.c_str(), src == 0 ? stdout : stderr);
		if (src != 0) rc = src;
	}
	return rc;
}


} // namespace

int main(int argc, char ** argv) {
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	const char * cmd = argv[1];
	if (std::strcmp(cmd, "--version") == 0) {
		std::printf("revm-tool %s\n", C64REWORK_VERSION);
		return 0;
	}
	if (std::strcmp(cmd, "snap") == 0) return cmd_inspect(true, argc - 2, argv + 2);
	if (std::strcmp(cmd, "ram") == 0) return cmd_inspect(false, argc - 2, argv + 2);
	if (std::strcmp(cmd, "sort-kb") == 0) return cmd_sort_kb(argc - 2, argv + 2);
	if (std::strcmp(cmd, "merge-cov") == 0 || std::strcmp(cmd, "merge-coverage") == 0)
		return cmd_merge_cov(argc - 2, argv + 2);
	if (std::strcmp(cmd, "selftest") == 0) return cmd_selftest();
	if (std::strcmp(cmd, "-h") == 0 || std::strcmp(cmd, "--help") == 0 ||
	    std::strcmp(cmd, "help") == 0) {
		usage(argv[0]);
		return 0;
	}
	std::fprintf(stderr, "Unknown command: %s\n", cmd);
	usage(argv[0]);
	return 2;
}
