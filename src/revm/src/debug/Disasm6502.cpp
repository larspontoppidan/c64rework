// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "debug/Disasm6502.hpp"

#include <cstdio>
#include <optional>
#include <set>
#include <string>

namespace revm {
namespace {

enum class Mode : uint8_t {
	Imp, Acc, Imm, Zp, ZpX, ZpY, Abs, AbsX, AbsY, Ind, IndX, IndY, Rel
};

struct OpInfo {
	const char * name;
	Mode mode;
};

// Minimal 6502 table (documented opcodes). Undocumented → ???.
constexpr OpInfo kOps[256] = {
	{"BRK", Mode::Imp}, {"ORA", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ORA", Mode::Zp},   {"ASL", Mode::Zp},  {"???", Mode::Imp},
	{"PHP", Mode::Imp}, {"ORA", Mode::Imm},  {"ASL", Mode::Acc}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ORA", Mode::Abs},  {"ASL", Mode::Abs}, {"???", Mode::Imp},

	{"BPL", Mode::Rel}, {"ORA", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ORA", Mode::ZpX},  {"ASL", Mode::ZpX}, {"???", Mode::Imp},
	{"CLC", Mode::Imp}, {"ORA", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ORA", Mode::AbsX}, {"ASL", Mode::AbsX},{"???", Mode::Imp},

	{"JSR", Mode::Abs}, {"AND", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"BIT", Mode::Zp},  {"AND", Mode::Zp},   {"ROL", Mode::Zp},  {"???", Mode::Imp},
	{"PLP", Mode::Imp}, {"AND", Mode::Imm},  {"ROL", Mode::Acc}, {"???", Mode::Imp},
	{"BIT", Mode::Abs}, {"AND", Mode::Abs},  {"ROL", Mode::Abs}, {"???", Mode::Imp},

	{"BMI", Mode::Rel}, {"AND", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"AND", Mode::ZpX},  {"ROL", Mode::ZpX}, {"???", Mode::Imp},
	{"SEC", Mode::Imp}, {"AND", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"AND", Mode::AbsX}, {"ROL", Mode::AbsX},{"???", Mode::Imp},

	{"RTI", Mode::Imp}, {"EOR", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"EOR", Mode::Zp},   {"LSR", Mode::Zp},  {"???", Mode::Imp},
	{"PHA", Mode::Imp}, {"EOR", Mode::Imm},  {"LSR", Mode::Acc}, {"???", Mode::Imp},
	{"JMP", Mode::Abs}, {"EOR", Mode::Abs},  {"LSR", Mode::Abs}, {"???", Mode::Imp},

	{"BVC", Mode::Rel}, {"EOR", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"EOR", Mode::ZpX},  {"LSR", Mode::ZpX}, {"???", Mode::Imp},
	{"CLI", Mode::Imp}, {"EOR", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"EOR", Mode::AbsX}, {"LSR", Mode::AbsX},{"???", Mode::Imp},

	{"RTS", Mode::Imp}, {"ADC", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ADC", Mode::Zp},   {"ROR", Mode::Zp},  {"???", Mode::Imp},
	{"PLA", Mode::Imp}, {"ADC", Mode::Imm},  {"ROR", Mode::Acc}, {"???", Mode::Imp},
	{"JMP", Mode::Ind}, {"ADC", Mode::Abs},  {"ROR", Mode::Abs}, {"???", Mode::Imp},

	{"BVS", Mode::Rel}, {"ADC", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ADC", Mode::ZpX},  {"ROR", Mode::ZpX}, {"???", Mode::Imp},
	{"SEI", Mode::Imp}, {"ADC", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"ADC", Mode::AbsX}, {"ROR", Mode::AbsX},{"???", Mode::Imp},

	{"STA", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"STY", Mode::Zp},  {"STA", Mode::Zp},   {"STX", Mode::Zp},  {"???", Mode::Imp},
	{"DEY", Mode::Imp}, {"???", Mode::Imp},  {"TXA", Mode::Imp}, {"???", Mode::Imp},
	{"STY", Mode::Abs}, {"STA", Mode::Abs},  {"STX", Mode::Abs}, {"???", Mode::Imp},

	{"BCC", Mode::Rel}, {"STA", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"STY", Mode::ZpX}, {"STA", Mode::ZpX},  {"STX", Mode::ZpY}, {"???", Mode::Imp},
	{"TYA", Mode::Imp}, {"STA", Mode::AbsY}, {"TXS", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"STA", Mode::AbsX}, {"???", Mode::Imp}, {"???", Mode::Imp},

	{"LDY", Mode::Imm}, {"LDA", Mode::IndX}, {"LDX", Mode::Imm}, {"???", Mode::Imp},
	{"LDY", Mode::Zp},  {"LDA", Mode::Zp},   {"LDX", Mode::Zp},  {"???", Mode::Imp},
	{"TAY", Mode::Imp}, {"LDA", Mode::Imm},  {"TAX", Mode::Imp}, {"???", Mode::Imp},
	{"LDY", Mode::Abs}, {"LDA", Mode::Abs},  {"LDX", Mode::Abs}, {"???", Mode::Imp},

	{"BCS", Mode::Rel}, {"LDA", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"LDY", Mode::ZpX}, {"LDA", Mode::ZpX},  {"LDX", Mode::ZpY}, {"???", Mode::Imp},
	{"CLV", Mode::Imp}, {"LDA", Mode::AbsY}, {"TSX", Mode::Imp}, {"???", Mode::Imp},
	{"LDY", Mode::AbsX},{"LDA", Mode::AbsX}, {"LDX", Mode::AbsY},{"???", Mode::Imp},

	{"CPY", Mode::Imm}, {"CMP", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"CPY", Mode::Zp},  {"CMP", Mode::Zp},   {"DEC", Mode::Zp},  {"???", Mode::Imp},
	{"INY", Mode::Imp}, {"CMP", Mode::Imm},  {"DEX", Mode::Imp}, {"???", Mode::Imp},
	{"CPY", Mode::Abs}, {"CMP", Mode::Abs},  {"DEC", Mode::Abs}, {"???", Mode::Imp},

	{"BNE", Mode::Rel}, {"CMP", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"CMP", Mode::ZpX},  {"DEC", Mode::ZpX}, {"???", Mode::Imp},
	{"CLD", Mode::Imp}, {"CMP", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"CMP", Mode::AbsX}, {"DEC", Mode::AbsX},{"???", Mode::Imp},

	{"CPX", Mode::Imm}, {"SBC", Mode::IndX}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"CPX", Mode::Zp},  {"SBC", Mode::Zp},   {"INC", Mode::Zp},  {"???", Mode::Imp},
	{"INX", Mode::Imp}, {"SBC", Mode::Imm},  {"NOP", Mode::Imp}, {"???", Mode::Imp},
	{"CPX", Mode::Abs}, {"SBC", Mode::Abs},  {"INC", Mode::Abs}, {"???", Mode::Imp},

	{"BEQ", Mode::Rel}, {"SBC", Mode::IndY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"SBC", Mode::ZpX},  {"INC", Mode::ZpX}, {"???", Mode::Imp},
	{"SED", Mode::Imp}, {"SBC", Mode::AbsY}, {"???", Mode::Imp}, {"???", Mode::Imp},
	{"???", Mode::Imp}, {"SBC", Mode::AbsX}, {"INC", Mode::AbsX},{"???", Mode::Imp},
};

unsigned operand_bytes(Mode m) {
	switch (m) {
	case Mode::Imp:
	case Mode::Acc:
		return 0;
	case Mode::Imm:
	case Mode::Zp:
	case Mode::ZpX:
	case Mode::ZpY:
	case Mode::IndX:
	case Mode::IndY:
	case Mode::Rel:
		return 1;
	case Mode::Abs:
	case Mode::AbsX:
	case Mode::AbsY:
	case Mode::Ind:
		return 2;
	}
	return 0;
}

const KbObject * named_at(const KnowledgeBase * kb, uint16_t addr) {
	if (!kb) return nullptr;
	const KbObject * o = kb->FindByAddr(addr);
	if (!o || o->name.empty()) return nullptr;
	if (o->addr != addr) return nullptr; // only exact entry labels
	return o;
}

bool object_in_bank(const KbObject & o, const std::string & bank_filter) {
	const std::string want = bank_filter.empty() ? "ram" : bank_filter;
	if (want == "ram") return o.IsRamBank();
	return !o.IsRamBank() && o.bank == want;
}

const char * name_of(const KnowledgeBase * kb, uint16_t addr) {
	const KbObject * o = named_at(kb, addr);
	return o ? o->name.c_str() : nullptr;
}

// Support-KB operand names: chip always OK; ROM-bank symbols only when the
// listing section is that ROM (avoids labeling under-ROM RAM $FFFE as KERNAL).
bool support_name_ok(const KbObject & o, const std::string & bank_filter) {
	const std::string want = bank_filter.empty() ? "ram" : bank_filter;
	if (o.bank == "chip") return true;
	if (want == "ram") return o.IsRamBank();
	return !o.IsRamBank() && o.bank == want;
}

const char * name_of_support(const KnowledgeBase * support_kb, uint16_t addr,
                             const std::string & bank_filter) {
	if (!support_kb) return nullptr;
	const KbObject * exact = nullptr;
	for (const auto & o : support_kb->Objects()) {
		if (o.name.empty() || o.addr != addr) continue;
		if (!support_name_ok(o, bank_filter)) continue;
		exact = &o;
		break;
	}
	return exact ? exact->name.c_str() : nullptr;
}

void format_addr(char * out, size_t n, uint16_t addr, const KnowledgeBase * kb,
                 const KnowledgeBase * support_kb, bool zp,
                 const std::string & bank_filter) {
	if (const char * nm = name_of(kb, addr)) {
		std::snprintf(out, n, "%s", nm);
		return;
	}
	if (const char * nm = name_of_support(support_kb, addr, bank_filter)) {
		std::snprintf(out, n, "%s", nm);
		return;
	}
	if (zp) std::snprintf(out, n, "$%02X", addr & 0xff);
	else std::snprintf(out, n, "$%04X", addr);
}

// Blob covering addr in the active bank. Prefers the blob whose addr == `addr`.
const KbObject * bank_blob_covering(const KnowledgeBase * kb, uint16_t addr,
                                   const std::string & bank_filter) {
	if (!kb) return nullptr;
	const KbObject * hit = nullptr;
	for (const auto & o : kb->Objects()) {
		if (o.kind != KbKind::Blob || !object_in_bank(o, bank_filter)) continue;
		const uint16_t hi = o.end ? *o.end : o.addr;
		if (addr < o.addr || addr > hi) continue;
		if (o.addr == addr) return &o;
		hit = &o;
	}
	return hit;
}

void append_blob_comment(std::string & out, const KbObject & o) {
	const uint16_t hi = o.end ? *o.end : o.addr;
	char line[256];
	std::snprintf(line, sizeof(line), "# blob $%04X-$%04X", o.addr, hi);
	out += line;
	if (!o.name.empty()) {
		out += " — ";
		out += o.name;
	}
	if (!o.format.empty()) {
		out += " (";
		out += o.format;
		out += ")";
	}
	out += "\n";
	if (!o.comment.empty()) {
		out += "; ";
		out += o.comment;
		out += "\n";
	}
}

void append_function_header(std::string & out, const KbObject & o, unsigned default_bytes) {
	const unsigned nbytes = o.end ? o.ByteLength() : default_bytes;
	char line[256];
	out += '\n';
	std::snprintf(line, sizeof(line), "# ---- $%04X — %s (%u bytes%s)\n", o.addr,
	              o.name.c_str(), nbytes,
	              o.end ? "" : ", --bytes fallback");
	out += line;
}

// Code sync points: never let a multi-byte decode step over these addresses.
std::set<uint16_t> collect_sync_points(const KnowledgeBase * kb,
                                       const std::string & bank_filter) {
	std::set<uint16_t> sync;
	if (!kb) return sync;
	for (const auto & o : kb->Objects()) {
		if (!object_in_bank(o, bank_filter) || o.name.empty()) continue;
		if (o.kind == KbKind::Function || o.kind == KbKind::Label) {
			sync.insert(o.addr);
		}
	}
	return sync;
}

// True if any sync point lies in (pc, pc+len) — i.e. would be skipped as an operand.
bool overlaps_sync(const std::set<uint16_t> & sync, uint16_t pc, unsigned len) {
	if (len <= 1 || sync.empty()) return false;
	auto it = sync.upper_bound(pc);
	if (it == sync.end()) return false;
	return *it < uint16_t(pc + len);
}

} // namespace

unsigned InsnBytes6502(uint8_t opcode) {
	return 1u + operand_bytes(kOps[opcode].mode);
}

std::string Disassemble6502(const uint8_t ram[0x10000], uint16_t start, unsigned max_bytes,
                            const KnowledgeBase * kb, const DisasmOptions * opt) {
	DisasmOptions local;
	if (opt) local = *opt;

	std::string out;
	unsigned consumed = 0;
	uint16_t pc = start;
	const uint32_t limit = uint32_t(start) + max_bytes;
	const std::set<uint16_t> sync = collect_sync_points(kb, local.bank);
	std::optional<uint16_t> prev_pc;

	while (consumed < max_bytes && uint32_t(pc) < limit) {
		// Skip KB blobs (data tables embedded in the code span).
		if (const KbObject * blob = bank_blob_covering(kb, pc, local.bank)) {
			// A function/label entry sharing the blob's last byte wins.
			if (sync.count(pc) == 0) {
				if (pc == blob->addr) append_blob_comment(out, *blob);
				const uint16_t hi = blob->end ? *blob->end : blob->addr;
				uint16_t next = uint16_t(hi + 1);
				if (sync.count(hi)) next = hi;
				if (next <= pc) next = uint16_t(pc + 1);
				const unsigned skip = unsigned(next - pc);
				if (skip > max_bytes - consumed) break;
				pc = next;
				consumed += skip;
				prev_pc.reset();
				continue;
			}
		}

		// Hole comment at the first instruction PC inside each kb ∧ ¬play run.
		if (local.emit_coverage_comments && local.mask_play && local.mask_kb) {
			const bool use_rom = !(local.bank.empty() || local.bank == "ram");
			auto hit_play = [&](uint16_t a) {
				return use_rom ? local.mask_play->HitPcRom(a)
				               : local.mask_play->HitPcRam(a);
			};
			auto hit_kb = [&](uint16_t a) {
				return use_rom ? local.mask_kb->HitPcRom(a)
				               : local.mask_kb->HitPcRam(a);
			};
			if (hit_kb(pc) && !hit_play(pc)) {
				uint16_t hole_lo = pc;
				uint16_t hole_hi = pc;
				while (hole_lo > 0 && hit_kb(uint16_t(hole_lo - 1)) &&
				       !hit_play(uint16_t(hole_lo - 1))) {
					--hole_lo;
				}
				while (uint32_t(hole_hi) + 1 < limit &&
				       hit_kb(uint16_t(hole_hi + 1)) &&
				       !hit_play(uint16_t(hole_hi + 1))) {
					++hole_hi;
				}
				// First insn in this hole (prev insn started before the hole).
				if (!prev_pc || *prev_pc < hole_lo) {
					char line[80];
					std::snprintf(line, sizeof(line),
					              "; not in coverage range: $%04X - $%04X\n",
					              hole_lo, hole_hi);
					out += line;
				}
			}
		}

		if (kb) {
			for (const auto & o : kb->Objects()) {
				if (o.addr != pc || !object_in_bank(o, local.bank)) continue;
				// opcodes: force-disasm only (via listing masks); never a symbol.
				if (o.kind == KbKind::Opcodes) continue;
				if (o.kind == KbKind::Function && !o.name.empty() &&
				    local.emit_function_headers) {
					if (local.include_auto || o.name.rfind("loc_", 0) != 0) {
						append_function_header(out, o, local.default_bytes);
					}
				}
				if (!o.name.empty()) {
					out += o.name;
					out += ":\n";
				}
				if (!o.comment.empty()) {
					out += "; ";
					out += o.comment;
					out += "\n";
				}
			}
		}

		const uint8_t op = ram[pc];
		const OpInfo & info = kOps[op];
		const unsigned n_op = operand_bytes(info.mode);
		unsigned len = 1 + n_op;
		// Unmarked data before a known entry: emit .byte so we resync.
		const bool as_data = overlaps_sync(sync, pc, len);
		if (as_data) len = 1;

		char line[160];
		char bytes[16];
		int bpos = 0;
		for (unsigned i = 0; i < len && i < 3; ++i) {
			bpos += std::snprintf(bytes + bpos, sizeof(bytes) - bpos, "%02X%s",
			                      ram[uint16_t(pc + i)], i + 1 < len ? " " : "");
		}

		if (as_data) {
			std::snprintf(line, sizeof(line), "%04X  %-8s  .byte $%02X\n", pc, bytes, op);
			out += line;
		} else {
			char operand[48] = "";
			char addr_buf[40];
			const KnowledgeBase * support = local.support_kb;
			const std::string & bank = local.bank;
			if (info.mode == Mode::Imm) {
				std::snprintf(operand, sizeof(operand), "#$%02X", ram[uint16_t(pc + 1)]);
			} else if (info.mode == Mode::Zp) {
				format_addr(addr_buf, sizeof(addr_buf), ram[uint16_t(pc + 1)], kb, support, true, bank);
				std::snprintf(operand, sizeof(operand), "%s", addr_buf);
			} else if (info.mode == Mode::ZpX) {
				format_addr(addr_buf, sizeof(addr_buf), ram[uint16_t(pc + 1)], kb, support, true, bank);
				std::snprintf(operand, sizeof(operand), "%s,X", addr_buf);
			} else if (info.mode == Mode::ZpY) {
				format_addr(addr_buf, sizeof(addr_buf), ram[uint16_t(pc + 1)], kb, support, true, bank);
				std::snprintf(operand, sizeof(operand), "%s,Y", addr_buf);
			} else if (info.mode == Mode::Abs) {
				const uint16_t a =
				    uint16_t(ram[uint16_t(pc + 1)] | (ram[uint16_t(pc + 2)] << 8));
				format_addr(addr_buf, sizeof(addr_buf), a, kb, support, false, bank);
				std::snprintf(operand, sizeof(operand), "%s", addr_buf);
			} else if (info.mode == Mode::AbsX) {
				const uint16_t a =
				    uint16_t(ram[uint16_t(pc + 1)] | (ram[uint16_t(pc + 2)] << 8));
				format_addr(addr_buf, sizeof(addr_buf), a, kb, support, false, bank);
				std::snprintf(operand, sizeof(operand), "%s,X", addr_buf);
			} else if (info.mode == Mode::AbsY) {
				const uint16_t a =
				    uint16_t(ram[uint16_t(pc + 1)] | (ram[uint16_t(pc + 2)] << 8));
				format_addr(addr_buf, sizeof(addr_buf), a, kb, support, false, bank);
				std::snprintf(operand, sizeof(operand), "%s,Y", addr_buf);
			} else if (info.mode == Mode::Ind) {
				const uint16_t a =
				    uint16_t(ram[uint16_t(pc + 1)] | (ram[uint16_t(pc + 2)] << 8));
				format_addr(addr_buf, sizeof(addr_buf), a, kb, support, false, bank);
				std::snprintf(operand, sizeof(operand), "(%s)", addr_buf);
			} else if (info.mode == Mode::IndX) {
				format_addr(addr_buf, sizeof(addr_buf), ram[uint16_t(pc + 1)], kb, support, true, bank);
				std::snprintf(operand, sizeof(operand), "(%s,X)", addr_buf);
			} else if (info.mode == Mode::IndY) {
				format_addr(addr_buf, sizeof(addr_buf), ram[uint16_t(pc + 1)], kb, support, true, bank);
				std::snprintf(operand, sizeof(operand), "(%s),Y", addr_buf);
			} else if (info.mode == Mode::Rel) {
				const int8_t off = int8_t(ram[uint16_t(pc + 1)]);
				const uint16_t tgt = uint16_t(pc + 2 + off);
				format_addr(addr_buf, sizeof(addr_buf), tgt, kb, support, false, bank);
				std::snprintf(operand, sizeof(operand), "%s", addr_buf);
			} else if (info.mode == Mode::Acc) {
				std::snprintf(operand, sizeof(operand), "A");
			}

			std::snprintf(line, sizeof(line), "%04X  %-8s  %s%s%s\n", pc, bytes, info.name,
			              operand[0] ? " " : "", operand);
			out += line;
		}

		prev_pc = pc;
		pc = uint16_t(pc + len);
		consumed += len;
		if (len == 0) break;
	}
	return out;
}

namespace {

struct MemRefDecode {
	uint16_t ref = 0;
	unsigned len = 1;
	const char * mnemonic = "";
	const char * kind = ""; // mem / jmp / jsr / branch / ind
	bool ok = false;
};

MemRefDecode decode_mem_ref(const uint8_t ram[0x10000], uint16_t pc) {
	MemRefDecode d;
	const uint8_t op = ram[pc];
	const OpInfo & info = kOps[op];
	d.mnemonic = info.name;
	d.len = 1 + operand_bytes(info.mode);

	switch (info.mode) {
	case Mode::Zp:
	case Mode::ZpX:
	case Mode::ZpY:
	case Mode::IndX:
	case Mode::IndY:
		d.ref = ram[uint16_t(pc + 1)];
		d.kind = "mem";
		d.ok = true;
		break;
	case Mode::Abs:
	case Mode::AbsX:
	case Mode::AbsY:
		d.ref = uint16_t(ram[uint16_t(pc + 1)] | (uint16_t(ram[uint16_t(pc + 2)]) << 8));
		if (op == 0x20) d.kind = "jsr";
		else if (op == 0x4C) d.kind = "jmp";
		else d.kind = "mem";
		d.ok = true;
		break;
	case Mode::Ind:
		d.ref = uint16_t(ram[uint16_t(pc + 1)] | (uint16_t(ram[uint16_t(pc + 2)]) << 8));
		d.kind = "ind";
		d.ok = true;
		break;
	case Mode::Rel: {
		const int8_t off = int8_t(ram[uint16_t(pc + 1)]);
		d.ref = uint16_t(pc + 2 + off);
		d.kind = "branch";
		d.ok = true;
		break;
	}
	default:
		break;
	}
	return d;
}

void add_ref(MemRefMap & out, uint16_t addr, const MemRefSite & site) {
	auto & sites = out[addr];
	for (const auto & s : sites) {
		if (s.pc == site.pc) return;
	}
	sites.push_back(site);
}

} // namespace

void CollectMemRefs(const uint8_t ram[0x10000], uint16_t start, unsigned max_bytes,
                    MemRefMap & out) {
	unsigned consumed = 0;
	uint16_t pc = start;
	while (consumed < max_bytes) {
		const MemRefDecode d = decode_mem_ref(ram, pc);
		if (d.ok) {
			add_ref(out, d.ref, MemRefSite{pc, d.mnemonic, d.kind});
		}
		const unsigned len = d.len ? d.len : 1;
		pc = uint16_t(pc + len);
		consumed += len;
	}
}

void CollectMemRefsCovered(const uint8_t ram[0x10000], const Coverage & cov,
                           MemRefMap & out) {
	for (uint32_t pc = 0; pc < 0x10000; ++pc) {
		if (!cov.HitPcRam(uint16_t(pc))) continue;
		const MemRefDecode d = decode_mem_ref(ram, uint16_t(pc));
		if (d.ok) {
			add_ref(out, d.ref, MemRefSite{uint16_t(pc), d.mnemonic, d.kind});
		}
	}
}

} // namespace revm
