#!/usr/bin/env python3
"""Mod an original C64 game: patch its BEGIN image and pack it into one PRG.

    c64mod.py build GAME_ROOT [--only NAME[,NAME...]] [--out FILE] [--packer auto|own|exomizer]
    c64mod.py check-boot GAME_ROOT [--prg FILE]
    c64mod.py test GAME_ROOT [NAME...]

The game root holds Mods/mods.toml, the patch sources it lists, and the BEGIN
snapshot (begin/snap.bin). `build`:

1. reads the BEGIN RAM and chip registers from the snapshot (revm-tool);
2. writes build/mods/game.inc: every KB name as a 64tass label;
3. assembles each patch with 64tass and checks it: every byte it writes is in
   its own area or in a hook whose original bytes it declares with `;@expect`
   lines, the declared bytes match the image, no two patches write the same
   byte, and the areas lie in RAM the plays never used (the free map);
4. writes the patched BEGIN image and a patched copy of the snapshot (for
   tests with revm --load-snapshot);
5. packs the image into one PRG with exomizer (when installed) or the
   built-in packer. At RUN the image is decompressed, then the boot stub
   restores the low memory, colour RAM and the chip registers of the BEGIN
   snapshot and jumps to the BEGIN PC.

`check-boot` runs the PRG in revm until the BEGIN PC and compares RAM and chip
registers there with the patched BEGIN state. `test` runs the scenario files
in Mods/tests/: a play derived from each, run headless, with its expectations
checked.

See docs/agents/MOD-ORIGINAL.md for the process.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tomllib

FRAMEWORK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAP_RAM_OFFSET = 28        # FullSnapshot: magic[16], version u16, pad, cycle u32, frame u32
LOADER_ADDR = 0x0801
CYCLES_PER_FRAME = 19656      # revm emulates a PAL C64: 312 lines of 63 cycles

# --------------------------------------------------------------------------
# Small helpers


def die(message):
    print(f"c64mod: {message}", file=sys.stderr)
    sys.exit(2)


def parse_addr(text):
    text = str(text).strip()
    if text.startswith("$"):
        return int(text[1:], 16)
    if text.lower().startswith("0x"):
        return int(text, 16)
    return int(text)


def parse_range(text):
    """"$7100-$73FF" -> (0x7100, 0x7400): start and end (exclusive)."""
    lo, hi = text.split("-")
    return parse_addr(lo), parse_addr(hi) + 1


def fmt_range(lo, hi):
    return f"${lo:04X}-${hi - 1:04X}"


def run(cmd, cwd=None):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        die(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")
    return result.stdout + result.stderr


# --------------------------------------------------------------------------
# The game: configuration, BEGIN state, symbols


class Game:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        with open(os.path.join(self.root, "rework.toml"), "rb") as f:
            self.manifest = tomllib.load(f)
        self.mods_dir = os.path.join(self.root, "Mods")
        config_path = os.path.join(self.mods_dir, "mods.toml")
        if not os.path.isfile(config_path):
            die(f"missing {config_path}")
        with open(config_path, "rb") as f:
            self.config = tomllib.load(f)
        self.build = os.path.join(self.root, "build", "mods")
        os.makedirs(self.build, exist_ok=True)
        self.snapshot = os.path.join(self.root, self.config.get("base", "begin/snap.bin"))
        self.revm = os.path.join(self.root, "bin", "revm")
        self.revm_tool = os.path.join(self.root, "bin", "revm-tool")
        self.kb = os.path.join(self.root, self.manifest["kb_json"])
        rom_dir = self.manifest.get("rom_dir", os.path.join(FRAMEWORK, "roms"))
        self.rom_dir = rom_dir if os.path.isabs(rom_dir) else os.path.join(self.root, rom_dir)
        boot = self.config.get("boot", {})
        self.stub_area = parse_range(boot.get("stub_area", "$CC00-$CFFF"))
        self.copy_area = parse_range(boot.get("copy_area", "$D000-$DFFF"))
        if self.copy_area[0] & 0xFF:
            die("copy_area must start on a page boundary ($xx00)")

    def path(self, name):
        return os.path.join(self.build, name)

    def load_begin(self):
        """BEGIN RAM (64 KB) and chip registers from the snapshot."""
        if not os.path.isfile(self.revm_tool):
            die(f"missing {self.revm_tool}; run ./rework build revm")
        run([self.revm_tool, "snap", self.snapshot, "--dump-ram", self.path("begin_ram.bin"),
             "--dump-chips", self.path("begin_chips.json")])
        with open(self.path("begin_ram.bin"), "rb") as f:
            self.ram = bytearray(f.read())
        with open(self.path("begin_chips.json")) as f:
            self.chips = json.load(f)

    def write_symbols(self):
        """Every KB object as a 64tass label (name = $addr), plus the end of
        ranges as name_end."""
        with open(self.kb) as f:
            objects = json.load(f)["objects"]
        lines = [f"; Generated by c64mod.py from {os.path.relpath(self.kb, self.root)}. Do not edit."]
        seen = set()
        for obj in sorted(objects, key=lambda o: int(o["addr"], 16)):
            name = obj["name"]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or name in seen:
                continue
            seen.add(name)
            lines.append(f"{name:<24} = ${int(obj['addr'], 16):04X}")
            if "end" in obj:
                lines.append(f"{name + '_end':<24} = ${int(obj['end'], 16):04X}")
        with open(self.path("game.inc"), "w") as f:
            f.write("\n".join(lines) + "\n")

    def free_runs(self):
        """Free RAM runs from the coverage of the recorded plays, or None."""
        coverage = os.path.join(self.root, "plays", "merged_cov.bin")
        if not os.path.isfile(coverage):
            return None
        out = self.path("free_map.txt")
        run([self.revm_tool, "snap", self.snapshot, "--kb", self.kb, "--cov", coverage,
             "--free-map", out, "--free-min", "16"])
        runs = []
        with open(out) as f:
            for line in f:
                m = re.match(r"\$([0-9A-F]{4})-\$([0-9A-F]{4})\s", line)
                if m and "KB:" not in line:      # a run with KB objects is not free
                    runs.append((int(m.group(1), 16), int(m.group(2), 16) + 1))
        return runs


def inside_free(lo, hi, runs):
    return any(rlo <= lo and hi <= rhi for rlo, rhi in runs)


# --------------------------------------------------------------------------
# Patches


def read_nonlinear(path):
    """64tass --nonlinear output: [len u16][addr u16][bytes]... ending in len 0."""
    with open(path, "rb") as f:
        data = f.read()
    blocks = []
    pos = 0
    while True:
        length = data[pos] | data[pos + 1] << 8
        if length == 0:
            break
        addr = data[pos + 2] | data[pos + 3] << 8
        blocks.append((addr, bytes(data[pos + 4:pos + 4 + length])))
        pos += 4 + length
    return blocks


# ;@expect $ADDR bytes [comment]: the bytes are whole two-digit hex tokens.
EXPECT = re.compile(r";\s*@expect\s+\$([0-9A-Fa-f]{4})((?:\s+[0-9A-Fa-f]{2}(?![0-9A-Za-z]))+)")


class Patch:
    def __init__(self, game, spec):
        self.name = spec["name"]
        self.source = os.path.join(game.mods_dir, spec["source"])
        self.area = parse_range(spec["area"])
        self.description = spec.get("description", "")
        self.expects = []
        with open(self.source) as f:
            for line in f:
                m = EXPECT.search(line)
                if m:
                    self.expects.append((int(m.group(1), 16), bytes.fromhex(m.group(2))))

    def assemble(self, game):
        out = game.path(f"{self.name}.bin")
        lo, hi = self.area
        # No -a: .text emits the source characters as they are (ASCII);
        # .enc "screen" gives screen codes.
        run(["64tass", "-C", "-n", "-q", "-Wall", "-Wno-implied-reg",
             "-D", f"AREA=${lo:04X}", "-D", f"AREA_END=${hi:04X}",
             "-I", game.build, "-I", game.mods_dir,
             "-L", game.path(f"{self.name}.lst"), "--labels", game.path(f"{self.name}.labels"),
             "-o", out, self.source])
        self.blocks = read_nonlinear(out)

    def check(self, game):
        """Each written byte lies in the area or in an expected hook range,
        and the expected bytes are what the image holds."""
        problems = []
        for addr, expected in self.expects:
            actual = bytes(game.ram[addr:addr + len(expected)])
            if actual != expected:
                problems.append(f"@expect ${addr:04X}: image has {actual.hex(' ')}, "
                                f"the patch expects {expected.hex(' ')}")
        lo, hi = self.area
        for addr, data in self.blocks:
            for a in range(addr, addr + len(data)):
                in_area = lo <= a < hi
                in_hook = any(e <= a < e + len(b) for e, b in self.expects)
                if not (in_area or in_hook):
                    problems.append(f"writes ${a:04X}, outside its area {fmt_range(lo, hi)} "
                                    "and outside every @expect hook")
                    break
        return problems

    def written(self):
        for addr, data in self.blocks:
            for i in range(len(data)):
                yield addr + i


# --------------------------------------------------------------------------
# The packer: an in-place LZ, decompressed backwards (from the top of each
# segment down), with the compressed data below the output.
#
# Token stream, in the order the depacker reads it:
#   $00-$7F  literal run: T+1 bytes follow
#   $80-$BF  match, length (T & $3F) + 3, one byte: distance - 1 (1..256)
#   $C0-$FF  match, length (T & $3F) + 3, two bytes: distance (lo, hi)
# The distance points up: the depacker writes downwards, and a match copies
# bytes written before (above).

MIN_MATCH = 3
MAX_MATCH = 0x3F + 3
MAX_LITERAL = 128


def lz_compress(data):
    """Greedy LZ77 with hash chains over data in the order it is written."""
    n = len(data)
    head = {}
    chain = [-1] * n
    out = bytearray()
    literals = bytearray()

    def flush():
        nonlocal literals
        while literals:
            chunk = literals[:MAX_LITERAL]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            literals = literals[MAX_LITERAL:]

    def insert(i):
        if i + 2 < n:
            key = bytes(data[i:i + 3])
            chain[i] = head.get(key, -1)
            head[key] = i

    i = 0
    while i < n:
        best_len, best_dist = 0, 0
        if i + 2 < n:
            j = head.get(bytes(data[i:i + 3]), -1)
            tries = 0
            while j >= 0 and tries < 512:
                dist = i - j
                if dist > 0xFFFF:
                    break
                length = 0
                while length < MAX_MATCH and i + length < n and data[j + length] == data[i + length]:
                    length += 1
                gain = length - (2 if dist <= 256 else 3)
                best_gain = best_len - (2 if best_dist <= 256 else 3)
                if length >= MIN_MATCH and (best_len == 0 or gain > best_gain):
                    best_len, best_dist = length, dist
                j = chain[j]
                tries += 1
        if best_len >= MIN_MATCH and not (best_len == 3 and best_dist > 256):
            flush()
            if best_dist <= 256:
                out += bytes([0x80 | (best_len - 3), best_dist - 1])
            else:
                out += bytes([0xC0 | (best_len - 3), best_dist & 0xFF, best_dist >> 8])
            for k in range(best_len):
                insert(i + k)
            i += best_len
        else:
            literals.append(data[i])
            insert(i)
            i += 1
    flush()
    return bytes(out)


def lz_events(stream):
    """The depacker's reads and writes: yields ('r', count) and ('w', count)
    in order (count = bytes read or written by one step)."""
    pos = 0
    while pos < len(stream):
        token = stream[pos]
        if token < 0x80:
            yield "r", 1
            for _ in range(token + 1):
                yield "r", 1
                yield "w", 1
            pos += 1 + token + 1
        else:
            extra = 1 if token < 0xC0 else 2
            yield "r", 1 + extra
            yield "w", (token & 0x3F) + 3
            pos += 1 + extra


def lz_decompress(stream, length):
    out = bytearray()
    pos = 0
    while len(out) < length:
        token = stream[pos]
        pos += 1
        if token < 0x80:
            out += stream[pos:pos + token + 1]
            pos += token + 1
        else:
            count = (token & 0x3F) + 3
            if token < 0xC0:
                dist = stream[pos] + 1
                pos += 1
            else:
                dist = stream[pos] | stream[pos + 1] << 8
                pos += 2
            for _ in range(count):
                out.append(out[-dist])
    return bytes(out)


def segments_of(lo_end, holes):
    """The decompressed ranges: $lo_end-$FFFF minus the holes, highest first."""
    ranges = [(lo_end, 0x10000)]
    for hlo, hhi in holes:
        next_ranges = []
        for lo, hi in ranges:
            if hhi <= lo or hlo >= hi:
                next_ranges.append((lo, hi))
                continue
            if lo < hlo:
                next_ranges.append((lo, hlo))
            if hhi < hi:
                next_ranges.append((hhi, hi))
        ranges = next_ranges
    return sorted(ranges, reverse=True)


def in_place_safe(segments, streams, in_start, in_end):
    """True if no write of the depacker lands on input it has not read yet.
    Input is read downwards from in_end; the unread input is
    [in_start, in_end - 1 - consumed]."""
    consumed = 0
    for (lo, hi), stream in zip(segments, streams):
        out = hi
        for kind, count in lz_events(stream):
            if kind == "r":
                consumed += count
                continue
            for _ in range(count):
                out -= 1
                unread_top = in_end - 1 - consumed
                if in_start <= out <= unread_top:
                    return False
    return True


# --------------------------------------------------------------------------
# The PRG: BASIC line, loader, boot stub (depacker + restore), compressed image


def loader_source(stub_lo, payload_pages):
    return f"""; Generated by c64mod.py: BASIC line and loader.
        * = ${LOADER_ADDR:04X}
        .word basic_end, 10
        .byte $9e
        .text format("%d", start)
        .byte 0
