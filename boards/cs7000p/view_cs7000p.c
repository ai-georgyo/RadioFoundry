// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS debug link view keys (the `board` section, docs/spec/debuglink.md 3.7; key list in
// docs/spec/hardware-cs7000p.md 15): what a first power-up and boards/cs7000p/hil/test_cs7000p.py need to see
// without a screen. The radio was written from its schematic and has an LCD board no document covers, so the first
// start is read from here: the clock that started, the chip's revision and ROM bootloader, the knob and the supply
// hold, the LCD board check and the panel's ID, the raw keys, the data flash, the calibration state and every
// refusal counter, the synthesizer, the modem and its I2S direction probe.
//
// One key a line with a compound value, on purpose: a section's reply is one frame of about a thousand characters,
// and a line that does not fit is dropped. The line that matters least is last. The maker's factory table and its
// copy are in a second section, `board2` (board.view_split), as the first is near its limit.
#include "config.h"
#if CONFIG_DEBUGLINK	// the whole file
#include <string.h>
#include "cs7000p.h"
#include "exp_cs7000p.h"
#include "store_cs7000p.h"
#include "pins.h"
#include "spi_flash/spi_flash.h"
#include "flash/spi_nor/spi_nor.h"	// SPI_NOR_ADDR4, the board's define and its default
#include "debuglink/dl_fmt.h"
#if CONFIG_DMR
#include "hrc/i2s_dir.h"
#endif

// The board sector (0x01B000, the record of the tree's own copy of the maker's table, hardware-cs7000p.md 10.2), read
// once at the first request (the factory copy writes it at most once, at start, before the debug link can ask):
// blank, a record of the tree's own format (its first four bytes), or someone else's bytes. The record's own state is
// cal_rec's
static const char *store_state(void)
{
	static const char *const names[] = { "unread", "blank", "ours", "foreign" };
	static const char magic[4] = { 'R', 'F', 'F', 'C' };
	static uint8_t state;	// an index into names; 0 until a whole read worked

	if (state == 0 && stm32_spi_flash.read != NULL &&
	    stm32_spi_flash.size >= CS7000P_STORE_CAL_ADDR + CS7000P_STORE_CAL_SIZE) {
		uint8_t chunk[64];
		uint8_t found = 1;
		for (uint32_t off = 0; found == 1 && off < CS7000P_STORE_CAL_SIZE; off += sizeof(chunk)) {
			if (!stm32_spi_flash.read(CS7000P_STORE_CAL_ADDR + off, chunk, sizeof(chunk))) {
				found = 0;	// not known: asked again at the next request
			} else if (off == 0 && memcmp(chunk, magic, sizeof(magic)) == 0) {
				found = 2;
			} else {
				for (unsigned i = 0; i < sizeof(chunk); i++) {
					if (chunk[i] != 0xFFu) {
						found = 3;
					}
				}
			}
		}
		state = found;
	}
	return names[state];
}

static void line_clock(char *buf, uint8_t len)
{
	// The tree the clock set-up ended on: the crystal; the same rates from the internal oscillator through the
	// PLL (the crystal did not start); the bare oscillator (no PLL locked: USB from HSI48, no SPI1-3 kernel clock)
	static const char *const names[] = { "hse", "hsi-pll", "hsi" };
	const unsigned s = (unsigned)stm32h7_clock_source();
	dl_fmt(buf, len, "clock=%s", s < 3u ? names[s] : "?");
}

static void line_mcu_rev(char *buf, uint8_t len)
{
	const uint16_t rev = stm32h7_rev_id();
	if (rev == 0x1003u || rev == 0x2001u || rev == 0x2003u) {
		dl_fmt(buf, len, "mcu_rev=%c", rev == 0x1003u ? 'Y' : rev == 0x2001u ? 'X' : 'V');
	} else {
		dl_fmt(buf, len, "mcu_rev=0x%04x", rev);
	}
}

static void line_rom_loader(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "rom_loader=0x%02x", stm32h7_rom_loader_id());	// 0x91: version 9.1
}

static void line_rdp(char *buf, uint8_t len)
{
	// The read protection byte as the option bytes hold it now: 0xaa level 0, 0xcc level 2, anything else level 1.
	// Read only: nothing in this image writes an option byte
	dl_fmt(buf, len, "rdp=0x%02x", (unsigned)((FLASH->OPTSR_CUR >> 8) & 0xFFu));
}

static void line_exp(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "exp=0x%06x", (unsigned)cs7000p_exp_get());
}

static void line_knob(char *buf, uint8_t len)
{
	static const char *const names[] = { "unknown", "on", "off", "noadc" };
	const unsigned k = (unsigned)cs7000p_knob();
	dl_fmt(buf, len, "knob=%s", k < 4u ? names[k] : "?");
}

