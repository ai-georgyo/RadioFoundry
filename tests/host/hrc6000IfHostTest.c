// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000 on a board that feeds it a 450 kHz IF (drivers/modem/hrc6000/if_host): tones off before the first DMR
// entry; the receive I/Q order follows the first LO's side at init, on a mode change and on a retune; the receive
// hold of the DAC control word after every set-up, and no trace of it on a board that does not ask for it; the codec
// input bits after a DMR entry and an FM transmit set-up; the modulation pair and one receiver restart on a new DMR
// channel; FM transmit refused without a pair; the FM receive audio path either way; every bus failure counted, and
// a chip that is not there found by its mode register on a bus that reports nothing. The held reference's other
// coding (one per receive state, 4:0x26 only when asked) and a receive 4:0x04 of the board's own; the board's FM
// transmit rows, after the set-up and written back at its end (page 4 then as without them); with those options
// left NULL, or answering what NULL means, every transaction as before they existed (a hash of the log).
// Against the fake register file of hrc6000ToneTest.c. Built a second time without DMR (hrc6000IfHostTestFm)
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "modem/hrc6000/if_host/hrc6000_if_host.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

// --- the fake chip ---

static uint8_t regs[8][256];
static uint8_t ext[8][0x800];
static uint32_t n_xfer;			// transactions, failed ones too
static uint32_t n_w[256], n_r[256];	// page 4 writes and reads by register
static uint32_t seq_w[256];		// the transaction that last wrote a page 4 register
static int fail_at = -1;		// the transaction that fails, counted from the last fake_reset; -1: none
static bool fail_all;
static int fail_w = -1, fail_r = -1;	// a page 4 register whose writes / reads fail
static int silent = -1;			// no chip on the bus: every read gives this level, writes are lost; -1: a chip
static uint32_t log_hash;		// FNV-1a over every transaction's header and written bytes, in order
static struct { uint8_t reg, val; } wl[8192];	// page 4 writes in order, n_wl of them
static uint32_t n_wl;

static void log_bytes(const uint8_t *p, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		log_hash = (log_hash ^ p[i]) * 16777619u;
	}
}

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	uint8_t page = hdr[0] & 7u;
	bool read = (hdr[0] & 0x80u) != 0;
	uint32_t k = n_xfer++;
	log_bytes(hdr, hdr_len);
	if (!read && tx != NULL) {
		log_bytes(tx, len);
	}
	if (fail_all || (int)k == fail_at) {
		return false;
	}
	if (silent >= 0) {
		if (read && rx != NULL) {
			memset(rx, silent, len);
		}
		return true;	// a bus without an acknowledge has no way to tell
	}
	if (hdr_len == 3) {
		uint16_t a = (uint16_t)(hdr[1] | ((uint16_t)hdr[2] << 8));
		for (uint16_t i = 0; i < len; i++) {
			if (read) {
				rx[i] = ext[page][(a + i) & 0x7FFu];
			} else {
				ext[page][(a + i) & 0x7FFu] = tx[i];
			}
		}
		return true;
	}
	if (page == 4 && len == 1 && (int)hdr[1] == (read ? fail_r : fail_w)) {
		return false;
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t r = (uint8_t)(hdr[1] + i);
		if (read) {
			if (rx != NULL) {
				rx[i] = regs[page][r];
			}
			n_r[r] += page == 4;
		} else {
			regs[page][r] = tx != NULL ? tx[i] : 0xFFu;
			n_w[r] += page == 4;
			if (page == 4) {
				seq_w[r] = k;
				if (n_wl < sizeof(wl) / sizeof(wl[0])) {
					wl[n_wl].reg = r;
					wl[n_wl].val = regs[page][r];
					n_wl++;
				}
			}
		}
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, NULL };
static hrc_t dev;

// --- the fake board ---

#define HZ_A		435000000u
#define HZ_B		446500000u
#define HZ_HIGH_SIDE	470000000u	// from here up the fake board's first LO is above the signal
#define SAFE_GAIN	0x80u

static bool side_flip;		// a bench switch: the other LO side on every frequency
static bool have_dmr_pair, have_fm_pair;
static int n_enter, n_leave, n_restart;
static uint8_t at_enter[256];	// page 4 as the engine finds it when it is switched on
static uint8_t at_leave[256];	// and when it is switched off
static int ctx_seen, ctx_bad;	// callbacks called with the board's context, and with anything else

static void see_ctx(const void *ctx)
{
	if (ctx == &ctx_seen) {
		ctx_seen++;
	} else {
		ctx_bad++;
	}
}

static bool lo_high(void *ctx, uint32_t hz)
{
	see_ctx(ctx);
	return (hz >= HZ_HIGH_SIDE) != side_flip;
}

// A pair that tells the frequency and the mode apart
static uint8_t pair_byte(uint32_t hz, bool dmr, bool mod1)
{
	return (uint8_t)(hz / 1000000u % 100u + (dmr ? 0x00u : 0x80u) + (mod1 ? 0x40u : 0x00u));
}

static bool mod_pair(void *ctx, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1)
{
	see_ctx(ctx);
	*mod2 = 0xEE;	// what a careless board leaves behind when it has no pair
	*mod1 = 0xEE;
	if (!(dmr ? have_dmr_pair : have_fm_pair)) {
		return false;
	}
	*mod2 = pair_byte(hz, dmr, false);
	*mod1 = pair_byte(hz, dmr, true);
	return true;
}

static int8_t ref_offset(void *ctx, uint32_t hz)
{
	see_ctx(ctx);
	return (int8_t)(hz == HZ_A ? -5 : hz == HZ_B ? 9 : 0);
}

// The reference coding and the receive 4:0x04, as a bench knob would set them
static uint8_t coding_now, adc_now;
static bool r04_own;		// rx_tx_offset answers r04_val, else ref_offset's value
static int8_t r04_val;
static bool r04_dmr;		// the mode rx_tx_offset was last asked for
static int n_coding, n_r04;

static uint8_t ref_coding(void *ctx, uint8_t *adc_ctl)
{
	see_ctx(ctx);
	n_coding++;
	*adc_ctl = adc_now;
	return coding_now;
}

static int8_t rx_tx_offset(void *ctx, uint32_t hz, bool dmr)
{
	see_ctx(ctx);
	n_r04++;
	r04_dmr = dmr;
	return r04_own ? r04_val : ref_offset(ctx, hz);
}

static void cb_enter(void)
{
	n_enter++;
	memcpy(at_enter, regs[4], sizeof(at_enter));
}

static void cb_leave(void)
{
	n_leave++;
	memcpy(at_leave, regs[4], sizeof(at_leave));
}

static void cb_restart(void) { n_restart++; }

