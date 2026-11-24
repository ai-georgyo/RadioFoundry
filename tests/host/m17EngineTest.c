// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 engine (core/m17/m17_engine.c, m17_rx.c, m17_tx.c) on the air interface session: two engines on made-up
// providers whose transmit queues feed each other's receivers a chunk at a time, the test playing both radio cores
// (PTT, the signature, the packet inbox). Streams with their fields, META and voice in order; late entry at every
// LICH phase; the squelch; the end of transmission; scrambled and AES streams with the right and the wrong key, joined
// late; a signed stream signed and verified with P-256, and one whose signature came late; packets of every length, the
// received-packet buffer's rules; BERT; the FN limit; a sync one symbol from another kind's inside a transmission. Then
// whole transmissions through a modeled discriminator tap and the soft modem (transmit pulses of the profile's RRC 0.5
// and a chip's RRC 0.2, a frequency offset, white noise), frame loss against the noise, and the host time per frame.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "m17/m17_engine.h"
#include "m17/m17_engine_int.h"	// the call layer's frame entry, for frames as the head hook leaves them
#include "air/air_syms.h"
#include "air/air_softmodem.h"
#include "crypto/p256.h"

// The secret work count (core/crash/crash_secret.c, not linked here): the engine's brackets move it. The cipher calls
// the engine makes go through the linker's --wrap (this test's flags in tests/host/Makefile): each is counted, and
// counted again when it ran outside secret work (crash_secret.h: a crash record then would keep key words). A linker
// without --wrap (Apple's) builds the test without M17_TEST_WRAP: the count is still checked, the calls are not
#ifndef M17_TEST_WRAP
#define M17_TEST_WRAP 0
#endif
volatile uint8_t crash_secret;
static uint32_t cipher_calls, cipher_open;
#if M17_TEST_WRAP

static void cipher_note(void)
{
	cipher_calls++;
	cipher_open += crash_secret == 0;
}

bool __real_aes_init(aes_ctx_t *ctx, const uint8_t *key, size_t len);
void __real_aes_encrypt(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);
bool __real_m17_scr_init(m17_scr_t *s, const uint8_t *seed, uint8_t len);
void __real_m17_scr_frame(m17_scr_t *s, uint8_t stream[M17_STREAM_BYTES]);
void __real_m17_scr_seek(m17_scr_t *s, const uint8_t *seed, uint8_t len, uint16_t fn, m17_scr_work_t *w);
bool __real_m17_scr_key_ok(const uint8_t *seed, uint8_t len);
bool __wrap_aes_init(aes_ctx_t *ctx, const uint8_t *key, size_t len);
void __wrap_aes_encrypt(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);
bool __wrap_m17_scr_init(m17_scr_t *s, const uint8_t *seed, uint8_t len);
void __wrap_m17_scr_frame(m17_scr_t *s, uint8_t stream[M17_STREAM_BYTES]);
void __wrap_m17_scr_seek(m17_scr_t *s, const uint8_t *seed, uint8_t len, uint16_t fn, m17_scr_work_t *w);
bool __wrap_m17_scr_key_ok(const uint8_t *seed, uint8_t len);

bool __wrap_aes_init(aes_ctx_t *ctx, const uint8_t *key, size_t len)
{
	cipher_note();
	return __real_aes_init(ctx, key, len);
}

void __wrap_aes_encrypt(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK])
{
	cipher_note();
	__real_aes_encrypt(ctx, in, out);
}

bool __wrap_m17_scr_init(m17_scr_t *s, const uint8_t *seed, uint8_t len)
{
	cipher_note();
	return __real_m17_scr_init(s, seed, len);
}

void __wrap_m17_scr_frame(m17_scr_t *s, uint8_t stream[M17_STREAM_BYTES])
{
	cipher_note();
	__real_m17_scr_frame(s, stream);
}

void __wrap_m17_scr_seek(m17_scr_t *s, const uint8_t *seed, uint8_t len, uint16_t fn, m17_scr_work_t *w)
{
	cipher_note();
	__real_m17_scr_seek(s, seed, len, fn, w);
}

// The key's check reads the seed's bytes into a register: secret work too (the radio core's prepare, the receiver's
// judge)
bool __wrap_m17_scr_key_ok(const uint8_t *seed, uint8_t len)
{
	cipher_note();
	return __real_m17_scr_key_ok(seed, len);
}
#endif

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x17E46172u;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double gauss(void)
{
	double u = (rng() % 1000000 + 1) / 1000001.0, w = (rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * w);
}

static double cpu_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

#define N0CALL		0x00004B13D106ull	// m17.md 11
#define W1AW		0x0000001680B7ull
#define ECHO		0x0000000ED87Dull
#define OTHER		0x000000001234ull

// ── Two providers, each other's far end: a chunk is 30 ms, 144 symbols ──
#define CHUNK		144
#define DEPTH		4
#define CAP_MAX		200000		// symbols of a transmission kept (one dibit a byte)
typedef struct {
	air_event_cb_t cb;
	void *ctx;
	bool open, on, started, deaf;
	air_tx_chunk_t q[DEPTH];
	uint8_t qn;
	uint32_t now_us, underruns, tx_syms;
} prov_t;
static prov_t prov[2];

// What the air does to a transmission from provider i: symbols before mute_until (from its start) are lost in noise,
// symbols given other soft values, Gaussian noise on the soft values, a run of symbols made random
typedef struct {
	uint32_t mute_until;
	uint32_t set_at[8];
	uint8_t set_to[8], nset;
	double sigma;
	uint32_t burst_from, burst_to;
	bool capture;			// keep what is sent in cap[]
	uint32_t xor_at[64];		// symbols XORed with xor_val (a coded bit turned: testLooseFirst)
	uint8_t xor_val[64], nxor;
} chan_t;
static chan_t ch[2];
static uint8_t cap[CAP_MAX];
static uint32_t cap_n;
static double rx_cpu;			// host time in the receivers' event handling
static bool timing;
static bool hard_only;			// the providers give no soft values

static void ev(prov_t *p, uint8_t kind, const air_chunk_t *c)
{
	air_event_t e = { .kind = kind, .t_us = p->now_us, .chunk = c };

	if (p->cb)
		p->cb(&e, p->ctx);
}

// Hard and soft as a provider with soft values gives them: +3 at AIR_SOFT_OUTER (or the soft value given), noise
// added, sliced at the inner/outer midpoint
static void deliver_soft(prov_t *to, const uint8_t *dibits, const int8_t *given, uint16_t n, double sigma)
{
	uint8_t hard[CHUNK / 4];
	int8_t soft[CHUNK];
	air_chunk_t c = { .t_us = to->now_us, .nsyms = n, .sync_at = -1, .hard = hard, .soft = hard_only ? NULL : soft };

	if (!to->open || to->on || to->deaf)
		return;
	for (uint16_t i = 0; i < n; i++) {
		double v = (given && given[i] ? given[i] : air_sym_level(dibits[i]) * (AIR_SOFT_OUTER / 3)) +
			   (sigma > 0 ? sigma * gauss() : 0);
		int s = (int)lround(v > 127 ? 127 : v < -127 ? -127 : v);

		soft[i] = (int8_t)s;
		air_sym_put(hard, i, (uint8_t)(s >= 64 ? 1 : s >= 0 ? 0 : s > -64 ? 2 : 3));
	}
	if (timing) {
		double t0 = cpu_s();

		ev(to, AIR_EV_RX, &c);
		rx_cpu += cpu_s() - t0;
	} else {
		ev(to, AIR_EV_RX, &c);
	}
}

static void deliver(prov_t *to, const uint8_t *dibits, uint16_t n, double sigma)
{
	deliver_soft(to, dibits, NULL, n, sigma);
}

static void noise_to(prov_t *to)
{
	uint8_t d[CHUNK];

	for (uint16_t i = 0; i < CHUNK; i++)
		d[i] = (uint8_t)(rng() & 3);
	deliver(to, d, CHUNK, 40);
}

static void send_chunk(uint8_t i, const air_tx_chunk_t *c)
{
	prov_t *p = &prov[i];
	chan_t *x = &ch[i];
	uint8_t d[CHUNK];
	int8_t sv[CHUNK];

	for (uint16_t k = 0; k < c->nsyms; k++) {
		uint32_t at = p->tx_syms + k;

		d[k] = air_sym_get(c->syms, k);
		sv[k] = 0;
		for (uint8_t s = 0; s < x->nset; s++)
			if (x->set_at[s] == at)
				d[k] = x->set_to[s];
		for (uint8_t s = 0; s < x->nxor; s++)
			if (x->xor_at[s] == at)
				d[k] ^= x->xor_val[s];
		if (x->capture && cap_n < CAP_MAX)
			cap[cap_n++] = d[k];
		if (at < x->mute_until || (at >= x->burst_from && at < x->burst_to))
			d[k] = (uint8_t)(rng() & 3);
	}
	p->tx_syms += c->nsyms;
	deliver_soft(&prov[i ^ 1], d, sv, c->nsyms, x->sigma);
}

static void step(uint8_t i)
{
	prov_t *p = &prov[i];

	p->now_us += 30000;
	if (!p->on) {
		if (!prov[i ^ 1].on)
			noise_to(p);
		return;
	}
	if (!p->started) {
		p->started = true;
		ev(p, AIR_EV_TX_STARTED, NULL);
	}
	if (!p->qn) {
		uint8_t idle[CHUNK];

		memset(idle, 1, sizeof idle);
		p->underruns++;
		deliver(&prov[i ^ 1], idle, CHUNK, 0);
		ev(p, AIR_EV_TX_UNDERRUN, NULL);
		return;
	}
	air_tx_chunk_t c = p->q[0];

	memmove(p->q, p->q + 1, (size_t)(--p->qn) * sizeof c);
	send_chunk(i, &c);
	if (c.flags & AIR_TX_LAST) {
		p->on = false;
		ev(p, AIR_EV_TX_DONE, NULL);
	} else {
		ev(p, AIR_EV_TX_SPACE, NULL);
	}
}

static uint32_t p_fit(const air_profile_t *pr)
{
	return pr->sym_rate == 4800 && pr->nsyncs == 6 ? AIR_FIT_RX | AIR_FIT_TX | (hard_only ? 0u : AIR_FIT_SOFT) : 0;
}
static bool p_open(uint8_t i, const air_profile_t *pr, air_event_cb_t cb, void *ctx)
{
	(void)pr;
	memset(&prov[i], 0, sizeof prov[i]);
	prov[i].cb = cb;
	prov[i].ctx = ctx;
	prov[i].open = true;
	return true;
}
static void p_close(uint8_t i) { prov[i].open = prov[i].on = false; prov[i].cb = NULL; }
static bool p_begin(uint8_t i)
{
	if (!prov[i].qn)
		return false;
	prov[i].on = true;
	prov[i].started = false;
	prov[i].tx_syms = 0;
	return true;
}
static bool p_queue(uint8_t i, const air_tx_chunk_t *c)
{
	if (prov[i].qn == DEPTH)
		return false;
	prov[i].q[prov[i].qn++] = *c;
	return true;
}
static void p_end(uint8_t i) { if (prov[i].qn) prov[i].q[prov[i].qn - 1].flags |= AIR_TX_LAST; }
static void p_abort(uint8_t i) { prov[i].qn = 0; prov[i].on = false; ev(&prov[i], AIR_EV_TX_DONE, NULL); }

#define PROVIDER(i) \
	static bool open##i(const air_profile_t *pr, air_event_cb_t cb, void *ctx) { return p_open(i, pr, cb, ctx); } \
	static void close##i(void) { p_close(i); } \
	static void restart##i(void) { } \
	static bool begin##i(void) { return p_begin(i); } \
	static bool queue##i(const air_tx_chunk_t *c) { return p_queue(i, c); } \
	static uint8_t space##i(void) { return (uint8_t)(DEPTH - prov[i].qn); } \
	static void end##i(void) { p_end(i); } \
	static void abort##i(void) { p_abort(i); } \
	static uint32_t now##i(void) { return prov[i].now_us; } \
	static const hal_air_t air##i = { p_fit, open##i, close##i, restart##i, begin##i, queue##i, space##i, end##i, abort##i, \
					  now##i, NULL, NULL, NULL, NULL, CHUNK, DEPTH, 60, 100 };
PROVIDER(0)
PROVIDER(1)
static const hal_air_t *const airs[2] = { &air0, &air1 };

// ── The two radios ──
static const uint8_t silence[M17_STREAM_BYTES] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
						   0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };

// Frame k's voice: what the encoder of a stream gives as its k-th frame (FN k: the FN wraps with it)
static void vf(uint32_t k, uint8_t out[M17_STREAM_BYTES])
{
	k &= M17_FN_MASK;
	for (uint8_t i = 0; i < M17_STREAM_BYTES; i++)
		out[i] = (uint8_t)(k * 31u + i * 17u + (k >> 7) * 5u + 0x3Cu);
}

