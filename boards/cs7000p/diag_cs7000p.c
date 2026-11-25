// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS diagnostics page lines: the readings of the debug link's `board` section (view_cs7000p.c), short
// enough for the screen, after the DMR modem's counters. No bus traffic: everything is cached elsewhere
#include <stddef.h>
#include "config.h"

#if CONFIG_DIAG	// the whole file
#include "cs7000p.h"
#include "exp_cs7000p.h"
#include "pins.h"
#include "spi_flash/spi_flash.h"
#include "ui/fmt.h"
#if CONFIG_DMR
#include "hrc/dmr_glue.h"
#include "hrc/i2s_pcm.h"
#include "hrc/i2s_dir.h"
#endif

static void put_kv(sbuf_t *b, const char *key, uint32_t v)
{
	sb_str(b, key);
	sb_u32(b, v, 1);
}

static void line_supply(sbuf_t *b)
{
	static const char *const knobs[] = { "?", "on", "off", "noadc" };
	const unsigned k = (unsigned)cs7000p_knob();
	put_kv(b, "bat ", cs7000p_adc_mv(CS7000P_ADC_POW_DET));	// the sense line, then the battery by the model
	put_kv(b, " ", cs7000p_power.battery_mv());
	sb_str(b, "mV ");
	sb_str(b, k < 4u ? knobs[k] : "?");
	sb_str(b, (cs7000p_exp_get() & EXP_POWER_HOLD) != 0 ? " hold" : "");
}

static void line_levels(sbuf_t *b)
{
	put_kv(b, "vdda ", stm32_adc_vdda_mv());
	put_kv(b, " vol ", cs7000p_adc_raw(CS7000P_ADC_VOL));
	put_kv(b, " vox ", cs7000p_adc_raw(CS7000P_ADC_VOX));
}

static void line_chip(sbuf_t *b)
{
	static const char *const clocks[] = { "hse", "hsi-pll", "hsi" };
	const unsigned c = (unsigned)stm32h7_clock_source();
	sb_str(b, "clk ");
	sb_str(b, c < 3u ? clocks[c] : "?");
	sb_str(b, " rev ");
	sb_hex(b, stm32h7_rev_id(), 4);
	sb_str(b, " ld ");
	sb_hex(b, stm32h7_rom_loader_id(), 2);
	sb_str(b, " rdp ");
	sb_hex(b, (FLASH->OPTSR_CUR >> 8) & 0xFFu, 2);
}

static void line_exp(sbuf_t *b)
{
	sb_str(b, "exp ");
	sb_hex(b, cs7000p_exp_get(), 6);
	put_kv(b, " dac ", cs7000p_apc_dac_get());
}

static void line_lcd(sbuf_t *b)
{
	static const char *const boards[] = { "assumed", "unexpected", "forced" };
	cs7000p_display_state_t d;
	cs7000p_display_state(&d);
	sb_str(b, "lcd ");
	sb_str(b, d.lcd_board < 3u ? boards[d.lcd_board] : "?");
	sb_chr(b, ' ');
	sb_hex(b, d.lcd_levels, 2);
}

static void line_panel(sbuf_t *b)
{
	cs7000p_display_state_t d;
	cs7000p_display_state(&d);
	sb_str(b, "pnl ");
	sb_str(b, d.panel.controller != NULL ? d.panel.controller : "none");
	sb_chr(b, ' ');
	for (unsigned i = 0; i < 3u; i++) {
		sb_hex(b, d.panel.id[i], 2);
	}
}

// The raw key scan and the channel selector's code: where a pressed key shows is how the matrix and the selector's
// table are checked on a radio
static void line_keys(sbuf_t *b)
{
	cs7000p_input_state_t k;
	cs7000p_input_state(&k);
	sb_str(b, "keys ");
	for (unsigned i = 0; i < 5u; i++) {
		sb_hex(b, k.rows[i], 2);
	}
	sb_str(b, " ch ");
	sb_hex(b, k.chsw_raw, 1);
	if (k.chsw_pos >= 0) {
		put_kv(b, " p", (uint32_t)k.chsw_pos);
	}
}

static void line_flash(sbuf_t *b)
{
	sb_str(b, "flash ");
	sb_hex(b, stm32_spi_flash.jedec != NULL ? stm32_spi_flash.jedec() : 0u, 6);
	put_kv(b, " ", stm32_spi_flash.size >> 10);
	sb_chr(b, 'K');
	// The accessory's detect line and the Bluetooth section's strap, read behind the debug port's pins
	put_kv(b, " ph", (GPIOA->IDR & GPIO_PIN_13) != 0 ? 1u : 0u);
	put_kv(b, " bt", (GPIOA->IDR & GPIO_PIN_14) != 0 ? 1u : 0u);
}