// A board whose reference and microphone hang on the chip, with no analog FM path
static const hrc6000_if_cfg_t cfg_hold = {
	.dmr_mic_gain = 7, .rx_gain_db = 2, .fm_audio_chip = true, .ref_dac_hold = true, .e0_set = 0x02, .e0_clear = 0x40,
	.lo_high = lo_high, .mod_pair = mod_pair, .ref_offset = ref_offset,
	.dmr_enter = cb_enter, .dmr_leave = cb_leave, .dmr_rx_restart = cb_restart, .safe_gain = SAFE_GAIN, .ctx = &ctx_seen,
};
// A board with an analog FM path and nothing of its own on the chip's DAC or codec inputs
static const hrc6000_if_cfg_t cfg_plain = {
	.dmr_mic_gain = 7, .rx_gain_db = -3, .lo_high = lo_high, .mod_pair = mod_pair, .ref_offset = ref_offset,
	.dmr_enter = cb_enter, .dmr_leave = cb_leave, .dmr_rx_restart = cb_restart, .safe_gain = SAFE_GAIN, .ctx = &ctx_seen,
};
// Nothing but the values
static const hrc6000_if_cfg_t cfg_bare = { .dmr_mic_gain = 7, .safe_gain = SAFE_GAIN };
// cfg_hold with the reference coding and the receive 4:0x04 asked of the board
static const hrc6000_if_cfg_t cfg_opts = {
	.dmr_mic_gain = 7, .rx_gain_db = 2, .fm_audio_chip = true, .ref_dac_hold = true, .e0_set = 0x02, .e0_clear = 0x40,
	.lo_high = lo_high, .mod_pair = mod_pair, .ref_offset = ref_offset, .ref_coding = ref_coding,
	.rx_tx_offset = rx_tx_offset,
	.dmr_enter = cb_enter, .dmr_leave = cb_leave, .dmr_rx_restart = cb_restart, .safe_gain = SAFE_GAIN, .ctx = &ctx_seen,
};
// The board's FM transmit rows (a calibration's registers, invented here), as the callback gives them: rows_n of
// rows_at, or none while rows_on is false
static const hrc6000_if_row_t rows_narrow[] = {
	{ 4, 0x3F, 0x04 }, { 4, 0x35, 0x20 }, { 4, 0x34, 0xBC }, { 4, 0x3E, 0x06 }, { 4, 0xE4, 0x20 },
};
static const hrc6000_if_row_t rows_wide[] = {
	{ 4, 0x3F, 0x07 }, { 4, 0x35, 0x20 }, { 4, 0x34, 0xBC }, { 4, 0x3E, 0x06 }, { 4, 0xE4, 0x22 },
};
#define ROWS_N	((uint8_t)(sizeof(rows_narrow) / sizeof(rows_narrow[0])))
static hrc6000_if_row_t rows_many[HRC6000_IF_TX_ROWS_MAX + 1u];
// A row on a register the transmit set-up never writes: only the host's write-back brings it back
static const hrc6000_if_row_t rows_free[] = { { 4, 0x3B, 0x5A } };
static bool rows_on, rows_too_many, rows_untouched;
static uint32_t rows_hz;
static bool rows_wide_asked;
static int n_rows_asked;

static const hrc6000_if_row_t *fm_tx_rows(void *ctx, uint32_t hz, bool wide, uint8_t *n)
{
	see_ctx(ctx);
	n_rows_asked++;
	rows_hz = hz;
	rows_wide_asked = wide;
	*n = 0;
	if (!rows_on) {
		return NULL;
	}
	if (rows_too_many) {
		*n = (uint8_t)(sizeof(rows_many) / sizeof(rows_many[0]));
		return rows_many;
	}
	if (rows_untouched) {
		*n = 1;
		return rows_free;
	}
	*n = ROWS_N;
	return wide ? rows_wide : rows_narrow;
}

// cfg_opts with the rows
static const hrc6000_if_cfg_t cfg_rows = {
	.dmr_mic_gain = 7, .rx_gain_db = 2, .fm_audio_chip = true, .ref_dac_hold = true, .e0_set = 0x02, .e0_clear = 0x40,
	.lo_high = lo_high, .mod_pair = mod_pair, .ref_offset = ref_offset, .ref_coding = ref_coding,
	.rx_tx_offset = rx_tx_offset, .fm_tx_rows = fm_tx_rows,
	.dmr_enter = cb_enter, .dmr_leave = cb_leave, .dmr_rx_restart = cb_restart, .safe_gain = SAFE_GAIN, .ctx = &ctx_seen,
};
// cfg_plain with the same two: they are for the hold, and never asked without it
static const hrc6000_if_cfg_t cfg_plain_opts = {
	.dmr_mic_gain = 7, .rx_gain_db = -3, .lo_high = lo_high, .mod_pair = mod_pair, .ref_offset = ref_offset,
	.ref_coding = ref_coding, .rx_tx_offset = rx_tx_offset,
	.dmr_enter = cb_enter, .dmr_leave = cb_leave, .dmr_rx_restart = cb_restart, .safe_gain = SAFE_GAIN, .ctx = &ctx_seen,
};

static void fake_reset(void)
{
	memset(regs, 0, sizeof(regs));
	memset(ext, 0, sizeof(ext));
	memset(n_w, 0, sizeof(n_w));
	memset(n_r, 0, sizeof(n_r));
	memset(seq_w, 0, sizeof(seq_w));
	memset(at_enter, 0, sizeof(at_enter));
	memset(at_leave, 0, sizeof(at_leave));
	n_xfer = 0;
	n_wl = 0;
	log_hash = 2166136261u;
	coding_now = HRC6000_IF_REF_MID;
	adc_now = 0;
	r04_own = false;
	fail_at = fail_w = fail_r = silent = -1;
	fail_all = false;
	side_flip = false;
	have_dmr_pair = have_fm_pair = true;
	n_enter = n_leave = n_restart = 0;
	hrc6000_attach(&dev, &bus);
}

static bool held(uint8_t r25)
{
	return (r25 & 0x2Au) == 0x08u;	// bit 3 set, bits 5 and 1 clear: the DAC on in receive, as measured
}

// 4:0x47, 0x48 and 0x04 as hrc6000_set_ref_offset leaves them for a receive offset
static bool ref_is(int8_t off)
{
	return regs[4][0x47] == (uint8_t)off && regs[4][0x04] == (uint8_t)off && regs[4][0x48] == (off < 0 ? 0x03 : 0x00);
}

// The same under the receive hold: 0x04 as the driver leaves it, and the held DAC's level in the two-point bias, ten
// bits unsigned, mid-scale plus the offset. Never the offset alone: that is the DAC at 0 V
static bool ref_held(int8_t off)
{
	uint16_t level = (uint16_t)(512 + off);
	return regs[4][0x04] == (uint8_t)off && regs[4][0x47] == (uint8_t)level && regs[4][0x48] == (uint8_t)(level >> 8);
}

// The held level in the other coding, under the set-up's own control word: the offset itself, ten bits two's
// complement (the same voltage as the hold's 512 + offset, by the inferred coding)
static bool ref_signed(int8_t off)
{
	uint16_t level = (uint16_t)((uint16_t)off & 0x3FFu);
	return regs[4][0x25] == 0x0E && regs[4][0x47] == (uint8_t)level && regs[4][0x48] == (uint8_t)(level >> 8);
}

// The first page 4 write of reg at or after the write numbered from, or -1
static int wl_next(uint32_t from, uint8_t reg)
{
	for (uint32_t i = from; i < n_wl; i++) {
		if (wl[i].reg == reg) {
			return (int)i;
		}
	}
	return -1;
}

// From the write numbered from: the signed level's writes, 0x04 then 0x47, 0x25 = 0x0E and 0x48 back to back
static bool signed_order(uint32_t from)
{
	int i04 = wl_next(from, 0x04), i47 = wl_next(from, 0x47), i25 = wl_next(from, 0x25), i48 = wl_next(from, 0x48);
	return i04 >= 0 && i47 == i04 + 1 && i25 == i47 + 1 && i48 == i25 + 1 && wl[i25].val == 0x0E;
}

// The number of writes of val to reg from the write numbered from
static int wl_count(uint32_t from, uint8_t reg, uint8_t val)
{
	int n = 0;
	for (uint32_t i = from; i < n_wl; i++) {
		n += wl[i].reg == reg && wl[i].val == val;
	}
	return n;
}

