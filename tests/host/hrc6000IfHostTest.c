// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000 on a board that feeds it a 450 kHz IF (drivers/modem/hrc6000/if_host): tones off before the first DMR
// entry; the receive I/Q order follows the first LO's side at init, on a mode change and on a retune; the receive
// hold of the DAC control word after every set-up, and no trace of it on a board that does not ask for it; the codec
// input bits after a DMR entry and an FM transmit set-up; the modulation pair and one receiver restart on a new DMR
// channel; FM transmit refused without a pair; the FM receive audio path either way; every bus failure counted, and
// a chip that is not there found by its mode register on a bus that reports nothing.
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

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	uint8_t page = hdr[0] & 7u;
	bool read = (hdr[0] & 0x80u) != 0;
	uint32_t k = n_xfer++;
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
	CHECK(ref_is(-5));
	CHECK(held(regs[4][0x25]) && h.r25 == regs[4][0x25]);
	// the hold is the driver's value with bit 1 clear, whatever the driver writes: 0x0C from its 0x0E
	CHECK(regs[4][0x25] == (uint8_t)(plain_25 & ~0x02u) && regs[4][0x25] == 0x0Cu);
	// the level is there before the hold opens the DAC
	CHECK(seq_w[0x47] < seq_w[0x25] && seq_w[0x48] < seq_w[0x25] && seq_w[0x04] < seq_w[0x25]);

	// the other side of the LO at init
	fake_reset();
	CHECK(hrc6000_if_init(&h, &dev, &cfg_hold, HZ_HIGH_SIDE));
	CHECK(regs[4][0x01] == 0xF0 && ref_is(0));
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
	hrc6000_if_retune(&h, HZ_B);
	CHECK(ref_is(9) && h.tuned_hz == HZ_B && held(regs[4][0x25]));
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
	CHECK(ref_is(-5));
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
	CHECK(regs[4][0x01] == 0xF0 && ref_is(0) && h.tuned_hz == HZ_HIGH_SIDE);
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
	CHECK(at_enter[0x47] == 0 && at_enter[0x48] == 0);
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
	CHECK(regs[4][0x01] == 0xB0 && ref_is(9) && h.tuned_hz == HZ_B);
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
	CHECK(ref_is(0) && h.tuned_hz == HZ_HIGH_SIDE);	// its frequency's offset, as after a DMR entry
	CHECK(held(regs[4][0x25]) && h.r25 == regs[4][0x25]);

	// and in again without a pair
	hrc6000_if_mode(&h, true, HZ_A);
	CHECK(n_enter == 2 && at_enter[0x45] == SAFE_GAIN && at_enter[0x46] == SAFE_GAIN);
	CHECK(at_enter[0x01] == 0xB0 && held(at_enter[0x25]) && at_enter[0xE0] == 0x8B && at_enter[0x36] == 0x00);
	CHECK(at_enter[0x47] == (uint8_t)-5 && at_enter[0x48] == 0x03);

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
	CHECK(regs[4][0x01] == 0xF0 && ref_is(0) && h.tuned_hz == HZ_HIGH_SIDE);
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
	CHECK(n_restart == 0 && ref_is(9));
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
	CHECK(ctx_seen > 0 && ctx_bad == 0);	// every callback got the board's context
	printf("hrc6000IfHostTest%s: %d checks, %d failures\n", CONFIG_DMR ? "" : " (no DMR)", checks, failures);
	return failures != 0;
}
