// Created  : 2026-07-20
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revm {

enum class KbKind {
	Variable,
	Function,
	Label,
	Blob,
	Opcodes,
	Comment, // default when kind omitted — listing/map line note
};

enum class KbWatchLevel {
	No,
	Yes,
	Verbose,
};

// CPU memory bank for an annotation. Omitted / "ram" is the default (game RAM /
// under-ROM RAM as seen in a typical BEGIN dump). Same addr may appear once per
// bank (e.g. RAM trampoline vs KERNAL jump table).
struct KbObject {
	std::string name;
	KbKind kind = KbKind::Comment;
	uint16_t addr = 0;
	std::optional<uint16_t> end; // inclusive; omit = single byte / entry-only
	std::string comment;
	std::string format; // blob format id → assets/FORMATS.md
	KbWatchLevel watch = KbWatchLevel::No;
	bool trace = false; // --trace-calls logs fetches here when true
	// "ram" (default), "kernal", "basic", "char", "chip", or other ROM id.
	std::string bank;

	bool IsRamBank() const { return bank.empty() || bank == "ram"; }
	bool IsChipBank() const { return bank == "chip"; }

	// Inclusive span length in bytes (1 if no end).
	unsigned ByteLength() const {
		const uint16_t hi = end ? *end : addr;
		return unsigned(hi - addr + 1);
	}
};

// Stage-2 knowledge base (*.kb.json). See docs/revm/KB.md.
class KnowledgeBase {
public:
	bool LoadFile(const std::string & path, std::string & error);
	bool LoadString(const std::string & json, std::string & error);
	// Write version:1 + objects (pretty-printed). Omits default/empty fields.
	bool SaveFile(const std::string & path, std::string & error) const;

	void Clear();
	bool Empty() const { return objects_.empty(); }
	size_t Size() const { return objects_.size(); }

	const std::vector<KbObject> & Objects() const { return objects_; }

	// Sort by bank, then addr (range start), then kind, then name (stable).
	void SortByAddress();

	const KbObject * FindByName(const std::string & name) const;
	// Object whose range covers addr. Prefers exact match, then RAM bank over
	// ROM banks (game dumps / disasm are usually RAM-centric).
	const KbObject * FindByAddr(uint16_t addr) const;

private:
	std::vector<KbObject> objects_;
};

} // namespace revm
