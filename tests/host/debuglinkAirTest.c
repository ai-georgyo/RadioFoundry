// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_AIR's OPEN (core/debuglink/debuglink_air.c, docs/spec/debuglink.md 3.28): the raw bench's profile, a 15-byte
// header as before (verify AIR_VERIFY_ALL, no flags, sync_any 0) or a 19-byte one with sync_any_syms, flags and
// verify, told apart by length; every other length refused; the soft modem's gate words prepared on every sync. The
// board's provider, the radio state, the heap and the log are faked
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// board's hooks change per test: defined writable, the code under test sees it const
#define board board_as_declared
#include "board.h"
#undef board
#include "air/air_softmodem.h"
#include "debuglink/debuglink_air.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fakes ---

static unsigned logs, mallocs, frees, fits, opens, closes;
static air_profile_t got;		// what the provider was asked to fit
static const air_profile_t *opened;
static uint32_t fit_answer;

void dlog(const char *fmt, ...)
{
	(void)fmt;
	logs++;
}

void *pvPortMalloc(size_t n)
{
	mallocs++;
	return malloc(n);
}

void vPortFree(void *p)
{
	frees += p != NULL;
	free(p);
}

void vTaskDelay(uint32_t ticks)
{
	(void)ticks;
}

radio_state_t radio_get_state(void)
{
	return RS_IDLE;
}

static uint32_t fake_fit(const air_profile_t *p)
{
	fits++;
	got = *p;
	return fit_answer;
}

static bool fake_open(const air_profile_t *p, air_event_cb_t cb, void *ctx)
{
	(void)cb;
	(void)ctx;
	opens++;
	opened = p;
	return true;
}

static void fake_close(void)
{
	closes++;
}

static uint8_t fake_space(void)
{
	return 3;
}

static uint32_t fake_now(void)
{
	return 0;
}

static const hal_air_t air = { .fit = fake_fit, .open = fake_open, .close = fake_close, .tx_space = fake_space,
			       .now_us = fake_now, .tx_chunk_syms = 144, .tx_queue_depth = 4 };
board_t board = { .air = &air };

// --- the request ---

static uint8_t req[DL_REQ_MAX + 32], out[256];

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

// OPEN with a profile of nsyncs patterns (P25's numbers, M17's sync words), the long header when long_hdr; extra bytes
// on the end (or fewer when negative); returns the request's length
static uint16_t open_req(uint8_t nsyncs, bool long_hdr, int extra)
{
	static const uint8_t syms[4][6] = {
		{ 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF }, { 0xFF, 0x5D }, { 0x55, 0xF7 }, { 0xDF, 0x55 },
	};
	uint8_t *b = req + 2;
	uint16_t n;

	memset(req, 0, sizeof(req));
	req[0] = DL_AIR_OP_OPEN;
	req[1] = 0x01;
	put16(b, 4800);
	b[2] = 4;
	b[3] = AIR_SHAPE_RRC;
	b[4] = 50;
	put16(b + 5, 2400);
	b[7] = AIR_TIMING_CONTINUOUS;
	b[8] = 0;
	put16(b + 9, 0);
	put16(b + 11, 864);
	b[13] = 0x77;
	b[14] = nsyncs;
	n = 15;
	if (long_hdr) {
		put16(b + 15, 192);
		b[17] = AIR_PROFILE_SYNC_GRID;
		b[18] = AIR_VERIFY_M17;
		n = 19;
	}
	for (uint8_t i = 0; i < nsyncs; i++, n += 11) {
		b[n] = (uint8_t)(0x10 + i);
		b[n + 1] = i ? 8 : 24;
		b[n + 2] = 1;
		b[n + 3] = i == 0 ? AIR_SYNC_START_ONLY : 0;
		b[n + 4] = (uint8_t)(int8_t)(i ? 0 : -3);
		memcpy(b + n + 5, syms[i & 3], 6);
	}
	return (uint16_t)(2 + (int)n + extra);
}

// The provider asked to fit (refusing: fit 0) means the profile parsed; bad argument means it didn't
static uint8_t try_open(uint16_t len)
{
	unsigned f = fits;
	size_t r;

	memset(&got, 0xA5, sizeof(got));
	fit_answer = 0;
	r = debuglink_cmd_air(req, len, out, sizeof(out));
	CHECK(r >= 1);
	CHECK(out[0] != DL_ST_REFUSED || (r == 5 && fits == f + 1));
	CHECK(out[0] != DL_ST_BAD_ARG || fits == f);
	CHECK(mallocs == frees);	// a profile refused either way gives its memory back
	return out[0];
}

static void check_prepped(const air_profile_t *p)
{
	for (uint8_t i = 0; i < p->nsyncs; i++) {
		air_sync_t s = p->sync[i];

		air_softmodem_sync_prep(&s);
		CHECK(p->sync[i].gate_up == s.gate_up && p->sync[i].gate_mask == s.gate_mask);
		CHECK(p->sync[i].sign_val == s.sign_val && p->sync[i].sign_mask == s.sign_mask);
		CHECK(p->sync[i].sign_mask != 0 && p->sync[i].gate_mask != 0);
	}
}