typedef struct {
	m17_engine_t e;
	m17_engine_cfg_t cfg;
	uint32_t starts, updates, ends;
	m17_call_t call;
	// Voice heard: right for its FN, silence, wrong, lost (M17_ERRS_BAD); the FNs played first and last
	uint32_t vgood, vsil, vbad, vlost;
	uint16_t vfirst, vlast;
	bool vany;
	// The encoder: frames ready (tx_always: every time; make_every: one more every that many ticks)
	bool tx_always;
	uint8_t make_every;
	uint32_t tx_made, tx_taken, tx_total;	// tx_taken: of the transmission in hand (its FN 0 the encoder's frame 0)
	// The radio core's signature work: sign after sign_wait polls (-1: never; -2: give NULL)
	bool sign;
	int sign_wait;
	uint8_t want_h[32];
	uint32_t wants, gives;
	uint32_t checks;
	m17_sig_check_t chk;
	uint8_t phases;			// view.tx_phase values seen while sending (bit n: M17_TXP n)
	// The inbox: take packets as they come
	bool take;
	uint32_t taken;
	m17_packet_info_t info;
	uint8_t pkt[1024];
	uint16_t pkt_n;
} radio_t;
static radio_t r[2];

static uint8_t sig_d[32], sig_q[64];
static p256_scratch_t p256s;

static void h_start(const m17_call_t *c, void *ctx) { radio_t *x = ctx; x->starts++; x->call = *c; }
static void h_update(const m17_call_t *c, void *ctx) { radio_t *x = ctx; x->updates++; x->call = *c; }
static void h_end(void *ctx) { radio_t *x = ctx; x->ends++; }
static void h_voice(const uint8_t stream[M17_STREAM_BYTES], uint8_t errs, void *ctx)
{
	radio_t *x = ctx;
	uint16_t fn = (uint16_t)(x->e.call.fn & M17_FN_MASK);
	uint8_t want[M17_STREAM_BYTES];

	if (errs == M17_ERRS_BAD) {
		x->vlost++;
		return;
	}
	vf(fn, want);
	if (!memcmp(stream, want, sizeof want))
		x->vgood++;
	else if (!memcmp(stream, silence, sizeof silence))
		x->vsil++;
	else
		x->vbad++;
	if (!x->vany)
		x->vfirst = fn;
	x->vany = true;
	x->vlast = fn;
}
static bool h_tx_voice(uint8_t stream[M17_STREAM_BYTES], void *ctx)
{
	radio_t *x = ctx;

	if (!x->tx_always && x->tx_taken >= x->tx_made)
		return false;
	vf(x->tx_taken++, stream);
	return true;
}
static const m17_engine_hooks_t hooks = { h_start, h_update, h_voice, h_end, h_tx_voice };

static m17_engine_cfg_t cfg_default(uint8_t i)
{
	m17_engine_cfg_t c;

	memset(&c, 0, sizeof c);
	c.src = i ? W1AW : N0CALL;
	c.dst = M17_ADDR_BROADCAST;
	c.voice = M17_DATA_VOICE;
	c.sig_tail = 4;
	c.silence = silence;
	return c;
}

static void radios(const m17_engine_cfg_t *c0, const m17_engine_cfg_t *c1)
{
	for (uint8_t i = 0; i < 2; i++) {
		radio_t *x = &r[i];

		if (prov[i].open)
			m17_engine_end(&x->e);
		memset(x, 0, sizeof *x);
		x->cfg = i ? *c1 : *c0;
		x->tx_always = true;
		x->take = true;
		memset(&ch[i], 0, sizeof ch[i]);
		ch[i].burst_from = ch[i].burst_to = UINT32_MAX;
		CHECK(m17_engine_begin(&x->e, airs[i], &x->cfg, &hooks, x), "begin %u", i);
	}
}

// The radio core's poll: the signature the transmitter waits for, a signed stream's check, the inbox
static void core_poll(uint8_t i)
{
	radio_t *x = &r[i];
	uint8_t h[32], rs[64];

	if (m17_engine_sig_want(&x->e, h)) {
		if (!x->wants++)
			memcpy(x->want_h, h, sizeof h);
		if (x->sign_wait == -2) {
			m17_engine_sig_give(&x->e, NULL);
		} else if (x->sign && x->sign_wait >= 0 && x->sign_wait-- == 0) {
			CHECK(p256_sign(sig_d, h, rs, &p256s), "sign");
			m17_engine_sig_give(&x->e, rs);
			x->gives++;
		}
	}
	if (m17_engine_sig_take(&x->e, &x->chk))
		x->checks++;
	if (x->take && x->e.pkt_hold.full) {
		x->pkt_n = m17_engine_packet_take(&x->e, &x->info, x->pkt, sizeof x->pkt);
		x->taken++;
	}
}

static uint32_t ticks_all;

static void tick(void)
{
	ticks_all++;
	for (uint8_t i = 0; i < 2; i++)
		if (r[i].make_every && ticks_all % r[i].make_every == 0)
			r[i].tx_made++;
	step(0);
	step(1);
	core_poll(0);
	core_poll(1);
}

static void idle(uint16_t ticks)
{
	while (ticks--)
		tick();
}

static m17_engine_view_t view(uint8_t i)
{
	m17_engine_view_t v;

	m17_engine_view(&r[i].e, &v);
	return v;
}

// Radio i transmits: prepare, key, go; the operator lets go after hold ticks (30 ms each); until the transmission is
// done. Returns the ticks it took
static uint32_t send(uint8_t i, uint8_t kind, uint32_t hold, uint32_t ts)
{
	m17_tx_req_t q;
	uint32_t n = 0;

	memset(&q, 0, sizeof q);
	q.kind = kind;
	q.ts = ts;
	r[i].tx_taken = 0;
	for (uint8_t k = 0; k < sizeof q.rnd; k++)
		q.rnd[k] = (uint8_t)rng();
	CHECK(m17_engine_tx_prepare(&r[i].e, &q), "prepare kind %u", kind);
	CHECK(m17_engine_tx_go(&r[i].e), "go");
	r[i].phases = 0;
	while (m17_engine_tx_state(&r[i].e) != AIR_TX_DONE && n < 2000000u) {
		m17_engine_view_t v;

		if (n == hold)
			m17_engine_tx_stop(&r[i].e);
		tick();
		m17_engine_view(&r[i].e, &v);
		r[i].phases |= (uint8_t)(1u << v.tx_phase);
		n++;
	}
	CHECK(m17_engine_tx_state(&r[i].e) == AIR_TX_DONE, "transmission done");
	m17_engine_tx_clear(&r[i].e);
	CHECK(m17_engine_tx_state(&r[i].e) == AIR_TX_IDLE && view(i).tx_phase == M17_TXP_IDLE, "cleared");
	r[i].tx_total += r[i].tx_taken;
	idle(4);
	return n;
}


// ── Streams ──
static void testStream(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_engine_view_t v;
	char text[M17_TEXT_MAX + 1];
	uint32_t ticks;

	printf("sizeof(m17_engine_t) on the host: %zu bytes (budget %u on the 32-bit targets)\n", sizeof(m17_engine_t),
	       M17_ENGINE_BUDGET);
	radios(&c0, &c1);
	CHECK(m17_engine_fit(&air0) == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT), "fit");
	idle(10);
	CHECK(r[1].e.stats.frames == 0 && !view(1).rx_active, "noise: no frame");
	ticks = send(0, M17_TX_STREAM, 40, 0);	// 1.2 s
	v = view(1);
	CHECK(r[1].starts == 1 && r[1].ends == 1, "starts %u ends %u", r[1].starts, r[1].ends);
	CHECK(r[1].call.src == N0CALL && r[1].call.dst == M17_ADDR_BROADCAST && r[1].call.flags & M17_CALL_LSF &&
	      !(r[1].call.flags & M17_CALL_LATE) && M17_TYPE_DATA(r[1].call.type) == M17_DATA_VOICE, "call fields");
	CHECK(r[1].vfirst == 0 && r[1].vgood == r[0].tx_taken && r[1].vbad == 0 && r[1].vlost == 0 && r[1].vsil == 0,
	      "voice: %u good of %u, %u bad, %u lost, first %u", r[1].vgood, r[0].tx_taken, r[1].vbad, r[1].vlost, r[1].vfirst);
	CHECK(r[1].e.stats.eot == 1 && r[1].e.stats.lsf_ok == 1 && r[1].e.stats.lich_bad == 0, "eot %u lsf %u lich_bad %u",
	      r[1].e.stats.eot, r[1].e.stats.lsf_ok, r[1].e.stats.lich_bad);
	CHECK(!v.rx_active && !v.rx_played && v.rx.src == N0CALL && v.rx_calls == 1 && (v.rx.fn & M17_FN_END), "view after");
	CHECK(prov[0].underruns == 0 && view(0).tx_filler == 0 && view(0).tx_calls == 1, "TX: underruns %u filler %u",
	      prov[0].underruns, view(0).tx_filler);
	CHECK(r[0].phases == (1u << M17_TXP_DATA | 1u << M17_TXP_EOT), "TX phases %02x", r[0].phases);
	CHECK(view(1).noise != 0, "noise mixed");
	printf("  stream: %u frames heard in %u ticks, %u bits corrected, head rejects %u (noise between)\n",
	       r[1].e.stats.stream, ticks, r[1].e.stats.fec_bits, r[1].e.stats.head_rejects);

	// The encoder stalls: silence covers it, the air never runs dry
	r[0].tx_always = false;
	r[0].tx_made = r[0].tx_taken = 0;
	r[1].vgood = r[1].vsil = r[1].vbad = r[1].vlost = 0;
	send(0, M17_TX_STREAM, 20, 0);
	CHECK(prov[0].underruns == 0 && view(0).tx_filler > 0 && r[1].vsil == view(0).tx_filler && r[1].vbad == 0,
	      "stalled encoder: filler %u silence heard %u underruns %u", view(0).tx_filler, r[1].vsil, prov[0].underruns);

	// META: text (four blocks), then GNSS (given before PTT: in the LSF, then text again)
	{
		static const char msg[] = "M17 engine test: a message of four META text blocks";
		m17_gnss_t g, got;

		c0.text = (const uint8_t *)msg;
		c0.text_len = (uint8_t)strlen(msg);
		c0.gnss = true;
		radios(&c0, &c1);
		memset(&g, 0, sizeof g);
		g.valid = M17_GNSS_VALID_POS | M17_GNSS_VALID_ALT;
		g.station = M17_STATION_HANDHELD;
		g.lat = 1234567;
		g.lon = -2345678;
		g.alt = 1100;
		m17_engine_gnss(&r[0].e, &g);
		// An encoder slower than the air (a frame every 60 ms): the engine waits for it, then fills in, and the text
		// still goes a block a superframe
		r[0].tx_always = false;
		r[0].make_every = 2;
		send(0, M17_TX_STREAM, 60, 0);	// 1.8 s: 7 superframes
		v = view(1);
		CHECK(m17_engine_text(&r[1].e, text) == strlen(msg) && !strcmp(text, msg), "text: '%s'", text);
		CHECK(v.rx.flags & M17_CALL_TEXT && v.text_seq >= 4, "text flag, seq %u", v.text_seq);
		got = v.rx.gnss;
		CHECK(v.rx.flags & M17_CALL_GNSS && got.lat == g.lat && got.lon == g.lon && got.alt == g.alt &&
		      got.station == g.station, "GNSS: %d %d %u", got.lat, got.lon, got.alt);
		CHECK(r[1].vgood + r[1].vsil + r[1].vbad == r[1].e.stats.stream && r[1].vsil == view(0).tx_filler &&
		      view(0).tx_filler > 0 && prov[0].underruns == 0, "voice with META: %u voice %u filler", r[1].vgood + r[1].vbad,
		      r[1].vsil);
	}
}

// A crafted transmission (what a repeater sends: extended callsign META) straight to radio 1's receiver
static void raw_deliver(const uint8_t *frames, uint16_t nframes)
{
	static uint8_t d[CHUNK * 4];
	uint32_t total = (uint32_t)nframes * M17_FRAME_SYMS;

	for (uint32_t at = 0; at < total; at += CHUNK) {
		uint16_t n = (uint16_t)(total - at < CHUNK ? total - at : CHUNK);

		for (uint16_t k = 0; k < n; k++)
			d[k] = air_sym_get(frames + ((at + k) / M17_FRAME_SYMS) * M17_FRAME_BYTES, (uint16_t)((at + k) % M17_FRAME_SYMS));
		prov[1].now_us += 30000;
		deliver(&prov[1], d, n, 0);
	}
}