basic_end
        .word 0
start   sei
        cld
        lda #$0b                ; screen off: no bad lines, faster depacking
        sta $d011
; Block NMIs (the RESTORE key) while the KERNAL is out: one CIA2 timer NMI,
; never acknowledged, holds the NMI line low, and the NMI input reacts only
; to edges. The restore stub's read of $DD0D lets go.
        lda #<nmi_rti
        sta $0318
        lda #>nmi_rti
        sta $0319
        lda #$7f
        sta $dd0d
        lda #$00
        sta $dd0e
        lda #$02
        sta $dd04
        lda #$00
        sta $dd05
        lda #$81                ; timer A NMI on
        sta $dd0d
        lda #$19                ; force load, one shot, start
        sta $dd0e
        ldx #$10
wait    dex                     ; the NMI comes here
        bne wait
        ldx #0
copy    .for page = 0, page < {payload_pages}, page += 1
        lda payload + page * 256,x
        sta ${stub_lo:04X} + page * 256,x
        .endfor
        inx
        bne copy
        jmp ${stub_lo:04X}
nmi_rti rti
payload
"""


def depacker_source(game, segments, in_end):
    """The depacker of the own packer: all segments, then on to restore."""
    lines = [f"""; Depacker: the segments of the image, backwards, from the input below.