// --- the tests ---

// What the driver's own set-ups leave in the two registers the host rewrites
static uint8_t plain_25, plain_e0_dmr, plain_e0_fm_tx;

static void test_plain(void)
{
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_plain, HZ_A));
	CHECK(h.up && !h.dmr && !h.fm_tx && h.errors == 0 && h.tuned_hz == HZ_A);
	CHECK(regs[4][0xA1] == 0x00);			// tones off
	CHECK(regs[4][0x37] == 0x9D);			// -3 dB
	CHECK(regs[4][0x36] == 0x00);			// no feed-through: the FM audio is the board's
	CHECK(regs[4][0x10] == 0x6E && h.r10 == 0x6E);	// PCM on the line out in FM
	CHECK((regs[4][0x01] & 0x40u) == 0);		// LO below the signal: straight
	CHECK(ref_is(-5));
	plain_25 = regs[4][0x25];
	CHECK(plain_25 == 0x0E && h.r25 == plain_25);
	uint32_t w25 = n_w[0x25];

	// receive entries, and a hold nobody asked for
	uint32_t x = n_xfer;
	hrc6000_if_rx_hold(&h);
	CHECK(n_xfer == x);
	hrc6000_if_retune(&h, HZ_B);
	CHECK(ref_is(9) && h.tuned_hz == HZ_B);
	CHECK(n_w[0x25] == w25 && n_restart == 0);

#if CONFIG_DMR
	hrc6000_if_mode(&h, true, HZ_B);
	CHECK(h.dmr && n_enter == 1);
	plain_e0_dmr = regs[4][0xE0];
	CHECK(plain_e0_dmr == 0xC9 && at_enter[0xE0] == 0xC9);	// the driver's choice stands
	CHECK(n_r[0xE0] == 0);
	CHECK(regs[4][0x45] == pair_byte(HZ_B, true, false) && regs[4][0x46] == pair_byte(HZ_B, true, true));
	CHECK(!hrc6000_if_fm_tx(&h, HZ_B, false, 7));	// not from DMR
	hrc6000_if_mode(&h, false, HZ_B);
	CHECK(!h.dmr && n_leave == 1);
	CHECK(regs[4][0x10] == 0x6E && regs[4][0x36] == 0x00);
#else
	plain_e0_dmr = 0xC9;
#endif
	CHECK(n_w[0x25] == w25 && regs[4][0x25] == plain_25);

	// FM transmit and back
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, true, 9));
	CHECK(h.fm_tx);
	plain_e0_fm_tx = regs[4][0xE0];
	CHECK(plain_e0_fm_tx == 0xC9 && n_r[0xE0] == 0);
	CHECK(regs[4][0x25] == plain_25);
	hrc6000_if_fm_tx_end(&h);
	CHECK(!h.fm_tx && regs[4][0x10] == 0x6E && regs[4][0x36] == 0x00 && regs[4][0x60] == 0x00);
	CHECK(regs[4][0x25] == plain_25);	// the transmit set-up's, never the hold
	CHECK(h.errors == 0);
}

static void test_init_hold(void)
{
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	CHECK(h.up && h.errors == 0 && ctx_seen > 0);
	CHECK(regs[4][0xA1] == 0x00);
	CHECK(regs[4][0x37] == 0xC2);			// +2 dB
	CHECK(regs[4][0x36] == 0x02);			// the chip's FM audio on its line out
	CHECK(regs[4][0x10] == 0x80 && h.r10 == 0x80);	// FM: hrc6000_fm_rx_pcm not applied
	CHECK(regs[4][0xE0] == 0x89);			// FM receive: the driver's codec set-up, untouched
	CHECK(regs[4][0x01] == 0xB0);			// straight
	CHECK(ref_held(-5));
	CHECK(held(regs[4][0x25]) && h.r25 == regs[4][0x25]);
	// the hold is the driver's value with bit 1 clear, whatever the driver writes: 0x0C from its 0x0E
	CHECK(regs[4][0x25] == (uint8_t)(plain_25 & ~0x02u) && regs[4][0x25] == 0x0Cu);
	// the level is there before the hold opens the DAC
	CHECK(seq_w[0x47] < seq_w[0x25] && seq_w[0x48] < seq_w[0x25] && seq_w[0x04] < seq_w[0x25]);

	// the other side of the LO at init
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_HIGH_SIDE));
	CHECK(regs[4][0x01] == 0xF0 && ref_held(0));
	fake_reset();
	side_flip = true;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_HIGH_SIDE));
	CHECK(regs[4][0x01] == 0xB0);
}

static void test_fm(void)
{
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));

	// a retune in FM: the offset, the hold again, nothing of DMR
	regs[4][0x25] = 0x0E;	// as a set-up elsewhere would leave it
	uint32_t w45 = n_w[0x45];
	uint32_t w47 = n_w[0x47], w48 = n_w[0x48];
	hrc6000_if_retune(&h, HZ_B);
	CHECK(ref_held(9) && h.tuned_hz == HZ_B && held(regs[4][0x25]));
	// written once each, straight to the level: never the offset alone on the way (the DAC near 0 V)
	CHECK(n_w[0x47] == w47 + 1 && n_w[0x48] == w48 + 1);
	CHECK(n_w[0x45] == w45 && n_restart == 0);
	CHECK(regs[4][0x01] == 0xB0);

	// across the LO side, by frequency and by the bench switch on one frequency
	hrc6000_if_retune(&h, HZ_HIGH_SIDE);
	CHECK(regs[4][0x01] == 0xF0);
	uint32_t w01 = n_w[0x01];
	hrc6000_if_retune(&h, HZ_HIGH_SIDE + 12500u);
	CHECK(n_w[0x01] == w01);	// same side: not rewritten
	side_flip = true;
	hrc6000_if_retune(&h, HZ_HIGH_SIDE + 12500u);
	CHECK(regs[4][0x01] == 0xB0);
	side_flip = false;
	hrc6000_if_retune(&h, HZ_B);
	CHECK(regs[4][0x01] == 0xB0 && n_w[0x01] == w01 + 1);

	// mode: already there
	uint32_t x = n_xfer;
	hrc6000_if_mode(&h, false, HZ_B);
	CHECK(n_xfer == x && n_leave == 0);

	// FM transmit without a pair: nothing written
	have_fm_pair = false;
	CHECK(!hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	CHECK(n_xfer == x && !h.fm_tx);
	CHECK(regs[4][0x45] != 0xEE && regs[4][0x46] != 0xEE);

	// with one
	have_fm_pair = true;
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 11));
	CHECK(h.fm_tx && !h.dmr);
	CHECK(regs[4][0x10] == 0x80 && regs[4][0x60] == 0x80);
	CHECK(regs[4][0x45] == pair_byte(HZ_B, false, false) && regs[4][0x46] == pair_byte(HZ_B, false, true));
	CHECK(regs[4][0xE4] == 0x20 + 11);
	CHECK(regs[4][0x35] == hrc6000_fm_deviation(true, false).voice);
	CHECK(regs[4][0x47] == 9 && regs[4][0x04] == 9 && regs[4][0x48] == 0x00);	// the transmit form
	CHECK(regs[4][0xE0] == (uint8_t)((plain_e0_fm_tx & ~0x40u) | 0x02u));
	CHECK(regs[4][0xE0] == 0x8B);
	CHECK(regs[4][0x36] == 0x00);			// the feed-through is for FM receive
	CHECK(regs[4][0x25] == plain_25 && h.r25 == plain_25);	// the transmit set-up's own
	CHECK(regs[4][0xA1] == 0x00);

	// while it lasts: no hold, and a retune is only noted
	x = n_xfer;
	hrc6000_if_rx_hold(&h);
	hrc6000_if_retune(&h, HZ_A);
	CHECK(n_xfer == x && h.tuned_hz == HZ_A);
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, true, 11));	// keyed again, the other bandwidth
	CHECK(regs[4][0x35] == hrc6000_fm_deviation(true, true).voice);

	// the end: FM receive on the frequency noted
	regs[4][0xA1] = 0x10;	// a tone the board ran
	hrc6000_if_fm_tx_end(&h);
	CHECK(!h.fm_tx);
	CHECK(regs[4][0x10] == 0x80 && h.r10 == 0x80 && regs[4][0x60] == 0x00 && regs[4][0x36] == 0x02);
	CHECK(regs[4][0xE0] == 0x89);
	CHECK(regs[4][0xA1] == 0x00);
	CHECK(ref_held(-5));
	CHECK(held(regs[4][0x25]) && h.r25 == regs[4][0x25]);
	CHECK(seq_w[0x47] < seq_w[0x25]);	// the receive level, then the hold
	x = n_xfer;
	hrc6000_if_fm_tx_end(&h);	// not transmitting
	CHECK(n_xfer == x);

	// the hold alone
	regs[4][0x25] = 0x0E;
	hrc6000_if_rx_hold(&h);
	CHECK(held(regs[4][0x25]) && n_xfer == x + 1);

	// a retune across the LO side during a transmission is noted too: the end brings the chip to that side
	CHECK(hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	w01 = n_w[0x01];
	hrc6000_if_retune(&h, HZ_HIGH_SIDE);
	CHECK(n_w[0x01] == w01 && regs[4][0x01] == 0xB0);
	hrc6000_if_fm_tx_end(&h);
	CHECK(regs[4][0x01] == 0xF0 && ref_held(0) && h.tuned_hz == HZ_HIGH_SIDE);
	CHECK(h.errors == 0);
}