static void testExtCall(void)
{
	static uint8_t frames[20][M17_FRAME_BYTES];
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	uint8_t lsf[M17_LSF_BYTES], meta[M17_META_BYTES], lich[M17_LICH_BYTES], st[M17_STREAM_BYTES];
	uint16_t type = m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_EXTCALL, 0, false);

	radios(&c0, &c1);
	memset(meta, 0, sizeof meta);
	m17_addr_put(meta, W1AW);
	m17_addr_put(meta + 6, ECHO);
	m17_lsf_make(lsf, OTHER, N0CALL, type, meta);
	m17_preamble(false, frames[0]);
	m17_frame_lsf(lsf, frames[1]);
	for (uint16_t k = 0; k < 16; k++) {
		vf(k, st);
		m17_lich_make(lsf, (uint8_t)(k % 6), lich);
		m17_frame_stream(lich, (uint16_t)(k | (k == 15 ? M17_FN_END : 0)), st, frames[2 + k]);
	}
	m17_eot(frames[18]);
	m17_eot(frames[19]);
	c1.squelch = M17_SQUELCH_ANY;
	m17_engine_set(&r[1].e, &c1);
	raw_deliver(frames[0], 20);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].vgood == 16, "extended: starts %u voice %u", r[1].starts, r[1].vgood);
	CHECK(r[1].call.flags & M17_CALL_EXTCALL && r[1].call.ext1 == W1AW && r[1].call.ext2 == ECHO && r[1].call.dst == OTHER,
	      "extended callsign: %llx %llx", (unsigned long long)r[1].call.ext1, (unsigned long long)r[1].call.ext2);
}

// Late entry at every LICH phase: radio 1 hears the stream from data frame j on (the symbols before are lost)
static void testLate(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);

	for (uint8_t j = 0; j < 12; j++) {
		uint16_t first_cnt2;

		radios(&c0, &c1);
		ch[0].mute_until = 384u + 192u * j - 50u;	// partway into the frame before
		send(0, M17_TX_STREAM, 50, 0);
		first_cnt2 = (uint16_t)(j + ((2 + 6 - j % 6) % 6));
		CHECK(r[1].starts == 1 && r[1].ends == 1 && (r[1].call.flags & M17_CALL_LATE) && (r[1].call.flags & M17_CALL_TYPE),
		      "late %u: starts %u ends %u flags %x", j, r[1].starts, r[1].ends, r[1].call.flags);
		CHECK(r[1].vfirst == first_cnt2 && r[1].vbad == 0 && r[1].vlost == 0 && r[1].vgood == r[0].tx_taken - first_cnt2,
		      "late %u: voice from FN %u (TYPE in chunk 2 at %u), %u good of %u", j, r[1].vfirst, first_cnt2, r[1].vgood,
		      r[0].tx_taken);
		CHECK(view(1).rx.src == N0CALL && view(1).rx.dst == M17_ADDR_BROADCAST && (view(1).rx.flags & M17_CALL_LSF),
		      "late %u: the LSF rebuilt", j);
	}
	printf("  late entry at LICH phases 0-5 (twice): voice from the first chunk 2 on, the LSF rebuilt\n");
}

// The squelch: CAN, everything, addressed (DST us, broadcast, what we call; another)
static void testSquelch(void)
{
	static const struct { uint8_t squelch, rx_can, tx_can; uint64_t dst, our_dst; uint8_t why; const char *what; } t[] = {
		{ M17_SQUELCH_CAN, 0, 0, M17_ADDR_BROADCAST, 0, M17_WHY_NONE, "CAN 0" },
		{ M17_SQUELCH_CAN, 3, 5, M17_ADDR_BROADCAST, 0, M17_WHY_CAN, "CAN 3, sent on 5" },
		{ M17_SQUELCH_CAN, M17_CAN_ANY, 5, M17_ADDR_BROADCAST, 0, M17_WHY_NONE, "any CAN" },
		{ M17_SQUELCH_ANY, 3, 5, OTHER, 0, M17_WHY_NONE, "squelch any" },
		{ M17_SQUELCH_ADDRESSED, 5, 5, W1AW, 0, M17_WHY_NONE, "addressed: to us" },
		{ M17_SQUELCH_ADDRESSED, 5, 5, M17_ADDR_BROADCAST, 0, M17_WHY_NONE, "addressed: broadcast" },
		{ M17_SQUELCH_ADDRESSED, 5, 5, ECHO, ECHO, M17_WHY_NONE, "addressed: what we call" },
		{ M17_SQUELCH_ADDRESSED, 5, 5, OTHER, ECHO, M17_WHY_ADDRESSED, "addressed: another" },
		{ M17_SQUELCH_ADDRESSED, 5, 4, W1AW, 0, M17_WHY_CAN, "addressed: another CAN" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		uint8_t why_seen = 0;

		c0.tx_can = t[i].tx_can;
		c0.dst = t[i].dst;
		c1.rx_can = t[i].rx_can;
		c1.squelch = t[i].squelch;
		c1.dst = t[i].our_dst;
		radios(&c0, &c1);
		{
			// (the reason, read while the stream is on)
			m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };

			CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
			for (uint16_t n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 400; n++) {
				if (n == 20)
					m17_engine_tx_stop(&r[0].e);
				tick();
				if (view(1).rx_active)
					why_seen = view(1).rx_why;
			}
			m17_engine_tx_clear(&r[0].e);
			idle(4);
		}
		CHECK(why_seen == t[i].why, "%s: why %u, want %u", t[i].what, why_seen, t[i].why);
		CHECK(t[i].why ? r[1].starts == 0 && r[1].e.stats.can_rejects == 1 && r[1].vgood == 0 :
				 r[1].starts == 1 && r[1].ends == 1 && r[1].vgood == r[0].tx_taken,
		      "%s: starts %u rejects %u voice %u", t[i].what, r[1].starts, r[1].e.stats.can_rejects, r[1].vgood);
	}
	// The CAN changes while a call plays: it stops
	{
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };

		radios(&c0, &c1);
		CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
		idle(15);
		CHECK(r[1].starts == 1 && view(1).rx_played, "playing");
		c1.rx_can = 9;
		m17_engine_set(&r[1].e, &c1);
		idle(3);
		CHECK(r[1].ends == 1 && !view(1).rx_played && view(1).rx_why == M17_WHY_CAN && view(1).rx_active, "set: stopped");
		m17_engine_tx_stop(&r[0].e);
		while (m17_engine_tx_state(&r[0].e) != AIR_TX_DONE)
			tick();
		m17_engine_tx_clear(&r[0].e);
		idle(4);
		CHECK(r[1].ends == 1 && !view(1).rx_active, "set: no second end");
	}
	printf("  squelch: CAN, any CAN, everything, addressed (to us, broadcast, what we call), a change mid-call\n");
}

// A frame lost in a fade mid-stream: played as lost, the call goes on; then a retune mid-stream
static void testFade(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);

	radios(&c0, &c1);
	ch[0].burst_from = 384 + 192 * 10 + 8;
	ch[0].burst_to = 384 + 192 * 11;
	send(0, M17_TX_STREAM, 40, 0);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].vlost == 1 && r[1].vbad == 0 && r[1].vgood == r[0].tx_taken - 1,
	      "fade: starts %u ends %u lost %u good %u of %u", r[1].starts, r[1].ends, r[1].vlost, r[1].vgood, r[0].tx_taken);
	// Retuned mid-stream (here the same channel): what comes after is another reception, joined late
	{
		m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };

		radios(&c0, &c1);
		CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
		idle(15);
		m17_engine_retuned(&r[1].e);
		idle(15);
		m17_engine_tx_stop(&r[0].e);
		while (m17_engine_tx_state(&r[0].e) != AIR_TX_DONE)
			tick();
		m17_engine_tx_clear(&r[0].e);
		idle(4);
		CHECK(r[1].starts == 2 && r[1].ends == 2 && view(1).rx_calls == 2 && (view(1).rx.flags & M17_CALL_LATE),
		      "retuned: starts %u ends %u calls %u", r[1].starts, r[1].ends, view(1).rx_calls);
	}
}

// ── Encryption ──
static void testCrypt(void)
{
	static const struct { uint8_t enc, len, rx_len; bool rx_same; bool late; const char *what; } t[] = {
		{ M17_ENC_SCRAMBLER, 1, 1, true, false, "scrambler 8" },
		{ M17_ENC_SCRAMBLER, 2, 2, true, false, "scrambler 16" },
		{ M17_ENC_SCRAMBLER, 3, 3, true, false, "scrambler 24" },
		{ M17_ENC_SCRAMBLER, 2, 2, true, true, "scrambler 16, late" },
		{ M17_ENC_SCRAMBLER, 3, 3, true, true, "scrambler 24, late" },
		{ M17_ENC_SCRAMBLER, 2, 1, true, false, "scrambler 16, an 8-bit key" },
		{ M17_ENC_SCRAMBLER, 2, 0, true, false, "scrambler 16, no key" },
		{ M17_ENC_SCRAMBLER, 2, 2, false, false, "scrambler 16, another key" },
		{ M17_ENC_AES, 16, 16, true, false, "AES-128" },
		{ M17_ENC_AES, 24, 24, true, false, "AES-192" },
		{ M17_ENC_AES, 32, 32, true, false, "AES-256" },
		{ M17_ENC_AES, 32, 32, true, true, "AES-256, late" },
		{ M17_ENC_AES, 16, 32, true, false, "AES-128, a 256-bit key" },
		{ M17_ENC_AES, 24, 3, true, false, "AES-192, a scrambler key" },
		{ M17_ENC_AES, 16, 16, false, false, "AES-128, another key" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		bool skip = t[i].rx_len != t[i].len;
		uint8_t want_why = !skip ? M17_WHY_NONE : t[i].enc == M17_ENC_AES ? M17_WHY_ENC : M17_WHY_SCR, why = 0;

		c0.tx_enc = t[i].enc;
		c0.key_len = t[i].len;
		for (uint8_t k = 0; k < 32; k++)
			c0.key[k] = (uint8_t)(0xA1 + 7 * k);
		c1.key_len = t[i].rx_len;
		memcpy(c1.key, c0.key, sizeof c1.key);
		if (!t[i].rx_same)
			c1.key[0] ^= 0x40;
		radios(&c0, &c1);
		if (t[i].late)
			ch[0].mute_until = 384 + 192 * 21 - 10;
		{
			m17_tx_req_t q = { M17_TX_STREAM, 217123200u, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 } };

			CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "%s: key", t[i].what);
			for (uint16_t n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 400; n++) {
				if (n == 50)
					m17_engine_tx_stop(&r[0].e);
				tick();
				if (view(1).rx_active && view(1).rx_why)
					why = view(1).rx_why;
			}
			m17_engine_tx_clear(&r[0].e);
			idle(4);
		}
		CHECK(why == want_why && r[1].e.stats.enc_skipped == (skip ? 1u : 0u), "%s: why %u enc_skipped %u", t[i].what, why,
		      r[1].e.stats.enc_skipped);
		CHECK(M17_TYPE_ENC(view(1).rx.type) == t[i].enc, "%s: TYPE %04x", t[i].what, view(1).rx.type);
		if (skip) {
			CHECK(r[1].starts == 0 && r[1].vgood + r[1].vbad == 0, "%s: played", t[i].what);
		} else if (!t[i].rx_same) {
			// No integrity in either cipher: a key of the right size plays, as noise
			CHECK(r[1].starts == 1 && r[1].vgood == 0 && r[1].vbad > 0, "%s: %u good %u bad", t[i].what, r[1].vgood, r[1].vbad);
		} else {
			uint32_t from = t[i].late ? r[1].vfirst : 0;

			CHECK(r[1].starts == 1 && r[1].vbad == 0 && r[1].vgood == r[0].tx_taken - from, "%s: %u good of %u (from %u) %u bad",
			      t[i].what, r[1].vgood, r[0].tx_taken, from, r[1].vbad);
			// Joined at frame 21 (LICH_CNT 3): TYPE at 26 (chunk 2), the whole LSF (the AES nonce) at 29
			if (t[i].late)
				CHECK(t[i].enc == M17_ENC_AES ? from == 29 : from == 26, "%s: from FN %u", t[i].what, from);
		}
	}
	// Refused: a key the channel's encryption can't use, or none
	{
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };

		c0.tx_enc = M17_ENC_AES;
		c0.key_len = 20;
		radios(&c0, &c1);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "AES with a 20-byte key refused");
		c0.tx_enc = M17_ENC_SCRAMBLER;
		memset(c0.key, 0, sizeof c0.key);
		for (uint8_t len = 0; len <= 4; len++) {
			c0.key_len = len;
			m17_engine_set(&r[0].e, &c0);
			CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "an all-zero scrambler seed of %u bytes refused", len);
		}
		c0.key[0] = c0.key[1] = c0.key[2] = c0.key[3] = 0x5A;
		c0.key_len = 0;
		m17_engine_set(&r[0].e, &c0);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "a scrambler seed of 0 bytes refused");
		c0.key_len = 4;
		m17_engine_set(&r[0].e, &c0);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "a scrambler seed of 4 bytes refused");
		CHECK(m17_scr_key_ok(c0.key, 1) && m17_scr_key_ok(c0.key, 3) && !m17_scr_key_ok(NULL, 2) &&
		      !m17_scr_key_ok(c0.key, 4), "m17_scr_key_ok");
		c0.tx_enc = M17_ENC_OTHER;
		m17_engine_set(&r[0].e, &c0);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "encryption type 11 refused");
		c0 = cfg_default(0);
		c0.src = 0;
		m17_engine_set(&r[0].e, &c0);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "no source address: refused");
		c0 = cfg_default(0);
		c0.tx_inhibit = true;
		m17_engine_set(&r[0].e, &c0);
		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "TX inhibit: refused");
		CHECK(prov[0].qn == 0 && m17_engine_tx_state(&r[0].e) == AIR_TX_IDLE, "nothing queued");
	}
	// The end wipes every secret: the key, the scrambler state, the round keys
	{
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		static const uint8_t zero[sizeof r[0].e.u > sizeof r[0].e.work ? sizeof r[0].e.u : sizeof r[0].e.work];

		c0.tx_enc = c1.tx_enc = M17_ENC_SCRAMBLER;
		c0.key_len = c1.key_len = 3;
		memset(c0.key, 0x77, 3);
		memcpy(c1.key, c0.key, sizeof c1.key);
		radios(&c0, &c1);
		send(0, M17_TX_STREAM, 20, 0);
		CHECK(r[1].vgood == r[0].tx_taken, "scrambled");
		for (uint8_t i = 0; i < 2; i++) {
			m17_engine_end(&r[i].e);
			CHECK(!memcmp(r[i].e.cfg.key, zero, sizeof r[i].e.cfg.key) && !memcmp(&r[i].e.u, zero, sizeof r[i].e.u) &&
			      !memcmp(&r[i].e.work, zero, sizeof r[i].e.work) && !prov[i].open, "end %u: secrets wiped", i);
		}
	}
	printf("  encryption: scrambler 8/16/24 and AES 128/192/256, right key, wrong size (skipped), wrong key, late join\n");
}