inp     = $f8                   ; next input byte is at inp - 1
outp    = $fa                   ; next output byte goes to outp - 1
src     = $fc
stop    = $fe
len     = $f7

        * = ${game.stub_area[0]:04X}
boot    lda #$34                ; all RAM while depacking
        sta $01
        lda #<${in_end:04X}
        sta inp
        lda #>${in_end:04X}
        sta inp+1
        ldx #0
nextseg lda segments,x
        sta outp
        lda segments+1,x
        sta outp+1
        lda segments+2,x
        sta stop
        lda segments+3,x
        sta stop+1
        txa
        pha
        jsr depack
        pla
        clc
        adc #4
        tax
        cpx #{len(segments) * 4}
        bne nextseg
        jmp restore

depack  ldy #0
loop    lda outp
        cmp stop
        bne token
        lda outp+1
        cmp stop+1
        bne token
        rts
token   jsr getbyte
        tax
        bmi match
literal jsr getbyte
        jsr putbyte
        dex
        bpl literal
        bmi loop
match   and #$3f
        clc
        adc #3
        sta len
        cpx #$c0
        bcs long
        jsr getbyte             ; distance - 1
        sta src
        lda #0
        sta src+1
        sec                     ; + 1
        bcs add
long    jsr getbyte
        sta src
        jsr getbyte
        sta src+1
        clc
add     lda src
        adc outp
        sta src
        lda src+1
        adc outp+1
        sta src+1
        ldx len
copy    lda src
        bne +
        dec src+1
+       dec src
        lda (src),y
        jsr putbyte
        dex
        bne copy
        beq loop

getbyte lda inp
        bne +
        dec inp+1
+       dec inp
        lda (inp),y
        rts

putbyte pha
        lda outp
        bne +
        dec outp+1
+       dec outp
        pla
        sta (outp),y
        rts

segments"""]
    for lo, hi in segments:
        lines.append(f"        .word ${hi & 0xFFFF:04X}, ${lo:04X}    ; {fmt_range(lo, hi)}")
    return "\n".join(lines) + "\n"


def restore_source(game, low_end, origin=None):
    """The restore part of the boot stub, entered with any $01 and with the
    image in RAM: copy back low memory and colour RAM, write the chip
    registers, set the CPU state and jump to BEGIN."""
    chips = game.chips
    cpu = chips["cpu"]
    copy_lo = game.copy_area[0]
    color_copy = copy_lo + low_end
    vic = list(chips["vic"])
    # $D011 bit 7 and $D012 read the beam; written, they set the raster
    # interrupt line.
    irq_raster = chips["vic_irq_raster"]
    vic[0x11] = (vic[0x11] & 0x7F) | ((irq_raster & 0x100) >> 1)
    vic[0x12] = irq_raster & 0xFF
    lines = []
    if origin is not None:
        lines.append(f"        * = ${origin:04X}")
    lines.append(f"""
; Low memory $0002-${low_end - 1:04X} comes back from its copy at ${copy_lo:04X}.
; Nothing below uses the zero page or the stack until the CPU state is set.
restore sei
        lda #$34                ; all RAM: the copies lie under the I/O area
        sta $01
        ldx #2
lowcopy lda ${copy_lo:04X},x
lowdst  sta @w $0000,x            ; absolute: the high byte is stepped
        inx
        bne lowcopy
        inc lowcopy+2
        inc lowdst+2
        lda lowdst+2
        cmp #>${low_end:04X}
        bne lowcopy

; Colour RAM from its copy at ${color_copy:04X}.
        ldx #0
color   lda #$34
        sta $01
colsrc  lda ${color_copy:04X},x
        ldy #$35
        sty $01