#if CONFIG_DMR
static void test_dmr(void)
{
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	CHECK(n_enter == 0 && regs[4][0xA1] == 0x00);

	regs[4][0x25] = 0x0E;	// as a set-up elsewhere would leave it
	hrc6000_if_mode(&h, true, HZ_HIGH_SIDE);
	CHECK(h.dmr && n_enter == 1 && n_restart == 0 && h.tuned_hz == HZ_HIGH_SIDE);
	// the chip as the engine finds it
	CHECK(at_enter[0xA1] == 0x00);
	CHECK(at_enter[0x10] == 0x6E && h.r10 == 0x6E);
	CHECK(at_enter[0x01] == 0xF0);		// the mode change crossed the LO side
	CHECK(at_enter[0x45] == pair_byte(HZ_HIGH_SIDE, true, false) && at_enter[0x46] == pair_byte(HZ_HIGH_SIDE, true, true));
	CHECK(at_enter[0xE0] == (uint8_t)((plain_e0_dmr & ~0x40u) | 0x02u));
	CHECK(at_enter[0xE0] == 0x8B);
	CHECK(at_enter[0xE4] == 0x20 + 7);
	CHECK(at_enter[0x36] == 0x00);
	CHECK(held(at_enter[0x25]) && h.r25 == at_enter[0x25]);	// the hold is back before the engine runs
	CHECK(at_enter[0x47] == 0x00 && at_enter[0x48] == 0x02);	// the held level: mid-scale, never 0
	CHECK(seq_w[0x47] < seq_w[0x25]);

	// already there
	uint32_t x = n_xfer;
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(n_xfer == x && n_enter == 1);

	// the same channel: neither the pair nor a restart
	uint32_t w45 = n_w[0x45], w46 = n_w[0x46];
	hrc6000_if_retune(&h, HZ_HIGH_SIDE);
	CHECK(n_w[0x45] == w45 && n_w[0x46] == w46 && n_restart == 0);
	CHECK(held(regs[4][0x25]));

	// a new one: its pair, one restart, the I/Q order of its side
	hrc6000_if_retune(&h, HZ_B);
	CHECK(n_w[0x45] == w45 + 1 && n_w[0x46] == w46 + 1 && n_restart == 1);
	CHECK(regs[4][0x45] == pair_byte(HZ_B, true, false) && regs[4][0x46] == pair_byte(HZ_B, true, true));
	CHECK(regs[4][0x01] == 0xB0 && ref_held(9) && h.tuned_hz == HZ_B);
	hrc6000_if_retune(&h, HZ_B);
	CHECK(n_w[0x45] == w45 + 1 && n_restart == 1);

	// the bench switch on the same channel: the order, no restart
	side_flip = true;
	hrc6000_if_retune(&h, HZ_B);
	CHECK(regs[4][0x01] == 0xF0 && n_restart == 1);
	side_flip = false;

	// a channel without a pair gets the safe one, not what the board's function left behind
	have_dmr_pair = false;
	hrc6000_if_retune(&h, HZ_A);
	CHECK(regs[4][0x45] == SAFE_GAIN && regs[4][0x46] == SAFE_GAIN && n_restart == 2);
	CHECK(regs[4][0x01] == 0xB0);

	// after a burst sequence
	regs[4][0x25] = 0x0E;
	hrc6000_if_rx_hold(&h);
	CHECK(held(regs[4][0x25]));

	// FM transmit is not for DMR
	x = n_xfer;
	CHECK(!hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	CHECK(n_xfer == x && !h.fm_tx);

	// back to FM receive, on the other side of the LO
	regs[4][0x25] = 0x0E;
	hrc6000_if_mode(&h, false, HZ_HIGH_SIDE);
	CHECK(!h.dmr && n_leave == 1 && n_enter == 1);
	CHECK(at_leave[0x10] == 0x6E && at_leave[0x60] == 0x00);	// the engine goes off while the chip is still in DMR
	CHECK(regs[4][0x10] == 0x80 && h.r10 == 0x80 && regs[4][0x36] == 0x02);
	CHECK(regs[4][0x01] == 0xF0);
	CHECK(ref_held(0) && h.tuned_hz == HZ_HIGH_SIDE);	// its frequency's offset, as after a DMR entry
	CHECK(held(regs[4][0x25]) && h.r25 == regs[4][0x25]);

	// and in again without a pair
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(n_enter == 2 && at_enter[0x45] == SAFE_GAIN && at_enter[0x46] == SAFE_GAIN);
	CHECK(at_enter[0x01] == 0xB0 && held(at_enter[0x25]) && at_enter[0xE0] == 0x8B && at_enter[0x36] == 0x00);
	CHECK(at_enter[0x47] == 0xFB && at_enter[0x48] == 0x01);	// 512 - 5

	// a DMR entry from an FM transmission ends it
	hrc6000_if_mode(&h, false, HZ_A);
	CHECK(hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(h.dmr && !h.fm_tx && regs[4][0x60] == 0x00 && held(regs[4][0x25]));

	// a mode change is a receive entry on its frequency: the end of a transmission comes back to it, with no retune
	// in between
	hrc6000_if_mode(&h, false, HZ_HIGH_SIDE);
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	hrc6000_if_fm_tx_end(&h);
	CHECK(regs[4][0x01] == 0xF0 && ref_held(0) && h.tuned_hz == HZ_HIGH_SIDE);
	CHECK(h.errors == 0);
}
#else
static void test_dmr(void)
{
	// Without DMR the chip stays in FM receive whatever the mode asked for
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	uint32_t x = n_xfer;
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(!h.dmr && n_xfer == x && n_enter == 0 && n_leave == 0);
	CHECK(regs[4][0x10] == 0x80 && regs[4][0x36] == 0x02 && held(regs[4][0x25]));
	hrc6000_if_retune(&h, HZ_B);
	CHECK(n_restart == 0 && ref_held(9));
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	hrc6000_if_fm_tx_end(&h);
	CHECK(h.errors == 0);
}
#endif

static void test_bare(void)
{
	// No callbacks: the driver's I/Q order, no offset, the safe pair, no FM transmit
	hrc6000_if_t h;
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_bare, HZ_HIGH_SIDE));
	CHECK(regs[4][0x01] == 0xF0 && ref_is(0) && regs[4][0x25] == plain_25);
	hrc6000_if_retune(&h, HZ_A);
	CHECK(regs[4][0x01] == 0xF0 && ref_is(0));
	hrc6000_if_mode(&h, true, HZ_A);
#if CONFIG_DMR
	CHECK(h.dmr && regs[4][0x45] == SAFE_GAIN && regs[4][0x46] == SAFE_GAIN && regs[4][0xE0] == plain_e0_dmr);
	hrc6000_if_retune(&h, HZ_B);
	CHECK(regs[4][0x45] == SAFE_GAIN);
	hrc6000_if_mode(&h, false, HZ_B);
#endif
	CHECK(!h.dmr);
	uint32_t x = n_xfer;
	CHECK(!hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	CHECK(n_xfer == x && h.errors == 0);

	// Never brought up: nothing reaches the bus
	hrc6000_if_t z;
	memset(&z, 0, sizeof(z));
	x = n_xfer;
	hrc6000_if_mode(&z, true, HZ_A);
	hrc6000_if_retune(&z, HZ_A);
	CHECK(!hrc6000_if_fm_tx(&z, HZ_A, false, 7));
	hrc6000_if_fm_tx_end(&z);
	hrc6000_if_rx_hold(&z);
	CHECK(n_xfer == x && !z.dmr && !z.fm_tx);
}

static void walk(hrc6000_if_t *h);

// The reference DAC's coding: one per receive state, asked at its full set-up; 4:0x26 only when asked, and the
// driver's value back with the next set-up; the hold alone in each coding
static void test_coding(void)
{
	hrc6000_if_t h;

	// Init in the signed coding: the set-up's 0x0E stays, the level is the offset (-5) itself
	fake_reset();
	coding_now = HRC6000_IF_REF_SIGNED;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_opts, HZ_A));
	CHECK(h.up && h.ref_coding == HRC6000_IF_REF_SIGNED && h.adc_ctl == 0 && h.errors == 0);
	CHECK(ref_signed(-5) && regs[4][0x04] == 0xFB && h.r25 == 0x0E);
	CHECK(regs[4][0x47] == 0xFB && regs[4][0x48] == 0x03);
	CHECK(regs[4][0x26] == 0xFD && wl_count(0, 0x26, 0x7D) == 0);	// the driver's: not asked
	CHECK(wl_count(0, 0x25, 0x0C) == 0);	// never the hold's word in this state
	// a retune in the same coding: no set-up, the order of the level's writes, the offsets 9 and 0
	uint32_t w10 = n_w[0x10], mark = n_wl;
	CHECK(!hrc6000_if_retune(&h, HZ_B));
	CHECK(n_w[0x10] == w10 && signed_order(mark) && ref_signed(9) && regs[4][0x48] == 0x00);
	mark = n_wl;
	CHECK(!hrc6000_if_retune(&h, HZ_A + 25000u));
	CHECK(signed_order(mark) && ref_signed(0) && regs[4][0x47] == 0x00);
	CHECK(wl_count(0, 0x25, 0x0C) == 0);

	// 4:0x26 when asked: a retune that finds the answer changed runs the whole set-up
	adc_now = 0x7D;
	mark = n_wl;
	CHECK(hrc6000_if_retune(&h, HZ_A));
	CHECK(n_w[0x10] > w10 && h.adc_ctl == 0x7D && regs[4][0x26] == 0x7D && ref_signed(-5));
	// the FM receive rows' 0xFD first, then the board's value right after the level's last write
	int i26 = wl_next(mark, 0x26), i48 = wl_next(mark, 0x48);
	CHECK(i26 >= 0 && wl[i26].val == 0xFD && wl_count(mark, 0x26, 0x7D) == 1);
	CHECK(i48 > i26 && wl_next((uint32_t)i26 + 1u, 0x26) == i48 + 1 && wl[i48 + 1].val == 0x7D);
	mark = n_wl;
	CHECK(!hrc6000_if_retune(&h, HZ_B));	// every receive entry of that state
	CHECK(regs[4][0x26] == 0x7D && wl_count(mark, 0x26, 0x7D) == 1 && ref_signed(9));
	// and back: the driver's value, nothing written after the rows
	adc_now = 0;
	mark = n_wl;
	CHECK(hrc6000_if_retune(&h, HZ_B));
	CHECK(regs[4][0x26] == 0xFD && wl_count(mark, 0x26, 0x7D) == 0 && ref_signed(9));

	// The signed coding back to the hold: the whole set-up, the hold's word and level. The level's high bits land
	// under the signed coding's 0x0E (the rows do not write 0x25): the hold's 0x0C is the very next write
	coding_now = HRC6000_IF_REF_MID;
	w10 = n_w[0x10];
	mark = n_wl;
	CHECK(hrc6000_if_retune(&h, HZ_B));
	CHECK(n_w[0x10] > w10 && h.ref_coding == HRC6000_IF_REF_MID && ref_held(9) && regs[4][0x25] == 0x0C);
	CHECK(h.r25 == 0x0C && regs[4][0x26] == 0xFD);
	i48 = wl_next(mark, 0x48);
	CHECK(i48 >= 0 && wl_next(mark, 0x25) == i48 + 1 && wl[i48 + 1].val == 0x0C);
	CHECK(!hrc6000_if_retune(&h, HZ_B));
	// An answer the interface does not know is the hold, with the driver's 4:0x26 whatever else it gives
	coding_now = 7;
	adc_now = 0x7D;
	mark = n_wl;
	CHECK(!hrc6000_if_retune(&h, HZ_B) && ref_held(9) && h.adc_ctl == 0 && wl_count(mark, 0x26, 0x7D) == 0);
	adc_now = 0;

	// The hold alone, in each coding, one write and no question asked
	coding_now = HRC6000_IF_REF_MID;
	regs[4][0x25] = 0x0E;
	int asked = n_coding;
	uint32_t x = n_xfer;
	hrc6000_if_rx_hold(&h);
	CHECK(regs[4][0x25] == 0x0C && n_xfer == x + 1);
	coding_now = HRC6000_IF_REF_SIGNED;
	CHECK(hrc6000_if_retune(&h, HZ_B) && ref_signed(9));
	asked = n_coding;
	regs[4][0x25] = 0x0C;
	x = n_xfer;
	hrc6000_if_rx_hold(&h);
	CHECK(regs[4][0x25] == 0x0E && n_xfer == x + 1 && n_coding == asked);
	// the knob turned without a receive entry yet: the hold keeps the coding of the state it is in
	coding_now = HRC6000_IF_REF_MID;
	hrc6000_if_rx_hold(&h);
	CHECK(regs[4][0x25] == 0x0E && n_coding == asked);

	// A transmit set-up in between, and back: the end asks again
	coding_now = HRC6000_IF_REF_SIGNED;
	adc_now = 0x7D;
	CHECK(hrc6000_if_retune(&h, HZ_B) && regs[4][0x26] == 0x7D);
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	CHECK(regs[4][0x25] == 0x0E && regs[4][0x26] == 0xFE && regs[4][0x47] == 9 && regs[4][0x04] == 9);
	x = n_xfer;
	CHECK(!hrc6000_if_retune(&h, HZ_A) && n_xfer == x);	// only noted, whatever the coding says
	hrc6000_if_fm_tx_end(&h);
	CHECK(ref_signed(-5) && regs[4][0x26] == 0x7D && h.r25 == 0x0E);
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	coding_now = HRC6000_IF_REF_MID;	// changed during the transmission: its end takes it
	adc_now = 0;
	hrc6000_if_fm_tx_end(&h);
	CHECK(ref_held(-5) && regs[4][0x25] == 0x0C && regs[4][0x26] == 0xFD && h.ref_coding == HRC6000_IF_REF_MID);
	CHECK(!hrc6000_if_retune(&h, HZ_A));
	// The end of a transmission writes the level under the transmit set-up's 0x0E: the hold's word right after the
	// level's last write, before the I/Q order's 0x01 (the LO side changed during the transmission)
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7) && regs[4][0x25] == 0x0E);
	CHECK(!hrc6000_if_retune(&h, HZ_HIGH_SIDE));
	mark = n_wl;
	hrc6000_if_fm_tx_end(&h);
	i48 = wl_next(mark, 0x48);
	CHECK(i48 >= 0 && wl_next((uint32_t)i48 + 1u, 0x48) < 0 && wl[i48 + 1].reg == 0x25 && wl[i48 + 1].val == 0x0C);
	CHECK(wl_next((uint32_t)i48, 0x01) > i48 + 1 && regs[4][0x01] == 0xF0 && ref_held(0));
	CHECK(h.errors == 0);