// ── Signatures ──
static void testSigned(void)
{
	static const struct { uint8_t enc; int wait; bool late_join, cut; const char *what; } t[] = {
		{ M17_ENC_NONE, 2, false, false, "clear" },
		{ M17_ENC_AES, 1, false, false, "AES-128" },
		{ M17_ENC_SCRAMBLER, 3, false, false, "scrambler 24" },
		{ M17_ENC_NONE, -1, false, false, "signature never given: zeros" },
		{ M17_ENC_NONE, -2, false, false, "signature refused: zeros" },
		{ M17_ENC_NONE, 2, true, false, "joined late" },
		{ M17_ENC_NONE, 2, false, true, "cut before its tail" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		bool valid;

		c0.sign = true;
		c0.dst = ECHO;
		c0.tx_enc = t[i].enc;
		c0.key_len = t[i].enc == M17_ENC_AES ? 16 : t[i].enc == M17_ENC_SCRAMBLER ? 3 : 0;
		for (uint8_t k = 0; k < 16; k++)
			c0.key[k] = (uint8_t)(0x5A ^ k);
		c1 = cfg_default(1);
		c1.key_len = c0.key_len;
		memcpy(c1.key, c0.key, sizeof c1.key);
		c1.squelch = M17_SQUELCH_ANY;
		radios(&c0, &c1);
		r[0].sign = true;
		r[0].sign_wait = t[i].wait;
		if (t[i].late_join)
			ch[0].mute_until = 384 + 192 * 7 - 10;
		if (t[i].cut) {
			m17_tx_req_t q = { M17_TX_STREAM, 217123200u, { 9 } };

			CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
			idle(20);
			m17_engine_tx_abort(&r[0].e);
			idle(12);
			m17_engine_tx_clear(&r[0].e);
		} else {
			send(0, M17_TX_STREAM, 30, 217123200u);
		}
		CHECK(r[1].checks == 1, "%s: %u checks", t[i].what, r[1].checks);
		if (t[i].cut) {
			CHECK(r[1].chk.state == M17_SIG_INCOMPLETE && view(1).rx.sig == M17_SIG_INCOMPLETE, "%s: state %u", t[i].what,
			      r[1].chk.state);
			continue;
		}
		if (t[i].late_join) {
			CHECK(r[1].chk.state == M17_SIG_LATE, "%s: state %u", t[i].what, r[1].chk.state);
			continue;
		}
		CHECK(r[0].wants >= 1 && r[1].chk.state == M17_SIG_PENDING && !memcmp(r[1].chk.hash, r[0].want_h, 32),
		      "%s: state %u, the same H", t[i].what, r[1].chk.state);
		// The timestamp is the authentication frame's own: key-up's plus the frames before it (m17.md 7.1)
		{
			uint32_t ts = 217123200u + (r[0].tx_taken + c0.sig_tail) * M17_FRAME_MS / 1000u;

			CHECK(r[1].chk.signer == N0CALL && r[1].chk.dst == ECHO && r[1].chk.ts == ts && ts > 217123200u &&
			      r[1].chk.src == N0CALL, "%s: authentication frame, ts %u want %u", t[i].what, r[1].chk.ts, ts);
		}
		valid = p256_verify(sig_q, r[1].chk.hash, r[1].chk.rs, &p256s);
		if (t[i].wait >= 0) {
			CHECK(valid && r[0].e.stats.sig_late == 0 && r[0].gives == 1, "%s: verify %u late %u", t[i].what, valid,
			      r[0].e.stats.sig_late);
		} else {
			uint8_t z[64] = { 0 };

			CHECK(!valid && r[0].e.stats.sig_late == 1 && !memcmp(r[1].chk.rs, z, 64), "%s: verify %u late %u", t[i].what,
			      valid, r[0].e.stats.sig_late);
		}
		// The tail: sig_tail frames of silence, played as voice; the five tail frames never played
		CHECK(r[1].vsil == c0.sig_tail && r[1].vbad == 0 && r[1].vgood == r[0].tx_taken, "%s: voice %u silence %u bad %u",
		      t[i].what, r[1].vgood, r[1].vsil, r[1].vbad);
		CHECK(view(1).rx.fn == M17_FN_SIG4 && view(0).sig_seq == 1 && view(1).sig_seq == 1, "%s: last FN %04x", t[i].what,
		      view(1).rx.fn);
		CHECK(r[0].phases == (1u << M17_TXP_DATA | 1u << M17_TXP_TAIL | 1u << M17_TXP_SIG | 1u << M17_TXP_EOT),
		      "%s: phases %02x", t[i].what, r[0].phases);
	}
	printf("  signed streams: signed in the radio core's place and verified (clear, AES, scrambler); zeros when late; "
	       "late join; cut short\n");
}

// A late joiner of a signed stream that learns TYPE only from the whole LSF (chunk 2 held only on the soft retry,
// so TYPE is not taken from it): the check still says LATE, not "not signed". The frames go straight to the call
// layer as the head hook leaves them in the frame buffer (m17_engine_int.h)
static void testLateSignedType(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_engine_t *e = &r[1].e;
	uint8_t lsf[M17_LSF_BYTES], meta[M17_META_BYTES];

	c1.squelch = M17_SQUELCH_ANY;
	radios(&c0, &c1);
	memset(meta, 0, sizeof meta);
	m17_lsf_make(lsf, ECHO, N0CALL, m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_TEXT, 0, true), meta);
	for (uint16_t fn = 9; fn <= 20; fn++) {
		air_frame_t f;
		uint8_t cnt = (uint8_t)(fn % M17_LICH_CHUNKS);

		memset(&f, 0, sizeof f);
		memset(e->frame, 0, sizeof e->frame);
		e->frame[M17_F_FN] = (uint8_t)((fn | (fn == 20 ? M17_FN_END : 0)) >> 8);
		e->frame[M17_F_FN + 1] = (uint8_t)fn;
		vf(fn, e->frame + M17_F_STREAM);
		m17_lich_make(lsf, cnt, e->frame + M17_F_LICH);
		e->frame[M17_F_LICH_OK] = cnt == 2 ? 2 : 1;
		e->rx_kind = M17_K_STREAM;
		e->cur_errs = 0;
		m17_eng_rx_frame(e, &f);
	}
	core_poll(1);
	CHECK(r[1].starts == 1 && r[1].ends == 1 && (view(1).rx.flags & M17_CALL_LSF) && (view(1).rx.type & M17_TYPE_SIGNED),
	      "late, TYPE from the LSF: starts %u ends %u flags %x", r[1].starts, r[1].ends, view(1).rx.flags);
	CHECK(r[1].checks == 1 && r[1].chk.state == M17_SIG_LATE && view(1).rx.sig == M17_SIG_LATE,
	      "late, TYPE from the LSF: %u checks, state %u, call %u", r[1].checks, r[1].chk.state, view(1).rx.sig);
}

// ── Packets ──
static uint8_t pkt_src[M17_PKT_DATA_MAX];

static void testPackets(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	uint32_t bad = 0, sent = 0;
	uint8_t frames_max = 0;

	radios(&c0, &c1);
	for (uint16_t len = 1; len <= M17_PKT_DATA_MAX; len++) {
		uint16_t keep = (uint16_t)(len - 1u < M17_ENGINE_PKT_KEEP - 1u ? len - 1u : M17_ENGINE_PKT_KEEP - 1u);
		uint32_t before = r[1].taken;

		pkt_src[0] = M17_PKT_SMS;
		for (uint16_t k = 1; k < len; k++)
			pkt_src[k] = (uint8_t)(k * 3 + len);
		CHECK(m17_engine_packet_load(&r[0].e, ECHO, pkt_src, len), "load %u", len);
		send(0, M17_TX_PACKET, 0, 0);
		sent++;
		if (r[1].taken != before + 1 || r[1].pkt_n != keep || memcmp(r[1].pkt, pkt_src + 1, keep))
			bad++;
		if (m17_pkt_frames(len) > frames_max)
			frames_max = m17_pkt_frames(len);
	}
	CHECK(bad == 0 && r[0].e.stats.pkt_tx == sent && r[1].e.stats.pkt_rx == sent && r[1].e.stats.pkt_bad == 0,
	      "packets 1..823: %u wrong, sent %u received %u bad %u", bad, r[0].e.stats.pkt_tx, r[1].e.stats.pkt_rx,
	      r[1].e.stats.pkt_bad);
	CHECK(frames_max == 33, "the longest takes %u frames", frames_max);
	CHECK(!m17_engine_packet_load(&r[0].e, ECHO, pkt_src, 0) && !m17_engine_packet_load(&r[0].e, ECHO, pkt_src, 824) &&
	      !m17_engine_packet_load(&r[0].e, ECHO, NULL, 5), "loads refused");
	{
		m17_tx_req_t q = { M17_TX_PACKET, 0, { 0 } };

		CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "a packet without its data: refused");
	}
	printf("  packets: every length 1..823 bytes (1 to %u frames) sent and taken, the payload's first %u bytes kept\n",
	       frames_max, M17_ENGINE_PKT_KEEP - 1);

	// The inbox is full: the next packet is dropped and counted, the one waiting stays as it was; a stream heard and a
	// stream sent leave it too; a loaded packet is the caller's buffer, untouched by a stream heard before it goes
	{
		static uint8_t a[60], b[90], p1[200], p1_copy[200];
		m17_packet_info_t info;
		uint8_t buf[200];
		uint16_t n;

		radios(&c0, &c1);
		r[1].take = false;
		a[0] = b[0] = M17_PKT_SMS;
		for (uint8_t k = 1; k < sizeof a; k++)
			a[k] = (uint8_t)('a' + k % 26);
		for (uint8_t k = 1; k < sizeof b; k++)
			b[k] = (uint8_t)('B' + k % 20);
		CHECK(m17_engine_packet_load(&r[0].e, W1AW, a, sizeof a), "load a");
		send(0, M17_TX_PACKET, 0, 0);
		CHECK(view(1).pkt_seq == 1 && r[1].e.pkt_hold.full, "a waits");
		CHECK(m17_engine_packet_load(&r[0].e, W1AW, b, sizeof b), "load b");
		send(0, M17_TX_PACKET, 0, 0);
		CHECK(r[1].e.stats.pkt_dropped == 1 && r[1].e.stats.pkt_rx == 1 && view(1).pkt_seq == 1, "b dropped: %u",
		      r[1].e.stats.pkt_dropped);
		send(0, M17_TX_STREAM, 20, 0);	// heard while a waits
		r[0].take = false;
		for (uint16_t k = 0; k < sizeof p1; k++)
			p1[k] = p1_copy[k] = (uint8_t)(k ^ 0x5C);
		p1[0] = p1_copy[0] = M17_PKT_RAW;
		CHECK(m17_engine_packet_load(&r[0].e, W1AW, p1, sizeof p1), "load p1");
		send(1, M17_TX_STREAM, 20, 0);	// radio 0 hears a stream with p1 loaded
		send(1, M17_TX_STREAM, 20, 0);	// radio 1 sends with a waiting
		CHECK(r[0].starts == 2 && r[0].vgood == r[1].tx_total, "radio 0 heard both: starts %u voice %u of %u bad %u lost %u sil %u", r[0].starts, r[0].vgood, r[1].tx_total, r[0].vbad, r[0].vlost, r[0].vsil);
		n = m17_engine_packet_take(&r[1].e, &info, buf, sizeof buf);
		CHECK(n == sizeof a - 1 && !memcmp(buf, a + 1, n) && info.src == N0CALL && info.dst == W1AW && info.type == M17_PKT_SMS &&
		      !info.cut, "a intact: %u bytes, type %u", n, info.type);
		CHECK(m17_engine_packet_take(&r[1].e, &info, buf, sizeof buf) == 0, "taken once");
		r[1].take = true;
		send(0, M17_TX_PACKET, 0, 0);
		CHECK(!memcmp(p1, p1_copy, sizeof p1), "the loaded packet untouched");
		CHECK(r[1].taken == 1 && r[1].pkt_n == M17_ENGINE_PKT_KEEP - 1 && !memcmp(r[1].pkt, p1 + 1, r[1].pkt_n),
		      "p1 received after the streams: %u bytes", r[1].pkt_n);
		{
			m17_tx_req_t q = { M17_TX_PACKET, 0, { 0 } };

			CHECK(!m17_engine_tx_prepare(&r[0].e, &q), "sent: loaded again before the next");
		}
	}
	printf("  inbox: a packet while one waits dropped and counted, the one waiting intact through streams heard and "
	       "sent; a loaded packet untouched\n");
}