coldst  sta $d800,x
        inx
        bne color
        inc colsrc+2
        inc coldst+2
        lda coldst+2
        cmp #$dc
        bne color

; Chip registers of the BEGIN snapshot ($01 = $35: I/O visible).
        ldx #$2e
vicset  lda victab,x
        sta $d000,x
        dex
        bpl vicset
        ldx #$18
sidset  lda sidtab,x
        sta $d400,x
        dex
        bpl sidset
""")
    for base, name in ((0xDC00, "cia1"), (0xDD00, "cia2")):
        lines.append(f"; {name.upper()}")
        lines.append(cia_code(base, chips[name]))
    sp = cpu["sp"] & 0xFF
    lines.append(f"""
; CPU: port, stack, registers, flags; then BEGIN.
        lda #${cpu['ddr']:02X}
        sta $00
        lda #${cpu['pr']:02X}
        sta $01
        ldx #${(sp - 1) & 0xFF:02X}
        txs
        lda #${cpu['p']:02X}          ; the flags, pulled by PLP (a dead stack byte)
        sta ${0x100 + sp:04X}
        lda #${cpu['a']:02X}
        ldx #${cpu['x']:02X}
        ldy #${cpu['y']:02X}
        plp
        jmp ${cpu['pc']:04X}
""")
    lines.append("victab  .byte " + ", ".join(f"${v:02X}" for v in vic))
    lines.append("sidtab  .byte " + ", ".join(f"${v:02X}" for v in chips["sid"]))
    lines.append(f"""
        .cerror * > ${game.stub_area[1]:04X}, "boot stub does not fit its area"
