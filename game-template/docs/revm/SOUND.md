# Sound: SID emulation choices

REVM, the Stage 3/4 builds and the standalone export share the same SID
options. They change only what you hear: Twin compares SID *registers*, never
the audio, so no option here affects a gate.

## Engines

| Option | Engine |
|---|---|
| (none) | Frodo's own SID: light, synthesised in the audio callback |
| `--resid` | reSID, interpolated sampling |
| `--resid-hq` | reSID, FastMem resampling (higher quality, more memory) |
| `--sid-model 6581\|8580` | chip model for either engine (default 6581) |

Game code can make these choices itself; see below.

reSID speaker output keeps about 30 ms of audio buffered and corrects clock
drift between the emulator and the sound card by resampling (in practice
under 0.1%). `--save-audio` and `--state-hashes` take the samples before that
stage, so they are exact.

## The ADSR delay bug

The real SID's envelope generator has a quirk that reSID models faithfully.
Each envelope phase (attack, decay, release) steps when a 15-bit rate counter
reaches that phase's period. When a phase change selects a *shorter* period
than the counter already holds, as in decay → release, the counter does not
reset: it counts up to `0x8000`, wraps, and only then reaches the new period.
The envelope step comes up to about 33 ms late.

Players that gate notes off shortly before the next note-on run into this.
The release starts late, so the note rings on. If the next gate-on comes
inside the delay, the attack waits as well, starting late and from the still
high envelope, so it gives little or no new accent. Notes that should bounce
apart blur together, and the rhythm sounds sloppy. Frantic Freddie's player is
a clear case: it releases each note at 3/4 of its length, with only 20 ms
between gate-off and the next note-on in fast passages. Its tune data is
exact; the slop is the chip. Frodo's simpler SID does not model the quirk,
which is why the same music sounds tight without reSID.

Many later C64 players fight the quirk with a "hard restart" (gate off and
envelope zeroed a couple of frames before each note). Tunes written for such
players barely change under the options below; tunes like Frantic Freddie's
change a lot. Some demoscene music uses envelope timing on purpose, so leave
the real behaviour on for anything that claims to sound like the hardware.

## `--resid-adsr`

| Value | Note-on (attack) | Release | Sound |
|---|---|---|---|
| `real` (default) | chip delay, up to ~33 ms | chip delay | the real chip |
| `fix` | on time | on time | tight, more staccato (close to Frodo's SID) |
| `fix-attack` | on time | chip delay | tight note-ons, notes keep their length |
| `fix-attack-soft` | chip delay ÷ 5 | chip delay | as fix-attack, with a little natural spread |

Measured with the patched reSID: a note decayed, released, and the next
gate-on 20 ms later (attack after, for two rate-counter phases):

| Mode | Attack after |
|---|---|
| `real` | 12,695 / 11,718 cycles (12.9 / 11.9 ms) |
| `fix`, `fix-attack` | 6 cycles (reSID's normal pipeline) |
| `fix-attack-soft` | 2,544 / 2,348 cycles (2.6 / 2.4 ms) |

`fix-attack-soft` keeps the chip's note-to-note variation, five times smaller,
so the voices keep a natural, varying phase instead of starting in lockstep.
The divisor is `adsr_soft_divisor` in the reSID patch.

The option needs `--resid` or `--resid-hq`; with Frodo's SID it does nothing.

## Choosing the sound in game code

A game, typically a remaster, can fix exactly how reSID runs. Call the host
once when the game is installed, for example in Stage 4's `InstallGame`:

```cpp
void InstallGame(CpuMockHost & host) {
	host.SetResid(ResidQuality::High, SidModel::Mos6581);
	host.SetResidAdsr(ResidAdsr::FixAttackSoft);
	...
}
```

- `SetResid(quality, model)` switches to reSID, overriding `--resid`,
  `--resid-hq` and `--sid-model`: `ResidQuality::Normal` is `--resid`,
  `High` is `--resid-hq`; `SidModel::Mos6581` or `Mos8580`. It never turns
  sound on: with `--no-audio`, or a headless run without `--resid`, it does
  nothing and logs `SID: sound is off; the game's reSID choice is not used`.
- `SetResidAdsr(mode)` overrides `--resid-adsr`: `ResidAdsr::Real`, `Fix`,
  `FixAttack` or `FixAttackSoft`.

Each call logs what it set, for example `SID: reSID 6581 (FastMem
resampling), set by the game`. The types are in `sid/ResidOptions.hpp`. The
standalone export keeps both calls (`gamehost::GameHost`), so the exported game
always sounds that way, whatever its command line says. Twin compares SID
registers only, so the calls never affect a gate.

The chip model also changes what the game reads back from the SID's voice 3
registers (`$D41B`), exactly as `--sid-model` does.

Without these calls the command line decides. For example, Frantic Freddie
sounds best to its maintainer with:

```bash
./rework play-game stage4 --main-blank --no-twin --resid-hq --resid-adsr fix-attack-soft
```

## How it is built

reSID is an untouched git submodule (`src/resid`). The framework's change to
it is one patch, `src/revm/patches/resid-adsr-delay-fix.patch`: the REVM
build compiles a patched copy in its build directory, and `./rework export`
vendors the already patched source into the export.