// ── BERT ──
static void testBert(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_engine_view_t v;
	uint32_t ticks;

	radios(&c0, &c1);
	ticks = send(0, M17_TX_BERT, 100, 0);	// 3 s
	v = view(1);
	CHECK(prov[0].tx_syms % M17_FRAME_SYMS == 0 && prov[0].tx_syms / M17_FRAME_SYMS == r[1].e.stats.bert + 2u,
	      "BERT: %u symbols sent, %u frames heard", prov[0].tx_syms, r[1].e.stats.bert);
	CHECK(v.bert_locks == 1 && v.bert_losses == 0 && v.bert_errs == 0 && v.bert_state == M17_BERT_COUNT &&
	      v.bert_frames == r[1].e.stats.bert && v.bert_bits > 197u * (v.bert_frames - 1u), "BERT: locks %u errs %u bits %u",
	      v.bert_locks, v.bert_errs, v.bert_bits);
	// Ends at the stop: at most the frame being sent and one more after it
	CHECK(v.bert_frames <= (100u * CHUNK) / M17_FRAME_SYMS + 5u && v.bert_frames >= (100u * CHUNK) / M17_FRAME_SYMS - 5u,
	      "BERT frames %u for 100 ticks held (%u ticks in all)", v.bert_frames, ticks);
	// With noise: errors counted, lock kept
	radios(&c0, &c1);
	ch[0].sigma = 23;
	send(0, M17_TX_BERT, 150, 0);
	v = view(1);
	printf("  BERT: clean %u frames 0 errors; soft noise sigma 23: %u frames, %u errors in %u bits (%.2e), locks %u losses %u\n",
	       r[1].e.stats.bert, v.bert_frames, v.bert_errs, v.bert_bits, v.bert_bits ? (double)v.bert_errs / v.bert_bits : 0.0,
	       v.bert_locks, v.bert_losses);
	CHECK(v.bert_locks >= 1 && v.bert_bits > 20000u && v.bert_errs * 100u < v.bert_bits, "noisy BERT");
}

// ── The FN limit ──
// Encrypted and signed streams end with FN 7FFA (the end bit on it, or the signed tail ending there); a clear unsigned
// one wraps at 7FFF and goes on. 32,763 frames are 22 minutes on air. The signed one is heard whole (its check needs
// every frame); the others from a few hundred frames before the limit or the wrap: a late join at a high FN, the
// scrambler's seek and AES's nonce from the LICH there
static void testFnLimit(void)
{
	static const struct { uint8_t enc; bool sign; const char *what; } t[] = {
		{ M17_ENC_AES, false, "AES" },
		{ M17_ENC_SCRAMBLER, false, "scrambler" },
		{ M17_ENC_NONE, true, "signed" },
		{ M17_ENC_NONE, false, "clear, unsigned" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		m17_tx_req_t q = { M17_TX_STREAM, 1, { 7 } };
		bool clear = t[i].enc == M17_ENC_NONE && !t[i].sign;
		uint32_t n = 0, wraps = 0, from = t[i].sign ? 0u : clear ? 0x7F00u : 0x7E00u;
		uint16_t last = 0;

		c0.tx_enc = t[i].enc;
		c0.sign = t[i].sign;
		c0.key_len = t[i].enc == M17_ENC_AES ? 32 : t[i].enc == M17_ENC_SCRAMBLER ? 2 : 0;
		memset(c0.key, 0x3D, sizeof c0.key);
		c1.key_len = c0.key_len;
		memcpy(c1.key, c0.key, sizeof c1.key);
		radios(&c0, &c1);
		r[0].sign = true;
		prov[1].deaf = from != 0;
		CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
		for (; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 50000u; n++) {
			if (r[0].e.tx_fn >= from)
				prov[1].deaf = false;
			// The clear one: let go 100 frames after the wrap
			if (clear && wraps && (r[1].e.call.fn & M17_FN_MASK) >= 100u)
				m17_engine_tx_stop(&r[0].e);
			tick();
			if ((r[1].e.call.fn & M17_FN_MASK) < (last & M17_FN_MASK))
				wraps++;
			last = r[1].e.call.fn;
		}
		CHECK(m17_engine_tx_state(&r[0].e) == AIR_TX_DONE, "%s: done", t[i].what);
		if (clear) {
			// (every frame from the first played, across 7FFF -> 0, to the last with its end bit)
			CHECK(wraps == 1 && r[0].e.stats.fn_limit == 0 && r[1].starts == 1 && r[1].ends == 1 && r[1].vbad == 0 &&
			      (view(1).rx.fn & M17_FN_END) &&
			      r[1].vgood == (view(1).rx.fn & M17_FN_MASK) + 0x8000u - r[1].vfirst + 1u, "%s: wraps %u, %u frames heard",
			      t[i].what, wraps, r[1].vgood);
		} else {
			CHECK(r[0].e.stats.fn_limit == 1 && wraps == 0, "%s: fn_limit %u", t[i].what, r[0].e.stats.fn_limit);
			CHECK(t[i].sign ? view(1).rx.fn == M17_FN_SIG4 && r[1].checks == 1 && r[1].chk.state == M17_SIG_PENDING &&
					  p256_verify(sig_q, r[1].chk.hash, r[1].chk.rs, &p256s) :
					  view(1).rx.fn == (M17_FN_ENC_LAST | M17_FN_END),
			      "%s: last FN %04x", t[i].what, view(1).rx.fn);
			// 22 minutes signed: the authentication frame's time is its own, 1310 s after key-up's, never "old"
			// against a receiver's clock for being long
			if (t[i].sign)
				CHECK(r[1].chk.ts == 1u + (uint32_t)M17_FN_AUTH * M17_FRAME_MS / 1000u, "%s: auth ts %u", t[i].what,
				      r[1].chk.ts);
			CHECK(r[1].vgood + r[1].vsil == (uint32_t)M17_FN_ENC_LAST + 1u - r[1].vfirst && r[1].vbad == 0 &&
			      r[1].ends == 1 && (t[i].sign ? r[1].vfirst == 0 : r[1].vfirst > from),
			      "%s: %u voice frames from %u, %u bad", t[i].what, r[1].vgood + r[1].vsil, r[1].vfirst, r[1].vbad);
		}
		m17_engine_tx_clear(&r[0].e);
		idle(4);
		printf("  FN limit, %s: %u ticks, heard from FN %04x, last FN %04x, fn_limit %u\n", t[i].what, n, r[1].vfirst,
		       view(1).rx.fn, r[0].e.stats.fn_limit);
	}
}

// The FN limit with an encoder slower than the air: the engine waits for its frame at the limit, several times, and
// the forced end is still one (fn_limit 1). The stream is moved near the limit (the transmitter's FN set by hand: the
// receiver takes the gap as frames lost, AES needs only the FN)
static void testFnLimitSlow(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_tx_req_t q = { M17_TX_STREAM, 1, { 7 } };
	uint32_t n = 0;

	c0.tx_enc = M17_ENC_AES;
	c0.key_len = c1.key_len = 16;
	memset(c0.key, 0x3D, sizeof c0.key);
	memcpy(c1.key, c0.key, sizeof c1.key);
	radios(&c0, &c1);
	r[0].tx_always = false;
	r[0].make_every = 2;
	CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
	for (; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 2000u; n++) {
		if (n == 10)
			r[0].e.tx_fn = (uint16_t)(M17_FN_ENC_LAST - 20u);
		tick();
	}
	CHECK(m17_engine_tx_state(&r[0].e) == AIR_TX_DONE && r[0].e.stats.fn_limit == 1 &&
	      view(1).rx.fn == (M17_FN_ENC_LAST | M17_FN_END) && r[1].ends == 1, "FN limit, slow encoder: fn_limit %u, last FN %04x",
	      r[0].e.stats.fn_limit, view(1).rx.fn);
	m17_engine_tx_clear(&r[0].e);
	idle(4);
}

// ── A sync one symbol from another kind's, inside a transmission (m17.md 1.2) ──
// LSF and packet differ in symbols 1 and 6, stream and BERT too (-3 in packet and stream, +3 in LSF and BERT): one of
// them turned to the other kind's makes the sync one symbol from both. Frames 2, 4, ... 16 each get it in one of the two
// places (every other frame: misses in a row are the framer's sign of a lost signal);
// every frame still comes out as its own kind and the reception is one. On hard decisions the framer takes the first
// pattern listed within one symbol (stream before packet before LSF before BERT): a BERT frame's sync reads as a
// stream's, and the expected kind decides. On soft values neither pattern's fit holds (a symbol 6 levels off): the
// frame is placed by counting, and the grid keeps the framer from a chance sync in its payload
static uint8_t sync_ids;		// the sync patterns radio 1's framer saw, by air_sync_t.id

static void testSyncErrors(void)
{
	static const struct { uint8_t kind; uint8_t to; uint8_t other; const char *what; } t[] = {
		{ M17_TX_STREAM, 1, 5, "stream" },
		{ M17_TX_PACKET, 1, 4, "packet" },
		{ M17_TX_BERT, 3, 2, "BERT" },
	};

	for (uint8_t hard = 0; hard < 2; hard++) {
		for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
			m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
			uint32_t first = t[i].kind == M17_TX_BERT ? 192 : 384;
			m17_tx_req_t q = { t[i].kind, 0, { 0 } };
			uint16_t n;

			hard_only = hard;
			radios(&c0, &c1);
			for (uint8_t k = 0; k < 8; k++) {
				ch[0].set_at[k] = first + 192u * (2u + 2u * k) + (k & 1 ? 6u : 1u);
				ch[0].set_to[k] = t[i].to;	// +3 (01) for a -3, -3 (11) for a +3
			}
			ch[0].nset = 8;
			if (t[i].kind == M17_TX_PACKET) {
				for (uint16_t k = 0; k < 450; k++)
					pkt_src[k] = (uint8_t)(k * 11);
				pkt_src[0] = M17_PKT_SMS;
				CHECK(m17_engine_packet_load(&r[0].e, ECHO, pkt_src, 450), "load");
			}
			sync_ids = 0;
			r[0].tx_taken = 0;
			CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
			for (n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 400; n++) {
				if (n == 40 || t[i].kind == M17_TX_PACKET)
					m17_engine_tx_stop(&r[0].e);
				tick();
				if (r[1].e.session.fr.sync_seen)
					sync_ids |= (uint8_t)(1u << r[1].e.session.fr.sync_id);
			}
			m17_engine_tx_clear(&r[0].e);
			idle(4);
			if (hard && t[i].kind == M17_TX_BERT)
				CHECK(sync_ids & (1u << t[i].other), "%s, hard: the framer saw a stream sync (ids %02x)", t[i].what,
				      sync_ids);
			if (t[i].kind == M17_TX_PACKET) {
				CHECK(r[1].taken == 1 && r[1].e.stats.pkt_rx == 1 && r[1].e.stats.packet == m17_pkt_frames(450),
				      "%s: packet %u frames %u", t[i].what, r[1].e.stats.pkt_rx, r[1].e.stats.packet);
			} else if (t[i].kind == M17_TX_STREAM) {
				CHECK(r[1].starts == 1 && r[1].vgood == r[0].tx_taken && r[1].vlost == 0, "%s: voice %u of %u", t[i].what,
				      r[1].vgood, r[0].tx_taken);
			} else {
				CHECK(view(1).bert_errs == 0 && view(1).bert_losses == 0 && view(1).bert_frames == r[1].e.stats.bert &&
				      prov[0].tx_syms / M17_FRAME_SYMS == r[1].e.stats.bert + 2u, "%s: frames %u errs %u", t[i].what,
				      view(1).bert_frames, view(1).bert_errs);
			}
			CHECK(view(1).rx_calls == 1 && r[1].e.stats.expect_miss == 0, "%s %u: one reception (%u), expect_miss %u",
			      t[i].what, hard, view(1).rx_calls, r[1].e.stats.expect_miss);
			printf("  sync 1 symbol from the other kind's, %s, %s: every frame its own kind (syncs seen %02x, misses %u)\n",
			       t[i].what, hard ? "hard decisions" : "soft values", sync_ids, r[1].e.session.fr.stats.misses);
		}
	}
	hard_only = false;
}