""")
    return "\n".join(lines)


def cia_code(base, cia):
    """Registers of one CIA: interrupts off, ports, the timer counters (as
    latch + force load), the control registers (which start the timers that
    ran), then the real latches (a running timer does not reload on a latch
    write), and last the interrupt mask.

    Not restored: a stopped timer's counter when it differs from its latch
    (a latch write loads a stopped counter), the exact phase of a running
    timer (it starts some 200 cycles before BEGIN), pending interrupt flags
    (acknowledged; check-boot lists them) and the time of day."""
    b = base
    lines = [
        f"        lda #$7f",
        f"        sta ${b + 0x0D:04X}             ; interrupts off",
        f"        lda #$00",
        f"        sta ${b + 0x0E:04X}             ; timers stopped",
        f"        sta ${b + 0x0F:04X}",
        f"        lda ${b + 0x0D:04X}             ; acknowledge (CIA2: lets the NMI line go)",
        f"        lda #${cia['ddra']:02X}",
        f"        sta ${b + 0x02:04X}",
        f"        lda #${cia['ddrb']:02X}",
        f"        sta ${b + 0x03:04X}",
        f"        lda #${cia['pra']:02X}",
        f"        sta ${b + 0x00:04X}",
        f"        lda #${cia['prb']:02X}",
        f"        sta ${b + 0x01:04X}",
    ]
    timers = (("ta", 0x04, 0x0E), ("tb", 0x06, 0x0F))
    for timer, reg, ctrl in timers:
        counter = cia[f"{timer}_counter"]
        lines += [
            f"        lda #${counter & 0xFF:02X}              ; counter {counter}",
            f"        sta ${b + reg:04X}",
            f"        lda #${counter >> 8:02X}",
            f"        sta ${b + reg + 1:04X}",
            f"        lda #$10                ; force load",
            f"        sta ${b + ctrl:04X}",
        ]
    lines += [
        f"        lda #${cia['crb'] & 0xEF:02X}",
        f"        sta ${b + 0x0F:04X}",
        f"        lda #${cia['cra'] & 0xEF:02X}",
        f"        sta ${b + 0x0E:04X}",
    ]
    for timer, reg, ctrl in timers:
        latch = cia[f"{timer}_latch"]
        lines += [
            f"        lda #${latch & 0xFF:02X}              ; latch {latch}",
            f"        sta ${b + reg:04X}",
            f"        lda #${latch >> 8:02X}",
            f"        sta ${b + reg + 1:04X}",
        ]
    lines += [
        f"        lda #${0x80 | cia['int_mask']:02X}",
        f"        sta ${b + 0x0D:04X}             ; interrupt mask",
    ]
    return "\n".join(lines)


def assemble(game, source_text, name, raw=True):
    src = game.path(f"{name}.asm")
    out = game.path(f"{name}.bin")
    with open(src, "w") as f:
        f.write(source_text)
    cmd = ["64tass", "-C", "-a", "-q", "-L", game.path(f"{name}.lst"), "-o", out, src]
    if raw:
        cmd.insert(1, "-b")
    run(cmd)
    with open(out, "rb") as f:
        return f.read()


def pack_own(game, image, prg_path):
    """Pack the image with the built-in in-place LZ packer."""
    stub_lo, stub_hi = game.stub_area
    copy_lo, copy_hi = game.copy_area
    if copy_lo < stub_hi and stub_lo < copy_hi:
        die("the copy area overlaps the boot stub area")
    probe_segments = segments_of(0x0C00, [game.stub_area])
    stub_size = len(assemble(game, depacker_source(game, probe_segments, 0x1000)
                             + restore_source(game, 0x0C00), "stub"))
    payload_pages = (stub_size + 255) // 256
    if stub_lo + payload_pages * 256 > stub_hi:
        die(f"boot stub ({stub_size} bytes) does not fit {fmt_range(stub_lo, stub_hi)}")
    loader = assemble(game, loader_source(stub_lo, payload_pages), "loader", raw=False)
    in_start = LOADER_ADDR + len(loader) - 2 + payload_pages * 256

    # Low memory up to low_end comes back from the copy area; the depacked
    # segments start at low_end, which must clear the compressed input.
    low_end = (in_start + 0xFF) & 0xFF00
    while True:
        if low_end + 0x400 > copy_hi - copy_lo:
            die(f"low memory $0002-${low_end - 1:04X} and colour RAM do not fit the copy area "
                f"{fmt_range(copy_lo, copy_hi)}")
        if copy_lo < low_end:
            die(f"the copy area {fmt_range(copy_lo, copy_hi)} lies in the low memory it restores")
        work = bytearray(image)
        work[copy_lo:copy_lo + low_end] = image[0:low_end]
        work[copy_lo + low_end:copy_lo + low_end + 0x400] = bytes(game.chips["color"])
        segments = segments_of(low_end, [game.stub_area])
        streams = [lz_compress(work[lo:hi][::-1]) for lo, hi in segments]
        for (lo, hi), stream in zip(segments, streams):
            assert lz_decompress(stream, hi - lo) == bytes(work[lo:hi][::-1])
        payload = b"".join(streams)[::-1]
        in_end = in_start + len(payload)
        if in_end > stub_lo:
            die("the compressed image reaches into the boot stub area")
        if in_place_safe(segments, streams, in_start, in_end):
            break
        low_end += 0x100
    stub = assemble(game, depacker_source(game, segments, in_end)
                    + restore_source(game, low_end), "stub")
    if len(stub) > payload_pages * 256:
        die("the boot stub grew past its probed size")
    stub = stub.ljust(payload_pages * 256, b"\0")
    with open(prg_path, "wb") as f:
        f.write(loader + stub + payload)
    return dict(packer="own", low_end=low_end, segments=segments, in_start=in_start,
                in_end=in_end, size=len(loader) + len(stub) + len(payload), stub_size=stub_size)


# --------------------------------------------------------------------------
# Commands


def patched_image(game, only=None):
    """Assemble and check the enabled patches (or only the named ones) and
    apply them to the BEGIN image. Returns the image and the patches."""
    patches = [Patch(game, spec) for spec in game.config.get("patch", [])
               if spec.get("enabled", True)]
    if only is not None:
        wanted = set(only)
        unknown = wanted - {p.name for p in patches}
        if unknown:
            die(f"unknown patch(es): {', '.join(sorted(unknown))}")
        patches = [p for p in patches if p.name in wanted]

    free = game.free_runs()
    if free is None:
        print("c64mod: warning: no plays/merged_cov.bin, so the areas are not checked against "
              "free RAM (./rework generate-cov PLAY...)", file=sys.stderr)
    problems = []
    owner = {}
    reserved = [("boot stub", game.stub_area), ("copy area", game.copy_area)]
    for patch in patches:
        patch.assemble(game)
        problems += [f"{patch.name}: {p}" for p in patch.check(game)]
        lo, hi = patch.area
        for what, (rlo, rhi) in reserved:
            if lo < rhi and rlo < hi:
                problems.append(f"{patch.name}: area {fmt_range(lo, hi)} overlaps the {what}")
            for addr, expected in patch.expects:
                if addr < rhi and rlo < addr + len(expected):
                    problems.append(f"{patch.name}: hook ${addr:04X} lies in the {what}")
        if free is not None and not inside_free(lo, hi, free):
            problems.append(f"{patch.name}: area {fmt_range(lo, hi)} is not free RAM in "
                            "build/mods/free_map.txt")
        for a in patch.written():
            if a in owner:
                problems.append(f"{patch.name}: ${a:04X} is also written by {owner[a]}")
                break
            owner[a] = patch.name
    for what, (lo, hi) in reserved:
        if free is not None and not inside_free(lo, hi, free):
            problems.append(f"{what} {fmt_range(lo, hi)} is not free RAM in build/mods/free_map.txt")
    if problems:
        die("patch check failed:\n  " + "\n  ".join(problems))

    image = bytearray(game.ram)
    for patch in patches:
        for addr, data in patch.blocks:
            image[addr:addr + len(data)] = data
    return image, patches


def write_snapshot(game, image, path):
    """The BEGIN snapshot with its RAM replaced by image."""
    with open(game.snapshot, "rb") as f:
        snap = bytearray(f.read())
    if snap[SNAP_RAM_OFFSET:SNAP_RAM_OFFSET + 0x10000] != game.ram:
        die(f"{game.snapshot}: the RAM is not at offset {SNAP_RAM_OFFSET}; "
            "the snapshot format has changed (update SNAP_RAM_OFFSET)")
    snap[SNAP_RAM_OFFSET:SNAP_RAM_OFFSET + 0x10000] = image
    with open(path, "wb") as f:
        f.write(snap)


def build(args):
    game = Game(args.game_root)
    game.load_begin()
    game.write_symbols()
    image, patches = patched_image(game, args.only.split(",") if args.only else None)
    with open(game.path("patched_ram.bin"), "wb") as f:
        f.write(image)
    write_snapshot(game, image, game.path("patched_snap.bin"))

    out = args.out or game.config.get("output", "mod.prg")
    prg_path = out if os.path.isabs(out) else os.path.join(game.root, out)
    info = pack(game, image, prg_path, args.packer)
    no_exomizer = args.packer == "auto" and not find_exomizer()
    with open(game.path("build.json"), "w") as f:
        json.dump(dict(patches=[p.name for p in patches], prg=prg_path,
                       begin_pc=game.chips["cpu"]["pc"],
                       **{k: v for k, v in info.items() if k != "segments"}), f, indent=1)

    print(f"c64mod: {game.config.get('name', 'mod')}")
    for patch in patches:
        size = sum(len(d) for a, d in patch.blocks if patch.area[0] <= a < patch.area[1])
        hooks = ", ".join(f"${a:04X}" for a, _ in patch.expects) or "none"
        print(f"c64mod: patch {patch.name}: {size} bytes in {fmt_range(*patch.area)}, hooks {hooks}")
    print(f"c64mod: {info['packer']} packer: {info['size']:,} bytes -> {prg_path}")
    if no_exomizer:
        print("c64mod: (exomizer is not installed; the built-in packer was used)")
    print(f"c64mod: patched snapshot {game.path('patched_snap.bin')}")
    return 0


def find_exomizer():
    return os.environ.get("EXOMIZER") or shutil.which("exomizer")


def pack(game, image, prg_path, packer):
    if packer == "auto":
        packer = "exomizer" if find_exomizer() else "own"
    if packer == "exomizer":
        return pack_exomizer(game, image, prg_path)
    return pack_own(game, image, prg_path)


def write_prg(path, addr, data):
    with open(path, "wb") as f:
        f.write(bytes([addr & 0xFF, addr >> 8]) + bytes(data))


def pack_exomizer(game, image, prg_path):
    """Pack with exomizer's self-extractor. exomizer depacks the image from
    $0800 and the restore stub, then jumps to the stub, which copies back
    $0002-$07FF (where exomizer and the KERNAL may have worked: zero page,
    stack, $0334 table, screen) and colour RAM, and sets up the chips."""
    exomizer = find_exomizer()
    if not exomizer:
        die("exomizer not found: install it (or set EXOMIZER=/path/to/exomizer), "
            "or use --packer own")
    low_end = 0x0800
    copy_lo, copy_hi = game.copy_area
    if low_end + 0x400 > copy_hi - copy_lo:
        die(f"low memory and colour RAM do not fit the copy area {fmt_range(copy_lo, copy_hi)}")
    if copy_lo < low_end:
        die(f"the copy area {fmt_range(copy_lo, copy_hi)} lies in the low memory it restores")
    work = bytearray(image)
    work[copy_lo:copy_lo + low_end] = image[0:low_end]
    work[copy_lo + low_end:copy_lo + low_end + 0x400] = bytes(game.chips["color"])
    stub = assemble(game, restore_source(game, low_end, origin=game.stub_area[0]), "restore")
    inputs = []
    for lo, hi in segments_of(low_end, [game.stub_area]):
        path = game.path(f"image_{lo:04x}.prg")
        write_prg(path, lo, work[lo:hi])
        inputs.append(path)
    stub_path = game.path("restore.prg")
    write_prg(stub_path, game.stub_area[0], stub)
    inputs.append(stub_path)
    options = game.config.get("boot", {}).get("exomizer_options", ["-x3"])
    cmd = [exomizer, "sfx", f"0x{game.stub_area[0]:04x}", "-t", "64", *options, "-o", prg_path, *inputs]
    log = run(cmd)
    with open(game.path("exomizer.log"), "w") as f:
        f.write(" ".join(cmd) + "\n" + log)
    return dict(packer="exomizer", low_end=low_end, size=os.path.getsize(prg_path),
                stub_size=len(stub))


def check_boot(args):
    """Run the PRG to the BEGIN PC and compare with the patched BEGIN state."""
    game = Game(args.game_root)
    with open(game.path("build.json")) as f:
        info = json.load(f)
    prg = args.prg or info["prg"]
    pc = info["begin_pc"]
    common = [game.revm, "--load-prg", prg, "--rom-dir", game.rom_dir, "--headless", "--no-audio"]
    log = run(common + ["--max-frames", str(args.max_frames), "--watch-main-pc", f"{pc:04X}",
                        "--log-verbose", "0"],
              cwd=game.root)
    # The hit that came from the boot stub's jump (code at the BEGIN PC may
    # run earlier, in the loader or the KERNAL).
    cycle = None
    stub_lo, stub_hi = game.stub_area
    for m in re.finditer(r"\[main=\d+:(\d+)[^\]]*\][^\n]*PC=\$" + f"{pc:04X}" +
                         r" hit=#\d+\n[^\n]* from \$([0-9A-F]{4})", log):
        if stub_lo <= int(m.group(2), 16) < stub_hi:
            cycle = int(m.group(1))
            break
    if cycle is None:
        die(f"the PRG never jumped from its boot stub to the BEGIN PC ${pc:04X} "
            f"within {args.max_frames} frames")
    snap = game.path("boot_snap.bin")
    run(common + ["--max-frames", str(args.max_frames), "--max-cycles", str(cycle + 1),
                  "--save-snapshot", str(cycle), snap],
        cwd=game.root)
    run([game.revm_tool, "snap", snap, "--dump-ram", game.path("boot_ram.bin"),
         "--dump-chips", game.path("boot_chips.json")])
    with open(game.path("boot_ram.bin"), "rb") as f:
        ram = f.read()
    with open(game.path("patched_ram.bin"), "rb") as f:
        want = f.read()
    with open(game.path("boot_chips.json")) as f:
        chips = json.load(f)
    with open(game.path("begin_chips.json")) as f:
        begin = json.load(f)
    # The boot stub stays in its area; the copy area holds the copies; the
    # stack byte below BEGIN's stack pointer carried the flags.
    ignore = set(range(*game.stub_area)) | set(range(*game.copy_area)) | {0, 1}
    ignore.add(0x100 + begin["cpu"]["sp"])
    diffs = [a for a in range(0x10000) if a not in ignore and ram[a] != want[a]]
    print(f"check-boot: BEGIN PC ${pc:04X} reached at cycle {cycle:,} "
          f"(frame {cycle // CYCLES_PER_FRAME:,})")
    if diffs:
        print(f"check-boot: RAM differs at {len(diffs)} bytes, first ${diffs[0]:04X} "
              f"(PRG ${ram[diffs[0]]:02X}, image ${want[diffs[0]]:02X})")
    else:
        print("check-boot: RAM identical to the patched BEGIN image (outside the stub and "
              "copy areas and the flags byte)")
    chip_diffs, notes = compare_chips(chips, begin)
    for d in chip_diffs:
        print(f"check-boot: {d}")
    if not chip_diffs:
        print("check-boot: CPU, VIC (with the raster interrupt line), SID, CIA ports, latches, "
              "control registers and masks, and colour RAM as at BEGIN")
    for note in notes:
        print(f"check-boot: note: {note}")
    return 1 if diffs or chip_diffs else 0


# --------------------------------------------------------------------------
# Scenario tests: a play derived from a scenario file, run headless in revm


def parse_value(value):
    return parse_addr(value) if isinstance(value, str) else int(value)


def kb_or_addr(game, text):
    """An address: $hex, or the name of a KB object."""
    if text.startswith("$") or text[:1].isdigit():
        return parse_addr(text)
    with open(game.kb) as f:
        for obj in json.load(f)["objects"]:
            if obj["name"] == text:
                return int(obj["addr"], 16)
    die(f"unknown KB name {text}")


def held_at(events, frame):
    """The inputs held at a frame: the last event at or before it."""
    held = set()
    for f in sorted(events):
        if f > frame:
            break
        held = events[f]
    return held


def scenario_play(game, spec, info):
    """The play JSON for a scenario: source, inputs (a recorded play's input
    events with the scenario's added) and pokes as mod_events."""
    import hashlib
    with open(game.path("begin_chips.json")) as f:
        begin = json.load(f)
    roms = None
    plays_dir = os.path.join(game.root, "plays")
    if not os.path.isdir(plays_dir):
        die("no plays/ folder: scenario tests need a recorded play for the ROM hashes")
    for name in sorted(os.listdir(plays_dir)):
        if name.endswith(".json"):
            with open(os.path.join(plays_dir, name)) as f:
                roms = json.load(f).get("roms")
            if roms:
                break
    if not roms:
        die("no recorded play in plays/ to take the ROM hashes from")
    if spec.get("source", "snapshot") == "snapshot":
        path = spec.get("_snapshot") or game.path("patched_snap.bin")
        with open(path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        start_frame, start_cycle = begin["frame"], begin["cycle"]
        source = dict(type="snapshot", path=path, sha256=sha)
        snapshots = [dict(cycle=start_cycle, frame=start_frame, sha256=sha)]
    else:
        path = info["prg"]
        with open(path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        start_frame, start_cycle = 0, 0
        source = dict(type="prg", path=path, sha256=sha)
        snapshots = []

    recorded_events = {}
    mods = {}
    if spec.get("inputs_from"):
        # The recorded play's inputs and memory modifications.
        name = spec["inputs_from"]
        play_path = os.path.join(plays_dir, name if name.endswith(".json") else name + ".json")
        with open(play_path) as f:
            recorded = json.load(f)
        recorded_events = {int(f): set(held) for f, held in recorded["input_events"].items()}
        mods = {frame: list(ops) for frame, ops in recorded.get("mod_events", {}).items()}
    # An event lists everything held from that frame on. The scenario's
    # inputs come on top of what the recording holds at the time; a
    # scenario release lets go of the scenario's inputs only.
    scenario_events = {int(f): set(held) for f, held in spec.get("inputs", {}).items()}
    events = {}
    for frame in sorted(set(recorded_events) | set(scenario_events)):
        held = held_at(recorded_events, frame) | held_at(scenario_events, frame)
        events[str(frame)] = sorted(held)

    for frame, pokes in spec.get("pokes", {}).items():
        ops = mods.setdefault(str(frame), [])
        for addr, count in pokes.items():
            op = "increment" if count > 0 else "decrement"
            ops += [dict(address=kb_or_addr(game, addr), operation=op)] * abs(count)
    mods = dict(sorted(mods.items(), key=lambda kv: int(kv[0])))
    return dict(format_version=1, source=source,
                recorded_on=dict(no_twin=False, main_blank=False,
                                 details="c64mod scenario (not a recording)"),
                rand_seed=42, roms=roms,
                start_frame=start_frame, start_cycle=start_cycle, end_frame=int(spec["end_frame"]),
                timestamps=[], input_events=events, mod_events=mods, snapshots=snapshots)


def run_scenario(game, path, info):
    with open(path) as f:
        spec = json.load(f)
    name = os.path.splitext(os.path.basename(path))[0]
    out = game.path(os.path.join("tests", name))
    os.makedirs(out, exist_ok=True)
    image_path = game.path("patched_ram.bin")
    if "patches" in spec:
        # Only the patches under test: another patch may change the timing
        # (and with it the CIA-based randomness) a recorded play relies on.
        if spec.get("source", "snapshot") != "snapshot":
            die(f"{name}: a patch list needs source snapshot (the PRG has every patch)")
        if not hasattr(game, "ram"):
            game.load_begin()
            game.write_symbols()
        image, _ = patched_image(game, spec["patches"])
        image_path = os.path.join(out, "patched_ram.bin")
        with open(image_path, "wb") as f:
            f.write(image)
        spec["_snapshot"] = os.path.join(out, "patched_snap.bin")
        write_snapshot(game, image, spec["_snapshot"])
    play = scenario_play(game, spec, info)
    play_path = os.path.join(out, "play.json")
    with open(play_path, "w") as f:
        json.dump(play, f, indent=1)

    pcs = sorted({e["pc"].lstrip("$").upper() for e in spec.get("expect", []) if "pc" in e})
    timeline_path = os.path.join(out, "timeline.jsonl")
    cmd = [game.revm, "--use-play", play_path, "--rom-dir", game.rom_dir, "--headless",
           "--kb", game.kb, "--watch-timeline", timeline_path]
    if spec.get("audio") or spec.get("video"):
        cmd += ["--resid"]
    else:
        cmd += ["--no-audio"]
    if spec.get("audio"):
        cmd += ["--save-audio", os.path.join(out, "audio.wav")]
    if spec.get("video"):
        cmd += ["--save-video", os.path.join(out, "video.mp4")]
    if pcs:
        cmd += ["--watch-main-pc", ",".join(pcs), "--log-verbose", "0"]
    for frame in spec.get("screens", []):
        cmd += ["--save-screen", str(frame), os.path.join(out, f"frame{frame}.png")]
    log = run(cmd, cwd=game.root)
    with open(os.path.join(out, "revm.log"), "w") as f:
        f.write(log)
    hits = {}
    for m in re.finditer(r"\[main=(\d+):\d+[^\]]*\][^\n]*PC=\$([0-9A-F]{4}) hit=", log):
        hits.setdefault(m.group(2), []).append(int(m.group(1)))
    with open(game.kb) as f:
        kb_addr = {o["name"]: int(o["addr"], 16) for o in json.load(f)["objects"]}

    snapshots = {}

    def memory_at(frame):
        """RAM at a frame, from a snapshot of a second run of the same play:
        at the start cycle plus whole frames, i.e. at the raster position
        where the play starts (BEGIN's for a snapshot, the top for a PRG)."""
        if frame not in snapshots:
            cycle = play["start_cycle"] + (frame - play["start_frame"]) * CYCLES_PER_FRAME
            snap = os.path.join(out, f"frame{frame}.snap")
            sid = ["--resid"] if spec.get("audio") or spec.get("video") else ["--no-audio"]
            run([game.revm, "--use-play", play_path, "--rom-dir", game.rom_dir, "--headless",
                 *sid, "--max-frames", str(frame + 1), "--save-snapshot", str(cycle), snap],
                cwd=game.root)
            ram_path = os.path.join(out, f"frame{frame}.ram")
            chips_path = os.path.join(out, f"frame{frame}.chips.json")
            run([game.revm_tool, "snap", snap, "--dump-ram", ram_path, "--dump-chips", chips_path])
            with open(ram_path, "rb") as f:
                memory = bytearray(f.read())
            # Expectations read $D800-$DBFF as colour RAM, not the RAM under it.
            with open(chips_path) as f:
                memory[0xD800:0xDC00] = bytes(json.load(f)["color"])
            snapshots[frame] = bytes(memory)
        return snapshots[frame]

    failures = 0
    print(f"test {name}: {spec.get('description', '')}")
    for e in spec.get("expect", []):
        if "pc" in e:
            pc = e["pc"].lstrip("$").upper()
            frames = [f for f in hits.get(pc, [])
                      if e.get("after", 0) <= f <= e.get("before", 1 << 30)]
            ok = bool(frames) == e.get("hit", True)
            window = f" in frames {e.get('after', 'start')}-{e.get('before', 'end')}"
            text = (f"PC ${pc} {'hit' if frames else 'not hit'}{window}"
                    + (f" (first at frame {frames[0]})" if frames else "")
                    + f", want {'hit' if e.get('hit', True) else 'not hit'}")
        else:
            frame = int(e["frame"])
            if "var" in e:
                if e["var"] not in kb_addr:
                    die(f"{name}: unknown KB name {e['var']}")
                addr, label = kb_addr[e["var"]], e["var"]
            else:
                addr = parse_addr(e["addr"])
                label = f"${addr:04X}"
            want = (bytes.fromhex(e["bytes"]) if "bytes" in e
                    else bytes([parse_value(e["is"])]))
            got = memory_at(frame)[addr:addr + len(want)]
            ok = got == want
            text = f"{label} at frame {frame} is {got.hex(' ')}, want {want.hex(' ')}"
        print(f"  {'PASS' if ok else 'FAIL'}  {text}")
        failures += 0 if ok else 1
    for frame in spec.get("screens", []):
        print(f"  screen {os.path.join(out, f'frame{frame}.png')}")
    if spec.get("audio"):
        print(f"  audio  {os.path.join(out, 'audio.wav')}")
    if spec.get("video"):
        print(f"  video  {os.path.join(out, 'video.mp4')}")
    return failures


def test(args):
    game = Game(args.game_root)
    with open(game.path("build.json")) as f:
        info = json.load(f)
    tests_dir = os.path.join(game.mods_dir, "tests")
    names = args.names or sorted(n[:-5] for n in os.listdir(tests_dir) if n.endswith(".json"))
    failures = 0
    for name in names:
        failures += run_scenario(game, os.path.join(tests_dir, name + ".json"), info)
    print(f"c64mod: {len(names)} scenario(s), {failures} failed expectation(s)")
    return 1 if failures else 0


def compare_chips(got, want):
    """Differences in the restored chip state, and notes on BEGIN state the
    boot stub cannot recreate."""
    out = []
    notes = []
    for key in ("a", "x", "y", "p", "pc", "sp", "ddr", "pr"):
        if got["cpu"][key] != want["cpu"][key]:
            out.append(f"cpu.{key} = ${got['cpu'][key]:02X}, BEGIN ${want['cpu'][key]:02X}")
    # Read-only or moving VIC registers: raster, light pen, interrupt
    # latch, collisions; $D011 bit 7 reads the beam.
    for reg in range(0x2F):
        if reg in (0x12, 0x13, 0x14, 0x19, 0x1E, 0x1F):
            continue
        mask = 0x7F if reg == 0x11 else 0xFF
        if (got["vic"][reg] ^ want["vic"][reg]) & mask:
            out.append(f"VIC $D0{reg:02X} = ${got['vic'][reg]:02X}, BEGIN ${want['vic'][reg]:02X}")
    if got["vic_irq_raster"] != want["vic_irq_raster"]:
        out.append(f"VIC raster interrupt line {got['vic_irq_raster']}, "
                   f"BEGIN {want['vic_irq_raster']}")
    if want["vic"][0x19] & 0x0F:
        notes.append(f"VIC interrupt flags ${want['vic'][0x19] & 0x0F:X} pending at BEGIN, "
                     "acknowledged by the boot stub")
    for reg in range(0x19):
        if got["sid"][reg] != want["sid"][reg]:
            out.append(f"SID $D4{reg:02X} = ${got['sid'][reg]:02X}, BEGIN ${want['sid'][reg]:02X}")
    for name in ("cia1", "cia2"):
        g, w = got[name], want[name]
        for key in ("pra", "prb", "ddra", "ddrb", "ta_latch", "tb_latch", "cra", "crb", "int_mask"):
            if g[key] != w[key]:
                out.append(f"{name}.{key} = {g[key]}, BEGIN {w[key]}")
        for timer, ctrl in (("ta", "cra"), ("tb", "crb")):
            stopped = not (w[ctrl] & 1)
            if stopped and w[f"{timer}_counter"] != w[f"{timer}_latch"]:
                notes.append(f"{name} timer {timer[1].upper()} is stopped at BEGIN with counter "
                             f"{w[f'{timer}_counter']} and latch {w[f'{timer}_latch']}: "
                             f"restored as {g[f'{timer}_counter']}")
        if w["int_flags"] & 0x1F:
            notes.append(f"{name} interrupt flags ${w['int_flags'] & 0x1F:02X} pending at BEGIN, "
                         "acknowledged by the boot stub")
    color = [i for i in range(0x400) if got["color"][i] != want["color"][i]]
    if color:
        out.append(f"colour RAM differs at {len(color)} cells, first ${0xD800 + color[0]:04X}")
    return out, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="patch the BEGIN image and pack it into a PRG")
    b.add_argument("game_root")
    b.add_argument("--only", default="", help="comma-separated patch names (default: all enabled)")
    b.add_argument("--out", default="", help="PRG path (default: output in Mods/mods.toml)")
    b.add_argument("--packer", choices=("auto", "own", "exomizer"), default="auto",
                   help="auto: exomizer when it is installed, else the built-in packer")
    c = sub.add_parser("check-boot", help="run the PRG to BEGIN and compare with the image")
    c.add_argument("game_root")
    c.add_argument("--prg", default="")
    c.add_argument("--max-frames", type=int, default=600)
    t = sub.add_parser("test", help="run scenario tests from Mods/tests/*.json")
    t.add_argument("game_root")
    t.add_argument("names", nargs="*", help="scenario names (default: all)")
    args = parser.parse_args()
    if shutil.which("64tass") is None:
        die("64tass is required (apt install 64tass)")
    if args.command == "build":
        return build(args)
    if args.command == "test":
        return test(args)
    return check_boot(args)


if __name__ == "__main__":
    sys.exit(main())