static void line_cal(sbuf_t *b)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	sb_str(b, r.cal_maker ? "cal maker" : (r.cal_tx ? "cal model" : "cal none"));
	put_kv(b, " txref ", r.tx_refused);	// transmissions refused, and the last reason
	put_kv(b, "/", r.tx_refused_why);
	put_kv(b, " dref ", r.dac_refused);	// DAC writes the interlock turned down
}

static void line_pll(sbuf_t *b)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	sb_str(b, r.pll_locked ? "pll lock " : "pll UNLK ");
	put_kv(b, "", r.vco_hz / 1000u);
	put_kv(b, "k r", r.pll_r);
	put_kv(b, " u", r.unlocks);
}

static void line_rx(sbuf_t *b)
{
	put_kv(b, "rssi ", cs7000p_adc_raw(CS7000P_ADC_RSSI));
	put_kv(b, " nz ", cs7000p_adc_raw(CS7000P_ADC_NOISE));
	put_kv(b, " qt ", cs7000p_adc_raw(CS7000P_ADC_SUBAUDIO));
}

static void line_tune(sbuf_t *b)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	sb_str(b, r.lo_high ? "lo high" : "lo low");
	sb_str(b, r.ak_ok ? " ak ok" : " ak --");
	put_kv(b, " tv ", r.rx_tune);
	sb_str(b, " ro ");
	sb_i32(b, r.ref_offset);
	put_kv(b, " pw ", r.pc8_duty);
}

static void line_hrc(sbuf_t *b)
{
	cs7000p_radio_state_t r;
	cs7000p_radio_state(&r);
	sb_str(b, "C6000 10=");
	if (r.hrc_ok) {
		sb_hex(b, r.hrc_r10, 2);
		sb_str(b, " 25=");
		sb_hex(b, r.hrc_r25, 2);
	} else {
		sb_str(b, "--");
	}
}

static void line_gps(sbuf_t *b)
{
#if CONFIG_GPS
	uint32_t good, bad, n, baud;
	board_gps_counts(&good, &bad, &n, &baud);
	sb_str(b, board_has_gps() ? "gps yes " : "gps no ");
	put_kv(b, "", good);
	put_kv(b, "/", bad);
	put_kv(b, " b", baud);
	sb_str(b, board_gps_running() ? " on" : " off");
#else
	sb_str(b, "gps not built");
#endif
}

#if CONFIG_DMR
// The I2S direction probe's verdict and both counts, and the stream's errors (platform/stm32h7/hrc/i2s_dir.h)
static void line_i2s(sbuf_t *b)
{
	static const char *const dirs[] = { "unknown", "ok", "swapped", "busy", "none" };
	stm32h7_i2s_state_t s;
	stm32h7_i2s_state(&s);
	sb_str(b, s.running ? "i2s on " : "i2s off ");
	sb_str(b, s.dir < 5u ? dirs[s.dir] : "?");
	put_kv(b, " ", s.edges[0]);
	put_kv(b, "/", s.edges[1]);
	put_kv(b, " u", s.underruns);
	put_kv(b, " o", s.overruns);
}

// DMR PCM audio: blocks, overruns, blocks dropped
static void line_audio(sbuf_t *b)
{
	stm32_i2s_pcm_stats_t a;
	stm32_i2s_pcm_stats(&a);
	put_kv(b, "aud blk ", a.blocks);
	put_kv(b, " ov ", a.overruns);
	put_kv(b, " dr ", a.dropped);
}
#endif

static void (*const lines[])(sbuf_t *b) = {
	line_supply, line_levels, line_chip, line_exp, line_lcd, line_panel, line_keys, line_flash, line_cal, line_pll,
	line_rx, line_tune, line_hrc, line_gps,
#if CONFIG_DMR
	line_i2s, line_audio,
#endif
};

bool cs7000p_diag_line(uint8_t i, char *buf, uint8_t len)
{
	sbuf_t b;

	if (len == 0) {
		return false;
	}
#if CONFIG_DMR
	// The DMR modem counters first (bring-up), then the board readings
	if (i < STM32_HRC_DMR_DIAG_LINES) {
		return stm32_hrc_dmr_stats_line(i, buf, len);
	}
	i -= STM32_HRC_DMR_DIAG_LINES;
#endif
	if (i >= sizeof(lines) / sizeof(lines[0])) {
		return false;
	}
	sb_init(&b, buf, len);
	lines[i](&b);
	return true;
}

void cs7000p_diag_reset(void)
{
#if CONFIG_DMR
	stm32_hrc_dmr_stats_reset();
	stm32_i2s_pcm_reset_stats();
#endif
}

#endif	// CONFIG_DIAG