// ── Through the soft modem: a discriminator tap at 8 kHz ──
#define FS		240000
#define SPAN		8
#define OSR		(FS / 4800)
#define LEAD		2000
#define LPF_HALF	120
#define SM_SYMS		60000
#define WAVE_N		(SM_SYMS * OSR + 2 * LEAD)
static double wave[WAVE_N];
static int16_t tap[WAVE_N / 30 + 20000];
static uint32_t tap_n;
static uint8_t sm_air[SM_SYMS];
static uint32_t sm_air_n;

static void modulate(double alpha, double offset_hz)
{
	static double h[2 * SPAN * OSR + 1];
	int n = 2 * SPAN * OSR + 1;
	double sum = 0;

	for (int i = 0; i < n; i++) {
		double x = (double)(i - SPAN * OSR) / OSR, v;

		if (fabs(x) < 1e-9)
			v = 1 - alpha + 4 * alpha / M_PI;
		else if (fabs(fabs(4 * alpha * x) - 1) < 1e-6)
			v = alpha / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
		else
			v = (sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * cos(M_PI * x * (1 + alpha))) /
			    (M_PI * x * (1 - (4 * alpha * x) * (4 * alpha * x)));
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= OSR / sum;
	for (uint32_t i = 0; i < WAVE_N; i++)
		wave[i] = offset_hz;
	for (uint32_t k = 0; k < sm_air_n; k++) {
		long at = LEAD + (long)k * OSR - SPAN * OSR;
		double lv = air_sym_level(sm_air[k]) * 800.0;

		for (int i = 0; i < n; i++)
			if (at + i >= 0 && at + i < (long)WAVE_N)
				wave[at + i] += lv * h[i];
	}
}

// wave through the tap's low-pass, sampled at 8 kHz, 1000 counts a kHz, white noise of sigma counts; 0.4 s of
// discriminator noise without a carrier ahead and after
static void sample_tap(double sigma)
{
	static double lpf[2 * LPF_HALF + 1];
	uint32_t wave_n = LEAD + sm_air_n * OSR + LEAD;
	double sum = 0;

	for (int i = -LPF_HALF; i <= LPF_HALF; i++) {
		double x = 2 * M_PI * 3600.0 / FS * i, w = 0.54 + 0.46 * cos(M_PI * i / LPF_HALF);

		lpf[i + LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += lpf[i + LPF_HALF];
	}
	tap_n = 0;
	for (uint32_t j = 0; j < 3200; j++)
		tap[tap_n++] = (int16_t)lround(4000 * gauss());
	for (uint32_t j = 11 + LPF_HALF; j + LPF_HALF < wave_n; j += FS / 8000) {
		double v = 0;

		for (int i = -LPF_HALF; i <= LPF_HALF; i++)
			v += wave[j + (uint32_t)i] * lpf[i + LPF_HALF];
		v = v / sum + sigma * gauss();
		tap[tap_n++] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	for (uint32_t j = 0; j < 3200; j++)
		tap[tap_n++] = (int16_t)lround(4000 * gauss());
}

static void sm_out(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	prov[1].now_us = c->t_us;
	if (timing) {
		double t0 = cpu_s();

		ev(&prov[1], AIR_EV_RX, c);
		rx_cpu += cpu_s() - t0;
	} else {
		ev(&prov[1], AIR_EV_RX, c);
	}
}

// Radio 0's transmission of kind into sm_air (what it sends, as sent): its provider hears nothing back
static void sm_make(uint8_t kind, uint32_t hold, uint8_t packets)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);

	radios(&c0, &c1);
	prov[1].deaf = true;
	cap_n = 0;
	ch[0].capture = true;
	for (uint8_t p = 0; p < (kind == M17_TX_PACKET ? packets : 1); p++) {
		if (kind == M17_TX_PACKET) {
			pkt_src[0] = M17_PKT_SMS;
			for (uint16_t k = 1; k < 140; k++)
				pkt_src[k] = (uint8_t)(k + p);
			CHECK(m17_engine_packet_load(&r[0].e, ECHO, pkt_src, 140), "load");
		}
		send(0, kind, hold, 0);
		// 0.1 s of random symbols between packets
		for (uint16_t k = 0; kind == M17_TX_PACKET && k < 480 && cap_n < CAP_MAX; k++)
			cap[cap_n++] = (uint8_t)(rng() & 3);
	}
	sm_air_n = cap_n < SM_SYMS ? cap_n : SM_SYMS;
	memcpy(sm_air, cap, sm_air_n);
	ch[0].capture = false;
}

typedef struct { uint32_t frames_ok, frames, whole, sent; double ber; } sm_res_t;

// The transmission in sm_air, modulated (modulate), through the tap at sigma counts (1 count = 1 Hz) into radio 1's
// engine; what it got
static sm_res_t sm_run(uint8_t kind, double sigma, uint32_t sent)
{
	static air_softmodem_t m;
	m17_engine_cfg_t c1 = cfg_default(1);
	sm_res_t res;

	memset(&res, 0, sizeof res);
	if (prov[1].open)
		m17_engine_end(&r[1].e);
	memset(&r[1], 0, sizeof r[1]);
	r[1].cfg = c1;
	r[1].take = true;
	CHECK(m17_engine_begin(&r[1].e, &air1, &r[1].cfg, &hooks, &r[1]), "begin");
	sample_tap(sigma);
	CHECK(air_softmodem_rx_open(&m, &m17_air_mode.profile, 8000, 1000, sm_out, NULL), "open the modem");
	for (uint32_t from = 0; from < tap_n; from += 256) {
		air_softmodem_rx(&m, tap + from, (uint16_t)(tap_n - from < 256 ? tap_n - from : 256), 0x40000000u + from * 125u);
		core_poll(1);
	}
	res.sent = sent;
	if (kind == M17_TX_STREAM) {
		res.frames_ok = r[1].vgood;
		res.whole = r[1].starts == 1 && r[1].ends == 1 && r[1].vgood == sent;
	} else if (kind == M17_TX_PACKET) {
		res.frames_ok = r[1].e.stats.packet;
		res.whole = r[1].e.stats.pkt_rx;
	} else {
		m17_engine_view_t v = view(1);

		res.frames_ok = v.bert_frames;
		res.ber = v.bert_bits ? (double)v.bert_errs / v.bert_bits : 1.0;
		res.whole = v.bert_losses == 0 && v.bert_locks == 1;
	}
	res.frames = r[1].e.stats.frames;
	return res;
}

static const double sigmas[] = { 0, 150, 300, 400, 500, 600, 700, 800 };
static const struct { double alpha, offset; const char *what; } shapes[] = {
	{ 0.5, 0, "RRC 0.5 (the profile's)" },
	{ 0.2, 0, "RRC 0.2 (a chip's)" },
	{ 0.5, 400, "RRC 0.5, 400 Hz off" },
	{ 0.2, -400, "RRC 0.2, -400 Hz off" },
};
#define NSIG	(sizeof sigmas / sizeof sigmas[0])
#define NSHAPE	(sizeof shapes / sizeof shapes[0])

// Frame loss against the noise for each kind, each transmit pulse, with and without a frequency offset. Bounded at
// 0.30 kHz RMS: 2 % of the frames, no wrong frame played as right
static void testSoftModem(void)
{
	uint32_t sent;
	double t0;

	printf("  soft modem, 8 kHz tap, 2400 Hz outer deviation = 2400 counts, white noise (1 count = 1 Hz RMS):\n");
	for (uint8_t kind = M17_TX_STREAM; kind <= M17_TX_BERT; kind++) {
		sm_make(kind, kind == M17_TX_PACKET ? 0 : 200, 6);	// 6 s of stream or BERT; six packets of 140 bytes
		sent = kind == M17_TX_STREAM ? r[0].tx_taken : kind == M17_TX_PACKET ? 6u * m17_pkt_frames(140) :
		       prov[0].tx_syms / M17_FRAME_SYMS - 2u;
		for (uint8_t s = 0; s < NSHAPE; s++) {
			modulate(shapes[s].alpha, shapes[s].offset);
			printf("    %s, %s: %s", kind == M17_TX_STREAM ? "stream" : kind == M17_TX_PACKET ? "packet" : "BERT",
			       shapes[s].what, kind == M17_TX_PACKET ? "packets whole, frames lost" :
			       kind == M17_TX_BERT ? "frames lost, BER" : "frames lost, wrong, LICH held");
			for (uint8_t i = 0; i < NSIG; i++) {
				sm_res_t x;
				uint32_t ok;

				rng_state = 0x5EED0000u + i * 7u + s * 131u + kind * 1009u;
				x = sm_run(kind, sigmas[i], sent);
				ok = x.frames_ok > sent ? sent : x.frames_ok;
				if (kind == M17_TX_PACKET)
					printf(" | %.2f %u/6 %4.1f%%", sigmas[i] / 1000, x.whole, 100.0 * (sent - ok) / sent);
				else if (kind == M17_TX_BERT)
					printf(" | %.2f %4.1f%% %.0e", sigmas[i] / 1000, 100.0 * (sent - ok) / sent, x.ber);
				else
					printf(" | %.2f %4.1f%% %u %u%%", sigmas[i] / 1000, 100.0 * (sent - ok) / sent, r[1].vbad,
					       r[1].e.stats.lich_ok + r[1].e.stats.lich_bad ?
					       100u * r[1].e.stats.lich_ok / (r[1].e.stats.lich_ok + r[1].e.stats.lich_bad) : 0u);
				if (sigmas[i] <= 300) {
					CHECK(ok * 100u >= sent * 98u, "%u %s at %.0f: %u of %u frames", kind, shapes[s].what, sigmas[i], ok,
					      sent);
					if (kind == M17_TX_PACKET)
						CHECK(x.whole >= 5, "packet %s at %.0f: %u of 6 whole", shapes[s].what, sigmas[i], x.whole);
					if (kind == M17_TX_BERT)
						CHECK(x.ber < 1e-3, "BERT %s at %.0f: BER %.1e", shapes[s].what, sigmas[i], x.ber);
				}
				if (sigmas[i] == 0 && kind == M17_TX_STREAM)
					CHECK(x.whole, "stream %s clean: whole", shapes[s].what);
				if (kind == M17_TX_STREAM && sigmas[i] <= 500)
					CHECK(r[1].vbad <= sent / 50u, "stream %s at %.0f: %u wrong frames played as right",
					      shapes[s].what, sigmas[i], r[1].vbad);
			}
			printf("\n");
		}
		if (kind != M17_TX_STREAM)
			continue;
		// Host time per received frame: the framer, the head hook's decode, the call layer (the modem and the
		// channel model not counted in the first figure)
		modulate(0.5, 0);
		timing = true;
		rx_cpu = 0;
		t0 = cpu_s();
		(void)sm_run(M17_TX_STREAM, 150, sent);
		timing = false;
		printf("    host time: %.1f us a stream frame received (framer and engine), %.1f us with the modem and the "
		       "channel model\n", 1e6 * rx_cpu / r[1].e.stats.frames, 1e6 * (cpu_s() - t0) / r[1].e.stats.frames);
	}
}

// ── GNSS META without a position (m17.md 4.2: a field whose validity bit is 0 is ignored) ──
// A crafted stream to radio 1: the LSF's META, then superframe 1's and 2's through the LICH. A META without its position
// bit is no position (never 0, 0), and it adds its own fields to a position already in, never wiping it
static void gnss_stream(const m17_gnss_t *g0, const m17_gnss_t *g1, const m17_gnss_t *g2)
{
	static uint8_t frames[22][M17_FRAME_BYTES];
	const m17_gnss_t *g[3] = { g0, g1, g2 };
	uint8_t lsf[3][M17_LSF_BYTES], meta[M17_META_BYTES], lich[M17_LICH_BYTES], st[M17_STREAM_BYTES];
	uint16_t type = m17_type(true, M17_DATA_VOICE, M17_ENC_NONE, M17_META_GNSS, 0, false);

	for (uint8_t k = 0; k < 3; k++) {
		m17_gnss_encode(g[k], meta);
		m17_lsf_make(lsf[k], M17_ADDR_BROADCAST, N0CALL, type, meta);
	}
	m17_preamble(false, frames[0]);
	m17_frame_lsf(lsf[0], frames[1]);
	for (uint16_t k = 0; k < 18; k++) {
		vf(k, st);
		m17_lich_make(lsf[k / 6], (uint8_t)(k % 6), lich);
		m17_frame_stream(lich, (uint16_t)(k | (k == 17 ? M17_FN_END : 0)), st, frames[2 + k]);
	}
	m17_eot(frames[20]);
	m17_eot(frames[21]);
	raw_deliver(frames[0], 22);
}

static void testGnssValid(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_gnss_t pos, vel, alt, none;
	m17_call_t c;

	memset(&pos, 0, sizeof pos);
	pos.valid = M17_GNSS_VALID_POS;
	pos.station = M17_STATION_MOBILE;
	pos.lat = 3456789;
	pos.lon = -1234567;
	vel = pos;
	vel.valid = M17_GNSS_VALID_VEL;
	vel.lat = vel.lon = 0;
	vel.bearing = 270;
	vel.speed = 88;
	alt = vel;
	alt.valid = M17_GNSS_VALID_ALT | M17_GNSS_VALID_RADIUS;
	alt.bearing = alt.speed = 0;
	alt.alt = 2345;
	alt.radius = 5;
	none = alt;
	none.valid = 0;
	none.alt = 0;
	none.radius = 0;
	// Velocity alone, then altitude alone, then nothing valid: never a position
	radios(&c0, &c1);
	gnss_stream(&vel, &alt, &none);
	c = view(1).rx;
	CHECK(r[1].starts == 1 && !(c.flags & M17_CALL_GNSS) && !(r[1].call.flags & M17_CALL_GNSS),
	      "no position: flags %x", c.flags);
	// A position, then velocity alone, then altitude and radius alone: the position kept, the others added
	radios(&c0, &c1);
	gnss_stream(&pos, &vel, &alt);
	c = view(1).rx;
	CHECK((c.flags & M17_CALL_GNSS) && c.gnss.lat == pos.lat && c.gnss.lon == pos.lon &&
	      c.gnss.valid == (M17_GNSS_VALID_POS | M17_GNSS_VALID_VEL | M17_GNSS_VALID_ALT | M17_GNSS_VALID_RADIUS) &&
	      c.gnss.bearing == 270 && c.gnss.speed == 88 && c.gnss.alt == 2345 && c.gnss.radius == 5 &&
	      c.gnss.station == M17_STATION_MOBILE, "position kept: %d %d valid %x", c.gnss.lat, c.gnss.lon, c.gnss.valid);
	// A new position replaces the fix whole: what came with the old one goes with it
	radios(&c0, &c1);
	pos.lat = 1111111;
	gnss_stream(&alt, &pos, &none);
	c = view(1).rx;
	CHECK((c.flags & M17_CALL_GNSS) && c.gnss.lat == 1111111 && c.gnss.valid == M17_GNSS_VALID_POS && c.gnss.alt == 0,
	      "the altitude before the position not kept: valid %x alt %u", c.gnss.valid, c.gnss.alt);
	printf("  GNSS META: no position from velocity, altitude or radius alone; a position kept as fields are added\n");
}

// ── The FN due after the LSF (m17_fn_due) ──
// Frame k of the stream radio 0 sends (its symbols at 384 + 192 k), from a capture: the coded bits of its stream part
// turned one by one, at random, keeping each only while the hard decoder still gets the frame right, until it has
// corrected want bits. The LICH is left whole. The flips go into radio 0's channel as symbol XORs. Returns the bits
// the pattern reached (a pattern can get stuck short of want)
static int flips_try(uint32_t at, uint16_t k, int want)
{
	static m17_fec_scratch_t fs;
	uint8_t hard[M17_FRAME_BYTES], st[M17_STREAM_BYTES], ok[M17_STREAM_BYTES];
	uint16_t fn;
	int errs = 0;

	memset(hard, 0, sizeof hard);
	for (uint16_t i = 0; i < M17_FRAME_SYMS; i++)
		air_sym_put(hard, i, cap[at + i]);
	CHECK(m17_frame_stream_get(hard, NULL, &fs, &fn, ok) == 0 && fn == k, "frame %u as sent", k);
	ch[0].nxor = 0;
	for (uint32_t tries = 0; tries < 20000u && errs < want && ch[0].nxor < 64; tries++) {
		uint16_t x = (uint16_t)(M17_LICH_T2_BITS + rng() % (M17_PAYLOAD_BITS - M17_LICH_T2_BITS));
		uint16_t p = m17_interleave(x), sym = (uint16_t)(M17_SYNC_SYMS + (p >> 1));
		uint8_t m = (p & 1u) ? 1u : 2u, d = air_sym_get(hard, sym);
		int e;
		bool taken = false;

		for (uint8_t j = 0; j < ch[0].nxor; j++)
			taken |= ch[0].xor_at[j] == at + sym && (ch[0].xor_val[j] & m);
		if (taken)
			continue;
		air_sym_put(hard, sym, (uint8_t)(d ^ m));
		e = m17_frame_stream_get(hard, NULL, &fs, &fn, st);
		if (e == errs + 1 && fn == k && !memcmp(st, ok, sizeof st)) {
			ch[0].xor_at[ch[0].nxor] = at + sym;
			ch[0].xor_val[ch[0].nxor++] = m;
			errs = e;
		} else {
			air_sym_put(hard, sym, d);
		}
	}
	return errs;
}

static void loose_flips(uint16_t k, int want)
{
	int errs = 0;

	for (uint8_t round = 0; round < 16 && errs < want; round++)
		errs = flips_try(384u + 192u * k, k, want);
	CHECK(errs == want, "frame %u: %d of %d bits turned", k, errs, want);
}

// A frame where the stream's next one is due is taken with up to 40 corrected bits (m17.md 14.2), the first after the
// LSF too (FN 0), and the first after one lost there (FN 1). Hard decisions (no soft values: the decode is the one
// loose_flips made), 36 bits: past the stream's 24 and the LICH's 32, so only the FN due takes it
static void testLooseFirst(void)
{
	static const struct { bool sign; bool lose0; const char *what; } t[] = {
		{ false, false, "frame 0 by the FN due" },
		{ false, true, "frame 0 lost, frame 1 by the FN due" },
		{ true, false, "signed, frame 0 by the FN due" },
	};

	hard_only = true;
	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		uint16_t k = t[i].lose0 ? 1u : 0u;

		c0.sign = t[i].sign;
		c1.squelch = M17_SQUELCH_ANY;
		radios(&c0, &c1);
		r[0].sign = t[i].sign;
		r[0].sign_wait = 1;
		cap_n = 0;
		ch[0].capture = true;
		send(0, M17_TX_STREAM, 30, 217123200u);
		ch[0].capture = false;
		CHECK(r[1].vgood == r[0].tx_taken && r[1].vlost == 0, "%s: clean first", t[i].what);
		loose_flips(k, 36);
		{
			chan_t keep = ch[0];

			radios(&c0, &c1);
			ch[0] = keep;
		}
		r[0].sign = t[i].sign;
		r[0].sign_wait = 1;
		if (t[i].lose0) {
			ch[0].burst_from = 384 + 8;
			ch[0].burst_to = 384 + 192;
		}
		send(0, M17_TX_STREAM, 30, 217123200u);
		CHECK(r[1].starts == 1 && r[1].ends == 1 && r[1].vlost == k && r[1].vbad == 0 && r[1].vfirst == k &&
		      r[1].vgood + k == r[0].tx_taken && r[1].vsil == (t[i].sign ? c0.sig_tail : 0u),
		      "%s: %u good of %u, %u lost, %u bad, first %u", t[i].what, r[1].vgood, r[0].tx_taken, r[1].vlost,
		      r[1].vbad, r[1].vfirst);
		if (t[i].sign)
			CHECK(r[1].checks == 1 && r[1].chk.state == M17_SIG_PENDING &&
			      p256_verify(sig_q, r[1].chk.hash, r[1].chk.rs, &p256s), "%s: check %u state %u", t[i].what,
			      r[1].checks, r[1].chk.state);
	}
	hard_only = false;
	printf("  the FN due after the LSF: frame 0 with 36 corrected bits taken, after a lost frame 0 frame 1 too; a "
	       "signed stream's check whole\n");
}