#if CONFIG_DMR
	// DMR: the entry in the signed coding with 4:0x26 asked, as the engine finds it
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_opts, HZ_A));
	coding_now = HRC6000_IF_REF_SIGNED;
	adc_now = 0x7D;
	hrc6000_if_mode(&h, true, HZ_B);
	CHECK(h.dmr && n_enter == 1 && at_enter[0x25] == 0x0E && at_enter[0x26] == 0x7D);
	CHECK(at_enter[0x47] == 9 && at_enter[0x48] == 0x00 && h.r25 == 0x0E);
	// a coding change at a retune: the engine off and on around the whole DMR entry, the driver's 0x26 back
	adc_now = 0;
	coding_now = HRC6000_IF_REF_MID;
	int restarts = n_restart;
	CHECK(hrc6000_if_retune(&h, HZ_A));
	CHECK(n_leave == 1 && n_enter == 2 && n_restart == restarts && h.dmr);
	CHECK(at_enter[0x25] == 0x0C && at_enter[0x26] == 0xFD && at_enter[0x47] == 0xFB && at_enter[0x48] == 0x01);
	CHECK(at_enter[0x45] == pair_byte(HZ_A, true, false) && at_leave[0x10] == 0x6E);	// the new channel's pair
	CHECK(regs[4][0x36] == 0x00 && h.tuned_hz == HZ_A);
	// the hold after a burst sequence, in DMR, in each coding
	regs[4][0x25] = 0x0E;
	hrc6000_if_rx_hold(&h);
	CHECK(regs[4][0x25] == 0x0C);
	coding_now = HRC6000_IF_REF_SIGNED;
	CHECK(hrc6000_if_retune(&h, HZ_A) && n_enter == 3 && ref_signed(-5));
	regs[4][0x25] = 0x0C;
	hrc6000_if_rx_hold(&h);
	CHECK(regs[4][0x25] == 0x0E);
	// back to FM in the signed coding: the level and its word, nothing of the hold's
	mark = n_wl;
	hrc6000_if_mode(&h, false, HZ_B);
	CHECK(!h.dmr && ref_signed(9) && signed_order(mark) && wl_count(mark, 0x25, 0x0C) == 0);
	CHECK(h.errors == 0);