static void line_hold(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "hold=%u", (cs7000p_exp_get() & EXP_POWER_HOLD) != 0 ? 1u : 0u);
}

static void line_pow_det(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "pow_det_mv=%u", cs7000p_adc_mv(CS7000P_ADC_POW_DET));
}

static void line_vbat(char *buf, uint8_t len)
{
	// The battery by the model, smoothed as the radio shows it; 0 with the knob off
	dl_fmt(buf, len, "vbat_mv=%u", cs7000p_power.battery_mv());
}

static void line_vdda(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "vdda_mv=%u", stm32_adc_vdda_mv());	// 0: not measured, the nominal figure is in use
}

static void line_vol(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "vol_raw=%u", cs7000p_adc_raw(CS7000P_ADC_VOL));
}

static void line_vox(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "vox_raw=%u", cs7000p_adc_raw(CS7000P_ADC_VOX));
}

static void line_lcd_board(char *buf, uint8_t len)
{
	static const char *const names[] = { "assumed", "unexpected", "forced" };
	cs7000p_display_state_t d;
	cs7000p_display_state(&d);
	dl_fmt(buf, len, "lcd_board=%s levels=0x%02x", d.lcd_board < 3u ? names[d.lcd_board] : "?", d.lcd_levels);
}

static void line_panel(char *buf, uint8_t len)
{
	cs7000p_display_state_t d;
	cs7000p_display_state(&d);
	const hal_display_panel_t *p = &d.panel;
	dl_fmt(buf, len, "panel=%s source=%s id=%02x%02x%02x reg=%02x%02x%02x",
	       p->controller != NULL ? p->controller : "none", p->source != NULL ? p->source : "none", p->id[0], p->id[1],
	       p->id[2], p->id_reg[0], p->id_reg[1], p->id_reg[2]);
}

static void line_keys(char *buf, uint8_t len)
{
	cs7000p_input_state_t k;
	cs7000p_input_state(&k);
	dl_fmt(buf, len, "keys_raw=%02x%02x%02x%02x%02x", k.rows[0], k.rows[1], k.rows[2], k.rows[3], k.rows[4]);
}

static void line_chsw_raw(char *buf, uint8_t len)
{
	cs7000p_input_state_t k;
	cs7000p_input_state(&k);
	dl_fmt(buf, len, "chsw_raw=0x%x", k.chsw_raw);
}

static void line_chsw(char *buf, uint8_t len)
{
	cs7000p_input_state_t k;
	cs7000p_input_state(&k);
	if (k.chsw_pos < 0) {
		dl_fmt(buf, len, "chsw=unverified");	// no position: the selector's code table is not measured
	} else {
		dl_fmt(buf, len, "chsw=%d", k.chsw_pos);
	}
}

// PA13 and PA14 stay the debug port; the nets behind them are read through the port's input register
static void line_phone_det(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "phone_det=%u", (GPIOA->IDR & GPIO_PIN_13) != 0 ? 1u : 0u);
}

static void line_bt_strap(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "bt_strap=%u", (GPIOA->IDR & GPIO_PIN_14) != 0 ? 1u : 0u);
}

static void line_flash(char *buf, uint8_t len)
{
	// Manufacturer, memory type and capacity as the probe read them (EF4019 is the fitted part; 000000: no answer,
	// or a chip left busy or suspended and so not used), and the erases and programs the driver's backstop refused in
	// a sector holding the maker's bytes outside core/storage's grant (0 unless a writer misses the rule)
	const uint32_t id = stm32_spi_flash.jedec != NULL ? stm32_spi_flash.jedec() : 0u;
	dl_fmt(buf, len, "flash=%06X size=0x%x addr4=%u refused=%u", (unsigned)id, (unsigned)stm32_spi_flash.size,
	       (unsigned)(SPI_NOR_ADDR4), (unsigned)stm32_spi_flash_factory_refusals());
}

static void line_store(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "store=%s", store_state());
}

static void line_cal(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "cal=%s", r.cal_tx ? "model" : "none");	// none: every transmission is refused
}

static void line_tx_refused(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "tx_refused=%u why=%u", (unsigned)r.tx_refused, r.tx_refused_why);
}

static void line_dac(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	// The shared DAC's code now (0: off), and the radio's writes the interlock turned down: DAC and expander writes
	// in one count
	dl_fmt(buf, len, "dac=%u refused=%u", cs7000p_apc_dac_get(), (unsigned)r.dac_refused);
}

static void line_lo_side(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "lo_side=%s", r.lo_high ? "high" : "low");
}

static void line_pll(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "pll=%s r=%u vco_hz=%u unlocks=%u", r.pll_locked ? "locked" : "unlocked", r.pll_r,
	       (unsigned)r.vco_hz, (unsigned)r.unlocks);
}