// ── PTT over a call being received (talk-over: the radio is simplex) ──
// Radio 1 plays radio 0's stream, then keys: its first data frame ends the reception (the voice path's rx_end, once),
// u becomes the transmitter's; radio 0, sending, hears nothing of it. Let go, radio 1 joins radio 0's stream late
static void testTalkOver(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };
	uint32_t n;
	bool quiet = true;

	radios(&c0, &c1);
	CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key 0");
	idle(15);
	CHECK(r[1].starts == 1 && view(1).rx_played && r[1].ends == 0, "radio 1 plays");
	r[1].tx_taken = 0;
	CHECK(m17_engine_tx_prepare(&r[1].e, &q) && m17_engine_tx_go(&r[1].e), "key 1 over the call");
	for (n = 0; n < 40 && view(1).tx_phase != M17_TXP_DATA; n++)
		tick();
	CHECK(view(1).tx_phase == M17_TXP_DATA && r[1].ends == 1 && !view(1).rx_active && !view(1).rx_played,
	      "radio 1 sends: its reception ended once (%u ends), phase %u", r[1].ends, view(1).tx_phase);
	for (n = 0; n < 27; n++) {
		tick();
		quiet = quiet && !view(1).rx_active && !view(0).rx_active;
	}
	CHECK(quiet && r[0].starts == 0, "simplex: neither hears while both send");
	m17_engine_tx_stop(&r[1].e);
	for (n = 0; m17_engine_tx_state(&r[1].e) != AIR_TX_DONE && n < 400; n++)
		tick();
	m17_engine_tx_clear(&r[1].e);
	CHECK(m17_engine_tx_state(&r[1].e) == AIR_TX_IDLE && r[1].ends == 1, "radio 1 done");
	r[1].vbad = r[1].vgood = 0;
	idle(40);
	CHECK(r[1].starts == 2 && view(1).rx_played && (view(1).rx.flags & M17_CALL_LATE) && view(1).rx_calls == 2 &&
	      r[1].vbad == 0 && r[1].vgood > 0, "joined late again: starts %u flags %x voice %u bad %u", r[1].starts,
	      view(1).rx.flags, r[1].vgood, r[1].vbad);
	m17_engine_tx_stop(&r[0].e);
	for (n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 400; n++)
		tick();
	m17_engine_tx_clear(&r[0].e);
	idle(4);
	CHECK(r[1].ends == 2 && r[0].starts == 0, "ends %u", r[1].ends);
	printf("  talk-over: PTT over a call ends its reception once, simplex while both send, the call joined late after\n");
}

// ── The key changed while a call plays (m17_engine_set): the scrambler seeks from the FN, AES needs only the key ──
static void testKeyMidCall(void)
{
	static const struct { uint8_t enc, len; const char *what; } t[] = {
		{ M17_ENC_SCRAMBLER, 2, "scrambler 16" },
		{ M17_ENC_SCRAMBLER, 3, "scrambler 24" },
		{ M17_ENC_AES, 16, "AES-128" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
		m17_tx_req_t q = { M17_TX_STREAM, 217123200u, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 } };
		uint32_t bad, n;

		c0.tx_enc = t[i].enc;
		c0.key_len = c1.key_len = t[i].len;
		for (uint8_t k = 0; k < 32; k++)
			c0.key[k] = (uint8_t)(0x3B + 11 * k);
		memcpy(c1.key, c0.key, sizeof c1.key);
		c1.key[0] ^= 0x21;	// the wrong key: plays as noise
		radios(&c0, &c1);
		CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "%s: key", t[i].what);
		idle(30);
		CHECK(r[1].starts == 1 && r[1].vgood == 0 && r[1].vbad > 10, "%s: noise %u", t[i].what, r[1].vbad);
		c1.key[0] ^= 0x21;
		m17_engine_set(&r[1].e, &c1);
		bad = r[1].vbad;
		idle(40);
		CHECK(r[1].vbad <= bad + 1u && r[1].vgood >= 25u && r[1].ends == 0, "%s: the right key mid-call: %u good, %u bad "
		      "after (%u before)", t[i].what, r[1].vgood, r[1].vbad - bad, bad);
		m17_engine_tx_stop(&r[0].e);
		for (n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 400; n++)
			tick();
		m17_engine_tx_clear(&r[0].e);
		idle(4);
		CHECK(r[1].ends == 1 && r[1].vbad <= bad + 1u, "%s: to the end", t[i].what);
	}
	printf("  the key changed mid-call: scrambler (seek from the FN) and AES decode from the next frame\n");
}