#endif

	// Any one transaction failing in the signed coding with 4:0x26 asked: counted
	fake_reset();
	coding_now = HRC6000_IF_REF_SIGNED;
	adc_now = 0x7D;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_opts, HZ_A));
	uint32_t n_init = n_xfer;
	walk(&h);
	uint32_t n_all = n_xfer;
	for (uint32_t k = 0; k < n_all; k++) {
		fake_reset();
		coding_now = HRC6000_IF_REF_SIGNED;
		adc_now = 0x7D;
		fail_at = (int)k;
		bool up = hrc6000_if_init(&h, &dev, &cfg_opts, HZ_A);
		walk(&h);
		if ((k < n_init && up) || h.errors == 0) {
			printf("FAIL signed transaction %u: up %d, errors %u\n", (unsigned)k, (int)up, (unsigned)h.errors);
			failures++;
		}
		checks++;
	}
}

// The 4:0x04 value of a receive entry from its own callback: at every receive entry and nowhere else, the level
// still from the reference offset
static void test_rx_tx_offset(void)
{
	hrc6000_if_t h;
	fake_reset();
	r04_own = true;
	r04_val = 0x33;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_opts, HZ_A));
	CHECK(regs[4][0x04] == 0x33 && regs[4][0x47] == 0xFB && regs[4][0x48] == 0x01 && held(regs[4][0x25]));
	CHECK(!r04_dmr);
	r04_val = -2;
	CHECK(!hrc6000_if_retune(&h, HZ_B));
	CHECK(regs[4][0x04] == 0xFE && regs[4][0x47] == 9 && regs[4][0x48] == 0x02);
	// FM transmit takes ref_offset's value, and asks nothing
	int asked = n_r04;
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	CHECK(regs[4][0x04] == 9 && n_r04 == asked);
	hrc6000_if_fm_tx_end(&h);
	CHECK(regs[4][0x04] == 0xFE && n_r04 == asked + 1);
	// the hold alone writes no 4:0x04
	uint32_t w04 = n_w[0x04];
	hrc6000_if_rx_hold(&h);
	CHECK(n_w[0x04] == w04 && n_r04 == asked + 1);
	// the signed coding takes it as well
	coding_now = HRC6000_IF_REF_SIGNED;
	CHECK(hrc6000_if_retune(&h, HZ_B) && regs[4][0x04] == 0xFE && ref_signed(9));
	coding_now = HRC6000_IF_REF_MID;
	CHECK(hrc6000_if_retune(&h, HZ_B));
#if CONFIG_DMR
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(r04_dmr && regs[4][0x04] == 0xFE && at_enter[0x04] == 0xFE && at_enter[0x47] == 0xFB);
	CHECK(!hrc6000_if_retune(&h, HZ_B) && r04_dmr && regs[4][0x04] == 0xFE);
	hrc6000_if_mode(&h, false, HZ_B);
	CHECK(!r04_dmr);
#endif
	CHECK(h.errors == 0);
}

// Page 4 as a run leaves it, for runs with and without the rows to compare
static uint8_t page_tx[256], page_end[256];