static void testShort(void)
{
	// 15 + 11 n: today's meaning
	for (uint8_t n = 0; n <= 4; n++) {
		CHECK(try_open(open_req(n, false, 0)) == DL_ST_REFUSED);
		CHECK(got.nsyncs == n && got.sym_rate == 4800 && got.levels == 4 && got.shape == AIR_SHAPE_RRC);
		CHECK(got.shape_param == 50 && got.dev_outer_hz == 2400 && got.sync_every_syms == 864 && got.idle == 0x77);
		CHECK(got.verify == AIR_VERIFY_ALL && got.flags == 0 && got.sync_any_syms == 0);
		check_prepped(&got);
	}
	CHECK(got.sync[0].id == 0x10 && got.sync[0].nsyms == 24 && got.sync[0].flags == AIR_SYNC_START_ONLY);
	CHECK(got.sync[0].frame_at == -3 && got.sync[1].nsyms == 8 && got.sync[1].syms[0] == 0xFF && got.sync[1].syms[1] == 0x5D);
}

static void testLong(void)
{
	// 19 + 11 n: sync_any_syms, flags and verify too
	for (uint8_t n = 0; n <= 3; n++) {
		CHECK(try_open(open_req(n, true, 0)) == DL_ST_REFUSED);
		CHECK(got.nsyncs == n && got.sym_rate == 4800 && got.sync_every_syms == 864 && got.idle == 0x77);
		CHECK(got.sync_any_syms == 192 && got.flags == AIR_PROFILE_SYNC_GRID && got.verify == AIR_VERIFY_M17);
		check_prepped(&got);
	}
	CHECK(got.sync[2].id == 0x12 && got.sync[2].syms[0] == 0x55 && got.sync[2].syms[1] == 0xF7);
	// verify 0 travels as it is (a profile never verified)
	open_req(1, true, 0);
	req[2 + 18] = 0;
	CHECK(try_open(2 + 19 + 11) == DL_ST_REFUSED && got.verify == 0 && got.nsyncs == 1);
}

static void testLengths(void)
{
	uint16_t bad = 0;

	// Every length but 15 + 11 n and 19 + 11 n (n the nsyncs byte) is refused, whatever nsyncs says
	for (uint8_t n = 0; n <= 4; n++) {
		for (int extra = -15; extra <= 20; extra++) {
			uint16_t len = open_req(n, false, extra), plen = (uint16_t)(len - 2);
			bool ok = plen == 15 + 11 * n || plen == 19 + 11 * n;

			if (len < 2 || len > DL_REQ_MAX)
				continue;
			if (plen == 19 + 11 * n && n)
				continue;	// (a long header then, the first sync's bytes read as its four: either answer)
			if ((try_open(len) == DL_ST_BAD_ARG) == ok)
				bad++;
		}
	}
	CHECK(bad == 0);
	// More syncs than a profile holds, a sync too short or too long, a rate of 0, three levels: refused
	open_req(0, false, 0);
	req[2 + 14] = AIR_SYNCS_MAX + 1;
	CHECK(try_open(2 + 15 + 11 * (AIR_SYNCS_MAX + 1)) == DL_ST_BAD_ARG);
	open_req(2, true, 0);
	req[2 + 19 + 11 + 1] = 7;
	CHECK(try_open(2 + 19 + 22) == DL_ST_BAD_ARG);
	open_req(2, true, 0);
	req[2 + 19 + 11 + 1] = AIR_SYNC_SYMS_MAX + 1;
	CHECK(try_open(2 + 19 + 22) == DL_ST_BAD_ARG);
	open_req(1, false, 0);
	put16(req + 2, 0);
	CHECK(try_open(2 + 15 + 11) == DL_ST_BAD_ARG);
	open_req(1, true, 0);
	req[2 + 2] = 3;
	CHECK(try_open(2 + 19 + 11) == DL_ST_BAD_ARG);
	CHECK(try_open(2 + 14) == DL_ST_BAD_ARG);	// shorter than a header
	CHECK(try_open(2) == DL_ST_BAD_ARG);
	CHECK(try_open(1) == DL_ST_BAD_ARG);		// no flags byte
}

// A session opens on the parsed profile (gates prepared), and closes with its memory given back
static void testOpen(void)
{
	size_t r;

	fit_answer = AIR_FIT_RX | AIR_FIT_SOFT;
	r = debuglink_cmd_air(req, open_req(3, true, 0), out, sizeof(out));
	CHECK(r == 8 && out[0] == DL_ST_OK && opens == 1 && opened != NULL);
	if (opened != NULL) {
		CHECK(opened->nsyncs == 3 && opened->flags == AIR_PROFILE_SYNC_GRID && opened->verify == AIR_VERIFY_M17);
		check_prepped(opened);
	}
	req[0] = DL_AIR_OP_CLOSE;
	r = debuglink_cmd_air(req, 1, out, sizeof(out));
	CHECK(r == 1 && out[0] == DL_ST_OK && closes == 1 && mallocs == frees);
}

int main(void)
{
	testShort();
	testLong();
	testLengths();
	testOpen();
	printf("debuglinkAirTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