// ── A signed stream cut while its signature is wanted (TAIL, rs_state WANTED) ──
// The radio core can't give a signature to a transmission that is gone: once a reception takes u (rx_begin), sig_want
// says no and sig_give changes nothing; tx_clear ends it
static void testSigAbort(void)
{
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_tx_req_t q = { M17_TX_STREAM, 217123200u, { 9 } };
	uint8_t h[M17_HASH_BYTES], rs[M17_SIG_RS_BYTES];
	uint32_t n;

	c0.sign = true;
	c1.squelch = M17_SQUELCH_ANY;
	radios(&c0, &c1);
	r[0].sign = true;
	r[0].sign_wait = -1;	// never given
	CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
	idle(20);
	m17_engine_tx_stop(&r[0].e);
	for (n = 0; n < 50 && !(view(0).tx_phase == M17_TXP_TAIL && r[0].e.u.stream.rs_state == M17_RS_WANTED); n++)
		tick();
	CHECK(view(0).tx_phase == M17_TXP_TAIL && r[0].e.u.stream.rs_state == M17_RS_WANTED && m17_engine_sig_want(&r[0].e, h),
	      "in the tail, the signature wanted");
	m17_engine_tx_abort(&r[0].e);
	idle(4);
	CHECK(m17_engine_tx_state(&r[0].e) == AIR_TX_DONE, "aborted");
	send(1, M17_TX_STREAM, 20, 0);		// radio 0 hears radio 1: a reception takes u
	CHECK(r[0].starts == 1 && !m17_engine_sig_want(&r[0].e, h), "a reception after: no signature wanted");
	memset(rs, 0x5A, sizeof rs);
	m17_engine_sig_give(&r[0].e, rs);
	CHECK(r[0].e.u.stream.rs_state == M17_RS_NONE, "a late signature changes nothing (rs_state %u)",
	      r[0].e.u.stream.rs_state);
	m17_engine_tx_clear(&r[0].e);
	CHECK(m17_engine_tx_state(&r[0].e) == AIR_TX_IDLE && view(0).tx_phase == M17_TXP_IDLE && r[0].gives == 0,
	      "cleared");
	printf("  a signed stream cut in its tail: no signature taken after a reception, cleared\n");
}

// ── Secret work (crash_secret.h): every cipher call the engine makes, either way, runs inside it ──
static void testSecretWork(void)
{
	static const struct { uint8_t enc, len; bool late; const char *what; } t[] = {
		{ M17_ENC_SCRAMBLER, 3, false, "scrambler" },
		{ M17_ENC_SCRAMBLER, 2, true, "scrambler, joined late (the seek)" },
		{ M17_ENC_AES, 32, false, "AES" },
		{ M17_ENC_AES, 16, true, "AES, joined late" },
		{ M17_ENC_NONE, 0, false, "clear" },
	};

	for (uint8_t i = 0; i < sizeof t / sizeof t[0]; i++) {
		m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);

		c0.tx_enc = t[i].enc;
		c0.key_len = c1.key_len = t[i].len;
		c0.sign = c1.sign = t[i].enc == M17_ENC_SCRAMBLER;	// the signed tail's encryption ahead too
		memset(c0.key, 0x6E, sizeof c0.key);
		memcpy(c1.key, c0.key, sizeof c1.key);
		radios(&c0, &c1);
		r[0].sign = true;
		r[0].sign_wait = 1;
		if (t[i].late)
			ch[0].mute_until = 384 + 192 * 9 - 10;
		cipher_calls = cipher_open = 0;
		send(0, M17_TX_STREAM, 30, 217123200u);
		CHECK(r[1].vgood > 0 && r[1].vbad == 0, "%s: heard", t[i].what);
#if M17_TEST_WRAP
		CHECK(t[i].enc == M17_ENC_NONE ? cipher_calls == 0 : cipher_calls > 2u * r[1].vgood, "%s: %u cipher calls",
		      t[i].what, cipher_calls);
#endif
		CHECK(cipher_open == 0 && crash_secret == 0, "%s: %u outside secret work, count %u after", t[i].what, cipher_open,
		      crash_secret);
	}
	printf("  secret work: every scrambler and AES call, sent and received, inside the crash record's scrub%s\n",
	       M17_TEST_WRAP ? "" : " (the calls not seen: the linker has no --wrap)");
}

// ── GNSS META in a long stream (m17.md 4.2, 14.3): a new position at least 21 superframes after the last ──
// The META of each superframe, as the transmitter set it (the LSF its LICH carries): sampled every tick, each
// superframe's while its frames are built
static uint8_t sf_sub[80];
static int32_t sf_lat[80];

static void sf_sample(const m17_engine_t *e)
{
	uint16_t sf = (uint16_t)((e->tx_fn - 1u) / M17_LICH_CHUNKS);
	uint8_t meta[M17_META_BYTES];
	m17_gnss_t g;

	if (e->tx_fn == 0 || sf >= sizeof sf_sub)
		return;
	memcpy(meta, e->tx_lsf + M17_LSF_META, sizeof meta);
	sf_sub[sf] = (uint8_t)M17_TYPE_SUB(m17_lsf_type(e->tx_lsf));
	sf_lat[sf] = sf_sub[sf] == M17_META_GNSS && m17_gnss_decode(meta, &g) ? g.lat : 0;
}

static m17_gnss_t at_lat(int32_t lat)
{
	m17_gnss_t g;

	memset(&g, 0, sizeof g);
	g.valid = M17_GNSS_VALID_POS;
	g.lat = lat;
	g.lon = 77;
	return g;
}

static void testGnssCadence(void)
{
	static const char msg[] = "cadence";
	m17_engine_cfg_t c0 = cfg_default(0), c1 = cfg_default(1);
	m17_tx_req_t q = { M17_TX_STREAM, 0, { 0 } };
	m17_gnss_t g;
	uint32_t n;
	uint8_t gnss_sfs = 0, odd_seen = 0;
	bool order = true;

	c0.gnss = true;
	c0.text = (const uint8_t *)msg;
	c0.text_len = (uint8_t)strlen(msg);
	radios(&c0, &c1);
	memset(sf_sub, 0xFF, sizeof sf_sub);
	g = at_lat(100001);			// A, before PTT: the LSF
	m17_engine_gnss(&r[0].e, &g);
	CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
	sf_sub[0] = (uint8_t)M17_TYPE_SUB(m17_lsf_type(r[0].e.tx_lsf));
	sf_lat[0] = 100001;
	for (n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 1200u; n++) {
		uint16_t sf = (uint16_t)(r[0].e.tx_fn / M17_LICH_CHUNKS);

		if (n == 33) {				// B at superframe 4
			g = at_lat(200002);
			m17_engine_gnss(&r[0].e, &g);
		}
		if (n == 200) {				// C at superframe 25
			g = at_lat(300003);
			m17_engine_gnss(&r[0].e, &g);
		}
		// D: the radio core writing it (gnss_seq odd) from superframe 50 to 63's boundary: never taken half
		// written; it goes at the turn after (64)
		if (sf == 50 && !(r[0].e.gnss_seq & 1u)) {
			r[0].e.gnss_seq++;
			r[0].e.gnss_in = at_lat(-5);
			odd_seen = 1;
		}
		if (sf == 63 && r[0].e.tx_fn % M17_LICH_CHUNKS == 3 && (r[0].e.gnss_seq & 1u)) {
			r[0].e.gnss_in = at_lat(400004);
			r[0].e.gnss_seq++;
		}
		if (sf >= 70)
			m17_engine_tx_stop(&r[0].e);
		tick();
		sf_sample(&r[0].e);
	}
	m17_engine_tx_clear(&r[0].e);
	idle(4);
	for (uint8_t sf = 0; sf < 70; sf++) {
		bool want = sf == 0 || sf == 21 || sf == 42 || sf == 64;

		if (sf_sub[sf] == M17_META_GNSS)
			gnss_sfs++;
		order = order && (sf_sub[sf] == M17_META_GNSS) == want && sf_sub[sf] != 0xFF;
	}
	CHECK(order && odd_seen && gnss_sfs == 4, "GNSS at superframes 0, 21, 42, 64 only: %u of them", gnss_sfs);
	CHECK(sf_lat[0] == 100001 && sf_lat[21] == 200002 && sf_lat[42] == 300003 && sf_lat[64] == 400004,
	      "the positions: %d %d %d %d", sf_lat[0], sf_lat[21], sf_lat[42], sf_lat[64]);
	CHECK((view(1).rx.flags & M17_CALL_GNSS) && view(1).rx.gnss.lat == 400004 && r[1].vbad == 0,
	      "the receiver's last: %d", view(1).rx.gnss.lat);

	// Across the FN's wrap (an unsigned clear stream's 7FFF to 0): a new position every tick, never two GNSS
	// superframes closer than 21 (126 frames; the sample may lag the boundary by a frame or two)
	{
		uint32_t built = 0, last_gnss = 0, gaps = 0, min_gap = UINT32_MAX;
		uint16_t last_fn = 0;
		uint8_t last_sub = M17_META_TEXT;
		bool jumped = false, wrapped = false;
		int32_t lat = 1, last_lat = 0;

		radios(&c0, &c1);
		prov[1].deaf = true;
		CHECK(m17_engine_tx_prepare(&r[0].e, &q) && m17_engine_tx_go(&r[0].e), "key");
		for (n = 0; m17_engine_tx_state(&r[0].e) != AIR_TX_DONE && n < 4000u; n++) {
			uint8_t sub;

			g = at_lat(lat++);
			m17_engine_gnss(&r[0].e, &g);
			if (!jumped && built > 300u) {
				// Near the wrap, placed so a superframe starts at FN 7FFC and at FN 2 after the wrap: by FN / 6
				// both would be "superframe 0 of 21" (5460 and 0), 6 frames apart
				uint16_t d = (uint16_t)((M17_LICH_CHUNKS - r[0].e.tx_lich_cnt) % M17_LICH_CHUNKS);
				uint16_t at = (uint16_t)(0x7FB0u + (6u + 4u - (0x7FB0u + d) % 6u) % 6u);

				r[0].e.tx_fn = at;
				last_fn = at;
				jumped = true;
			}
			if (built > 1200u)
				m17_engine_tx_stop(&r[0].e);
			tick();
			built += (uint16_t)((r[0].e.tx_fn - last_fn) & M17_FN_MASK);
			wrapped = wrapped || (jumped && r[0].e.tx_fn < last_fn);
			last_fn = r[0].e.tx_fn;
			sub = (uint8_t)M17_TYPE_SUB(m17_lsf_type(r[0].e.tx_lsf));
			// (two GNSS superframes in a row tell apart by their positions: a new one every tick)
			if (sub == M17_META_GNSS) {
				m17_gnss_t got;

				(void)m17_gnss_decode(r[0].e.tx_lsf + M17_LSF_META, &got);
				if (last_sub != M17_META_GNSS || got.lat != last_lat) {
					if (gaps++ && built - last_gnss < min_gap)
						min_gap = built - last_gnss;
					last_gnss = built;
				}
				last_lat = got.lat;
			}
			last_sub = sub;
		}
		m17_engine_tx_clear(&r[0].e);
		idle(4);
		CHECK(wrapped && gaps >= 8 && min_gap >= 6u * 21u - 3u && min_gap <= 6u * 21u + 3u,
		      "across the wrap: %u GNSS superframes, the closest %u frames apart", gaps, min_gap);
	}
	printf("  GNSS META in a long stream: at superframes 0, 21, 42, a position written meanwhile taken whole after; "
	       "21 superframes apart across the FN's wrap\n");
}

int main(void)
{
	// The signing key of m17.md 7.5 (d = SHA-256 of "M17 test key")
	static const uint8_t d[32] = {
		0x3F, 0x9B, 0xC0, 0x00, 0xFA, 0xB8, 0x3A, 0xED, 0x45, 0x51, 0xEC, 0x40, 0xA5, 0xF6, 0xA7, 0xC1,
		0x78, 0xB2, 0x55, 0xBA, 0x65, 0x4C, 0x06, 0x84, 0xD2, 0x42, 0xBC, 0x8F, 0x59, 0xF8, 0xDA, 0x18,
	};

	memcpy(sig_d, d, sizeof d);
	CHECK(p256_pub_from_priv(sig_d, sig_q, &p256s), "public key");
	testStream();
	testExtCall();
	testLate();
	testSquelch();
	testFade();
	testCrypt();
	testSigned();
	testLateSignedType();
	testGnssValid();
	testLooseFirst();
	testTalkOver();
	testKeyMidCall();
	testSigAbort();
	testSecretWork();
	testGnssCadence();
	testPackets();
	testBert();
	testSyncErrors();
	testFnLimit();
	testFnLimitSlow();
	testSoftModem();
	printf("m17EngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