// The steps of a transmission the rows must not outlive: an FM key-up (wide or narrow), keyed again on the other
// width when twice, then its end or (dmr_end) a DMR entry. Page 4 after the set-up and after the end
static void rows_run(const hrc6000_if_cfg_t *cfg, bool on, bool wide, bool twice, bool dmr_end, hrc6000_if_t *h)
{
	fake_reset();
	rows_on = on;
	rows_too_many = false;
	CHECK(hrc6000_if_init(h, &dev, cfg, HZ_A));
	CHECK(hrc6000_if_fm_tx(h, HZ_B, wide, 7));
	if (twice) {
		CHECK(hrc6000_if_fm_tx(h, HZ_B, !wide, 7));
	}
	memcpy(page_tx, regs[4], sizeof(page_tx));
	if (dmr_end) {
		hrc6000_if_mode(h, true, HZ_B);
	} else {
		hrc6000_if_fm_tx_end(h);
	}
	memcpy(page_end, regs[4], sizeof(page_end));
}

// Page 4 after the set-up with the rows differs from without them in the rows' registers alone, which hold the rows'
static bool rows_over(const uint8_t *with, const uint8_t *without, const hrc6000_if_row_t *rows)
{
	for (unsigned r = 0; r < 256u; r++) {
		bool row = false;
		for (unsigned i = 0; i < ROWS_N; i++) {
			if (rows[i].reg == r) {
				row = true;
				if (with[r] != rows[i].val) {
					return false;
				}
			}
		}
		if (!row && with[r] != without[r]) {
			return false;
		}
	}
	return true;
}

// The board's FM transmit rows: written after the whole set-up (the codec inputs, analog transmit on), each register's
// set-up value read before the first row is written; the end and a DMR entry write those values back before the
// receive rows, so page 4 after the transmission is what it is without the rows, register for register. Keyed again
// without an end, the set-up's values come back, not the rows'. A failed read writes no row; too many rows, none;
// both counted. None given: nothing read or written for them
static void test_fm_tx_rows(void)
{
	hrc6000_if_t h;
	static uint8_t base_tx[256], base_end[256];
	for (int wide = 0; wide < 2; wide++) {
		const hrc6000_if_row_t *rows = wide ? rows_wide : rows_narrow;
		rows_run(&cfg_rows, false, wide != 0, false, false, &h);
		memcpy(base_tx, page_tx, sizeof(base_tx));
		memcpy(base_end, page_end, sizeof(base_end));
		CHECK(h.n_back == 0u && h.errors == 0 && n_r[0x3F] == 0 && n_r[0x35] == 0);
		n_rows_asked = 0;
		rows_run(&cfg_rows, true, wide != 0, false, false, &h);
		CHECK(n_rows_asked == 1 && rows_hz == HZ_B && rows_wide_asked == (wide != 0) && h.errors == 0);
		CHECK(rows_over(page_tx, base_tx, rows));
		CHECK(memcmp(page_end, base_end, sizeof(base_end)) == 0 && h.n_back == 0u);
		// Read once each, after the set-up; written after the codec inputs and analog transmit
		for (unsigned i = 0; i < ROWS_N; i++) {
			CHECK(n_r[rows[i].reg] >= 1u);
		}
		// The order of the key-up and of its end, on the write log of one more run
		fake_reset();
		rows_on = true;
		CHECK(hrc6000_if_init(&h, &dev, &cfg_rows, HZ_A));
		uint32_t from = n_wl;
		CHECK(hrc6000_if_fm_tx(&h, HZ_B, wide != 0, 7));
		int i60 = wl_next(from, 0x60), ie0 = wl_next(from, 0xE0), i3f = -1;
		for (uint32_t k = from; k < n_wl; k++) {
			if (wl[k].reg == 0x3F && wl[k].val == rows[0].val && k > (uint32_t)i60) {
				i3f = (int)k;
			}
		}
		CHECK(i60 >= 0 && ie0 >= 0 && i3f > i60 && i3f > wl_next((uint32_t)ie0 + 1u, 0xE0));
		CHECK(h.n_back == ROWS_N && h.tx_back[0].reg == 0x3F && h.tx_back[0].val == base_tx[0x3F]);
		from = n_wl;
		hrc6000_if_fm_tx_end(&h);
		int b3f = wl_next(from, 0x3F), b34 = wl_next(from, 0x34), r60 = wl_next(from, 0x60);
		CHECK(b3f >= 0 && wl[b3f].val == base_tx[0x3F] && b34 >= 0 && wl[b34].val == base_tx[0x34]);
		CHECK(r60 > b3f && r60 > b34 && wl[r60].val == 0x00);	// before the receive rows (their first: 4:0x60)
	}
	// Keyed again without an end: the set-up's values back, the second width's rows on
	rows_run(&cfg_rows, false, false, true, false, &h);
	memcpy(base_tx, page_tx, sizeof(base_tx));
	memcpy(base_end, page_end, sizeof(base_end));
	rows_run(&cfg_rows, true, false, true, false, &h);
	CHECK(rows_over(page_tx, base_tx, rows_wide) && memcmp(page_end, base_end, sizeof(base_end)) == 0);
	CHECK(h.errors == 0);
	// The same with a row on a register the set-up never writes: the second set-up reads the set-up's value, not the
	// first transmission's row, so the end leaves the register as it was
	rows_untouched = true;
	rows_run(&cfg_rows, true, false, true, false, &h);
	rows_untouched = false;
	CHECK(page_tx[0x3B] == 0x5A && base_end[0x3B] != 0x5A && memcmp(page_end, base_end, sizeof(base_end)) == 0);
	CHECK(h.errors == 0);
#if CONFIG_DMR
	// A DMR entry that ends the transmission writes them back too
	rows_run(&cfg_rows, false, true, false, true, &h);
	memcpy(base_end, page_end, sizeof(base_end));
	rows_run(&cfg_rows, true, true, false, true, &h);
	CHECK(memcmp(page_end, base_end, sizeof(base_end)) == 0 && h.n_back == 0u && h.errors == 0 && h.dmr);
#endif
	// A read that fails: counted, no row written, nothing to write back
	rows_run(&cfg_rows, false, false, false, false, &h);
	memcpy(base_tx, page_tx, sizeof(base_tx));
	memcpy(base_end, page_end, sizeof(base_end));
	fake_reset();
	rows_on = true;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_rows, HZ_A));
	fail_r = 0x34;
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	fail_r = -1;
	CHECK(h.errors == 1 && h.n_back == 0u && memcmp(regs[4], base_tx, sizeof(base_tx)) == 0);
	hrc6000_if_fm_tx_end(&h);
	CHECK(memcmp(regs[4], base_end, sizeof(base_end)) == 0 && h.errors == 1);
	// Too many: none read or written, counted
	fake_reset();
	rows_on = rows_too_many = true;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_rows, HZ_A));
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	CHECK(h.errors == 1 && h.n_back == 0u && memcmp(regs[4], base_tx, sizeof(base_tx)) == 0);
	rows_too_many = false;
	// A row's write that fails (the third, 4:0x34: the rows are the set-up's last writes, before its two reads back):
	// counted, and every row still written back at the end
	fake_reset();
	rows_on = true;
	CHECK(hrc6000_if_init(&h, &dev, &cfg_rows, HZ_A));
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	const uint32_t x_end = n_xfer;
	fake_reset();
	rows_on = true;
	fail_at = (int)(x_end - 2u - ROWS_N + 2u);
	CHECK(hrc6000_if_init(&h, &dev, &cfg_rows, HZ_A));
	CHECK(hrc6000_if_fm_tx(&h, HZ_B, false, 7));
	fail_at = -1;
	CHECK(h.errors == 1 && h.n_back == ROWS_N && regs[4][0x34] == base_tx[0x34] && regs[4][0x3E] == 0x06);
	hrc6000_if_fm_tx_end(&h);
	CHECK(memcmp(regs[4], base_end, sizeof(base_end)) == 0 && h.errors == 1);
	rows_on = false;
}