static void line_ak(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "ak=%s", r.ak_ok ? "ok" : "fail");	// the IF receiver's last set-up
}

static void line_rssi(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "rssi_raw=%u", cs7000p_adc_raw(CS7000P_ADC_RSSI));
}

static void line_noise(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "noise_raw=%u", cs7000p_adc_raw(CS7000P_ADC_NOISE));
}

static void line_qt(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "qt_raw=%u", cs7000p_adc_raw(CS7000P_ADC_SUBAUDIO));
}

static void line_rx_tune(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "rx_tune=%u", r.rx_tune);
}

static void line_ref_offset(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "ref_offset=%d", r.ref_offset);
}

static void line_pc8(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	dl_fmt(buf, len, "pc8_duty=%u", r.pc8_duty);
}

static void line_hrc(char *buf, uint8_t len)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	// The modem answered after its set-up, and its 4:0x10 and 4:0x25 as read back then
	dl_fmt(buf, len, "hrc=%u r10=0x%02x r25=0x%02x", r.hrc_ok ? 1u : 0u, r.hrc_r10, r.hrc_r25);
}

#if CONFIG_DMR	// the PCM stream, and with it the probe, is DMR-only code
static void line_i2s(char *buf, uint8_t len)
{
	// The platform's probe of which data pad the I2S block drives (platform/stm32h7/hrc/i2s_dir.h). Until it says
	// ok or swapped the stream does not start: no beeps, no PCM
	static const char *const dirs[] = { "unknown", "ok", "swapped", "busy", "none" };
	stm32h7_i2s_state_t s;
	stm32h7_i2s_state(&s);
	dl_fmt(buf, len, "i2s=%u dir=%s ioswp=%u edges=%u/%u und=%u ovr=%u", s.running ? 1u : 0u,
	       s.dir < 5u ? dirs[s.dir] : "?", s.ioswp ? 1u : 0u, s.edges[0], s.edges[1], (unsigned)s.underruns,
	       (unsigned)s.overruns);
}
#endif

static void line_gps_baud(char *buf, uint8_t len)
{
	uint32_t good, bad, bytes, baud;
	board_gps_counts(&good, &bad, &bytes, &baud);
	dl_fmt(buf, len, "gps_baud=%u", (unsigned)baud);	// 0: not heard (yet), or built without GPS
}

static void line_adc_restarts(char *buf, uint8_t len)
{
	dl_fmt(buf, len, "adc_restarts=%u", (unsigned)stm32_adc_scan_restarts());	// overrun recoveries: 0 is expected
}

// The maker's factory table in use, the factory copy's record, what receive takes from the table and what it holds
// that nothing applies yet (radio_cs7000p.c): the second section's, all of one reply from one copy, the first line's
static void line_cal_table(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(0, buf, len);
}

static void line_cal_rec(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(1, buf, len);
}

static void line_ref(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(2, buf, len);
}

static void line_rx_cal(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(3, buf, len);
}

static void line_sq_maker(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(4, buf, len);
}

static void line_tx_arm(char *buf, uint8_t len)
{
	cs7000p_cal_view_line(5, buf, len);
}

// The `board` section's lines, the bring-up readings, near one reply's size; from CS7000P_VIEW_SPLIT on the
// `board2` section's (board.view_split), keys still board.<key>
static void (*const lines[])(char *buf, uint8_t len) = {
	line_clock, line_mcu_rev, line_rom_loader, line_rdp, line_exp, line_knob, line_hold, line_pow_det, line_vbat,
	line_vdda, line_vol, line_vox, line_lcd_board, line_panel, line_keys, line_chsw_raw, line_chsw, line_phone_det,
	line_bt_strap, line_flash, line_store, line_cal, line_tx_refused, line_dac, line_lo_side, line_pll, line_ak,
	line_rssi, line_noise, line_qt, line_rx_tune, line_ref_offset, line_pc8, line_hrc,
#if CONFIG_DMR
	line_i2s,
#endif
	line_gps_baud, line_adc_restarts,
	// board2
	line_cal_table, line_cal_rec,
	line_ref, line_rx_cal,
	line_sq_maker, line_tx_arm,
};
#define LINES	(sizeof(lines) / sizeof(lines[0]))
// board2 is exactly the table's lines, in cs7000p_cal_view_line's order: its first takes the copy for the others
_Static_assert(LINES == CS7000P_VIEW_SPLIT + CS7000P_CAL_VIEW_LINES,
	       "board.view_split counts the lines before board2's");

bool cs7000p_view_line(uint8_t i, char *buf, uint8_t len)
{
	if (i >= LINES) {
		return false;
	}
	lines[i](buf, len);
	return true;
}

#endif	// CONFIG_DEBUGLINK
