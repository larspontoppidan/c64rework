// Created  : 2026-07-19
// Author   : Lars Ole Pontoppidan
// Project  : REVM - C64rework

#include "snapshot/ChipIo.hpp"

namespace revm {

bool PublicVicIoEqual(const MOS6569State & a, const MOS6569State & b) {
	// $D000–$D02E programmer-visible regs (+ programmed irq_raster).
	// Ignore live raster counter / RST8 (ctrl1 bit7) — those track beam position
	// and differ under QuietVBlank / twin catch-up without meaning a game diverge.
	const uint8_t ctrl1_a = uint8_t(a.ctrl1 & 0x7f);
	const uint8_t ctrl1_b = uint8_t(b.ctrl1 & 0x7f);
	return a.m0x == b.m0x && a.m0y == b.m0y &&
	       a.m1x == b.m1x && a.m1y == b.m1y &&
	       a.m2x == b.m2x && a.m2y == b.m2y &&
	       a.m3x == b.m3x && a.m3y == b.m3y &&
	       a.m4x == b.m4x && a.m4y == b.m4y &&
	       a.m5x == b.m5x && a.m5y == b.m5y &&
	       a.m6x == b.m6x && a.m6y == b.m6y &&
	       a.m7x == b.m7x && a.m7y == b.m7y &&
	       a.mx8 == b.mx8 &&
	       ctrl1_a == ctrl1_b &&
	       a.lpx == b.lpx && a.lpy == b.lpy &&
	       a.me == b.me && a.ctrl2 == b.ctrl2 && a.mye == b.mye &&
	       a.vbase == b.vbase &&
	       a.irq_flag == b.irq_flag && a.irq_mask == b.irq_mask &&
	       a.mdp == b.mdp && a.mmc == b.mmc && a.mxe == b.mxe &&
	       a.mm == b.mm && a.md == b.md &&
	       a.ec == b.ec && a.b0c == b.b0c && a.b1c == b.b1c &&
	       a.b2c == b.b2c && a.b3c == b.b3c &&
	       a.mm0 == b.mm0 && a.mm1 == b.mm1 &&
	       a.m0c == b.m0c && a.m1c == b.m1c && a.m2c == b.m2c &&
	       a.m3c == b.m3c && a.m4c == b.m4c && a.m5c == b.m5c &&
	       a.m6c == b.m6c && a.m7c == b.m7c &&
	       a.irq_raster == b.irq_raster;
}

bool PublicSidIoEqual(const MOS6581State & a, const MOS6581State & b) {
	// $D400–$D418 write regs + pot readbacks. Not fake-v3 / last_sid_* internals.
	return a.freq_lo_1 == b.freq_lo_1 && a.freq_hi_1 == b.freq_hi_1 &&
	       a.pw_lo_1 == b.pw_lo_1 && a.pw_hi_1 == b.pw_hi_1 &&
	       a.ctrl_1 == b.ctrl_1 && a.AD_1 == b.AD_1 && a.SR_1 == b.SR_1 &&
	       a.freq_lo_2 == b.freq_lo_2 && a.freq_hi_2 == b.freq_hi_2 &&
	       a.pw_lo_2 == b.pw_lo_2 && a.pw_hi_2 == b.pw_hi_2 &&
	       a.ctrl_2 == b.ctrl_2 && a.AD_2 == b.AD_2 && a.SR_2 == b.SR_2 &&
	       a.freq_lo_3 == b.freq_lo_3 && a.freq_hi_3 == b.freq_hi_3 &&
	       a.pw_lo_3 == b.pw_lo_3 && a.pw_hi_3 == b.pw_hi_3 &&
	       a.ctrl_3 == b.ctrl_3 && a.AD_3 == b.AD_3 && a.SR_3 == b.SR_3 &&
	       a.fc_lo == b.fc_lo && a.fc_hi == b.fc_hi &&
	       a.res_filt == b.res_filt && a.mode_vol == b.mode_vol &&
	       a.pot_x == b.pot_x && a.pot_y == b.pot_y;
}

bool PublicCiaIoEqual(const MOS6526State & a, const MOS6526State & b,
                      bool compare_ifr) {
	// Programmer-visible CIA registers. Not SC delay pipelines / TOD bookkeeping.
	if (!(a.pra == b.pra && a.ddra == b.ddra &&
	      a.prb == b.prb && a.ddrb == b.ddrb &&
	      a.ta_lo == b.ta_lo && a.ta_hi == b.ta_hi &&
	      a.tb_lo == b.tb_lo && a.tb_hi == b.tb_hi &&
	      a.tod_10ths == b.tod_10ths && a.tod_sec == b.tod_sec &&
	      a.tod_min == b.tod_min && a.tod_hr == b.tod_hr &&
	      a.sdr == b.sdr &&
	      a.cra == b.cra && a.crb == b.crb &&
	      a.ta_latch == b.ta_latch && a.tb_latch == b.tb_latch &&
	      a.ltc_10ths == b.ltc_10ths && a.ltc_sec == b.ltc_sec &&
	      a.ltc_min == b.ltc_min && a.ltc_hr == b.ltc_hr &&
	      a.alm_10ths == b.alm_10ths && a.alm_sec == b.alm_sec &&
	      a.alm_min == b.alm_min && a.alm_hr == b.alm_hr &&
	      a.int_mask == b.int_mask)) {
		return false;
	}
	if (compare_ifr && a.int_flags != b.int_flags) return false;
	return true;
}

bool PublicChipIoEqual(const MOS6569State & vic_a, const MOS6569State & vic_b,
                       const MOS6581State & sid_a, const MOS6581State & sid_b,
                       const MOS6526State & cia1_a, const MOS6526State & cia1_b,
                       const MOS6526State & cia2_a, const MOS6526State & cia2_b,
                       bool compare_sid, bool compare_cia_ifr) {
	if (!PublicVicIoEqual(vic_a, vic_b)) return false;
	if (compare_sid && !PublicSidIoEqual(sid_a, sid_b)) return false;
	return PublicCiaIoEqual(cia1_a, cia1_b, compare_cia_ifr) &&
	       PublicCiaIoEqual(cia2_a, cia2_b, compare_cia_ifr);
}

} // namespace revm