// Everything the host does after a good init, once
static void walk(hrc6000_if_t *h)
{
	hrc6000_if_retune(h, HZ_B);
	hrc6000_if_mode(h, true, HZ_B);
	hrc6000_if_retune(h, HZ_HIGH_SIDE);
	hrc6000_if_rx_hold(h);
	hrc6000_if_mode(h, false, HZ_A);
	hrc6000_if_retune(h, HZ_A);
	(void)hrc6000_if_fm_tx(h, HZ_A, true, 7);
	hrc6000_if_fm_tx_end(h);
	hrc6000_if_rx_hold(h);
}

// walk() and more on one configuration: the hash of the whole bus log, every transaction in order
static uint32_t script(const hrc6000_if_cfg_t *cfg)
{
	hrc6000_if_t h;
	fake_reset();
	(void)hrc6000_if_init(&h, &dev, cfg, HZ_A);
	walk(&h);
	hrc6000_if_retune(&h, HZ_HIGH_SIDE);
	(void)hrc6000_if_fm_tx(&h, HZ_B, false, 9);
	hrc6000_if_retune(&h, HZ_A);
	hrc6000_if_fm_tx_end(&h);
	hrc6000_if_mode(&h, true, HZ_A);
	hrc6000_if_rx_hold(&h);
	hrc6000_if_retune(&h, HZ_B);
	hrc6000_if_mode(&h, false, HZ_B);
	hrc6000_if_retune(&h, HZ_B);
	return log_hash;
}

// The logs of the code before the reference coding and the receive 4:0x04 callback existed, taken from it once. The
// hold's again since the end of an FM transmission writes the hold before the I/Q order (test_coding), the only
// change: the plain and bare logs, which have no hold, are the same as before it
#if CONFIG_DMR
#define LOG_HOLD	0x95f8a3cfu
#define LOG_PLAIN	0x77e3cdaau
#define LOG_BARE	0xf59b2579u
#else
#define LOG_HOLD	0xaaa0c7c3u
#define LOG_PLAIN	0x1c76dd26u
#define LOG_BARE	0x47eaacebu
#endif

// The options left NULL write exactly what the host wrote before them, transaction for transaction; and so do
// options that answer what NULL means: the measured hold, the driver's 4:0x26, ref_offset's value in 4:0x04
static void test_log(void)
{
	CHECK(script(&cfg_hold) == LOG_HOLD);
	CHECK(script(&cfg_plain) == LOG_PLAIN);
	CHECK(script(&cfg_bare) == LOG_BARE);
	n_coding = n_r04 = 0;
	CHECK(script(&cfg_opts) == LOG_HOLD);
	CHECK(n_coding > 0 && n_r04 > 0);
	// The rows' callback answering none: the same log; with rows, another
	n_rows_asked = 0;
	CHECK(script(&cfg_rows) == LOG_HOLD && n_rows_asked > 0);
	rows_on = true;
	n_rows_asked = 0;
	uint32_t with_rows = script(&cfg_rows);
	CHECK(with_rows != LOG_HOLD && n_rows_asked > 0);
	rows_on = false;
	// On a board without the receive hold the two options are never asked
	n_coding = n_r04 = 0;
	CHECK(script(&cfg_plain_opts) == LOG_PLAIN && n_coding == 0 && n_r04 == 0);
}

static void test_failures(void)
{
	hrc6000_if_t h;

	// A chip that does not answer: not up, and left alone from then on
	fake_reset();
	fail_all = true;
	CHECK(!hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	CHECK(!h.up && h.errors > 0);
	fail_all = false;
	uint32_t x = n_xfer, e = h.errors;
	walk(&h);
	CHECK(n_xfer == x && h.errors == e && n_enter == 0 && n_leave == 0 && n_restart == 0);
	CHECK(!h.dmr && !h.fm_tx);
	CHECK(!hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	// a second try brings it up, with a clean count
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	CHECK(h.up && h.errors == 0);

	// No chip, on a bus that cannot tell: every read gives the idle level of the data line. Not up, with no failure
	// counted, and left alone
	static const int levels[] = { 0xFF, 0x00 };
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		fake_reset();
		silent = levels[i];
		CHECK(!hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
		CHECK(!h.up && h.errors == 0 && h.r10 == levels[i]);
		x = n_xfer;
		walk(&h);
		CHECK(n_xfer == x && n_enter == 0 && n_leave == 0 && n_restart == 0);
		fake_reset();
		silent = levels[i];
		CHECK(!hrc6000_if_init(&h, &dev, &cfg_plain, HZ_A));
		CHECK(!h.up && h.errors == 0);
	}

	// Any one transaction of the init failing: counted, and the chip is not up
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	uint32_t n_init = n_xfer;
	CHECK(n_init > 100);
	for (uint32_t k = 0; k < n_init; k++) {
		fake_reset();
		fail_at = (int)k;
		bool up = hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A);
		if (up || h.up || h.errors == 0) {
			printf("FAIL init transaction %u: up %d, errors %u\n", (unsigned)k, (int)up, (unsigned)h.errors);
			failures++;
		}
		checks++;
	}

	// Any one transaction after it failing: counted
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	walk(&h);
	uint32_t n_all = n_xfer;
	CHECK(n_all > n_init + 20 && h.errors == 0);
	for (uint32_t k = n_init; k < n_all; k++) {
		fake_reset();
		fail_at = (int)k;
		(void)hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A);
		walk(&h);
		if (!h.up || h.errors == 0) {
			printf("FAIL transaction %u: up %d, errors %u\n", (unsigned)k, (int)h.up, (unsigned)h.errors);
			failures++;
		}
		checks++;
	}

	// By register: the hold's write, the codec input read (no write follows a failed read), the offset
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_A));
	fail_w = 0x25;
	hrc6000_if_rx_hold(&h);
	CHECK(h.errors == 1);
	fail_w = -1;
	fail_r = 0xE0;
	uint32_t we0 = n_w[0xE0];
	CHECK(hrc6000_if_fm_tx(&h, HZ_A, false, 7));
	CHECK(h.errors == 2 && n_w[0xE0] == we0 + 1 && regs[4][0xE0] == plain_e0_fm_tx);	// the driver's one write
	fail_r = -1;
	hrc6000_if_fm_tx_end(&h);
	fail_w = 0x47;
	hrc6000_if_retune(&h, HZ_B);
	CHECK(h.errors == 3);
	fail_w = -1;
#if CONFIG_DMR
	hrc6000_if_mode(&h, true, HZ_B);
	fail_w = 0x46;
	hrc6000_if_retune(&h, HZ_A);
	CHECK(h.errors == 4 && n_restart == 1);	// the receiver still searches again on the new channel
	fail_w = -1;
#endif
}

int main(void)
{
	test_plain();
	test_init_hold();
	test_fm();
	test_dmr();
	test_bare();
	test_failures();
	test_log();
	test_coding();
	test_rx_tx_offset();
	test_fm_tx_rows();
	CHECK(ctx_seen > 0 && ctx_bad == 0);	// every callback got the board's context
	printf("hrc6000IfHostTest%s: %d checks, %d failures\n", CONFIG_DMR ? "" : " (no DMR)", checks, failures);
	return failures != 0;
}
