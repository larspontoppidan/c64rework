// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include "debug/Coverage.hpp"
#include "debug/InsnBytes6502.hpp"
#include "debug/KnowledgeBase.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace revm {

// Options for KB-aware disassembly (all default on / sensible).
struct DisasmOptions {
	// Emit "# ---- $ADDR — name (N bytes, …)" before each function entry.
	bool emit_function_headers = true;
	// Include auto-generated loc_XXXX function headers.
	bool include_auto = true;
	// Header length when a function has no `end` in the KB.
	unsigned default_bytes = 128;
	// Restrict KB annotations (headers/labels/sync/blobs) to this bank.
	// Empty or "ram" = RAM bank (default Stage-2 listing). "kernal" / "basic" / …
	std::string bank;
	// Optional C64 support KB: used only for operand name fallback when the
	// main KB has no name at an address. Never drives headers/sync/blobs.
	const KnowledgeBase * support_kb = nullptr;
	// Dual listing masks (from listing pre-pass). When both set, emit
	// `; not in coverage range:` at the start of each kb && !play run.
	// Plane: PcRam for bank ram, PcRom otherwise.
	const Coverage * mask_play = nullptr;
	const Coverage * mask_kb = nullptr;
	// Emit `; coverage range:` / `; not in coverage range:` comments.
	bool emit_coverage_comments = true;
};

// Disassemble up to `max_bytes` starting at `start` from a 64K RAM image.
// Returns multi-line text (address  bytes  mnemonic).
// If `kb` is set, emits labels/comments and substitutes named operands.
// RAM `blob` regions are not decoded as code: a `# blob $LO-$HI …` comment is
// emitted at the start of each blob and the bytes are skipped.
std::string Disassemble6502(const uint8_t ram[0x10000], uint16_t start, unsigned max_bytes,
                            const KnowledgeBase * kb = nullptr,
                            const DisasmOptions * opt = nullptr);

// One instruction site that references a memory/code address as its operand.
struct MemRefSite {
	uint16_t pc = 0;
	const char * mnemonic = ""; // e.g. "LDA", "JSR", "BNE"
	const char * kind = "";     // "mem", "jmp", "jsr", "branch", "ind"
};

// addr → sites that reference it (deduped by pc).
using MemRefMap = std::map<uint16_t, std::vector<MemRefSite>>;

// Collect memory addresses referenced as instruction operands in
// [start, start+max_bytes). Includes zp/abs/indexed/indirect bases and
// relative/JSR/JMP targets. Skips immediates and implied/accumulator.
void CollectMemRefs(const uint8_t ram[0x10000], uint16_t start, unsigned max_bytes,
                    MemRefMap & out);

// Same as CollectMemRefs, but only at PCs marked in coverage PC-RAM plane.
void CollectMemRefsCovered(const uint8_t ram[0x10000], const Coverage & cov,
                           MemRefMap & out);

} // namespace revm
