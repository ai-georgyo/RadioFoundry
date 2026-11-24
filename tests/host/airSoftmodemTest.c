// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (core/air/air_softmodem.c) on a modeled channel: symbols through the mode's transmit
// pulse to a deviation waveform, a band-limited tap sampled at its own rate and phase, with a scale error, an
// offset and noise. It checks the symbols and soft values that come out, the times on them, that a whole P25 call
// decodes through the framer behind it, that noise makes no sync, and what a hole in the samples does. Then the goldens
// (P25 and NXDN output as it was before the grid rule and the per-pattern gates), each mode's gate literals against
// the prep, and M17's physical layer: random payload with the grid rule and without, the gates on M17's patterns,
// an inverted path, and the cost against P25's.
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "nxdn/nxdn.h"
#include "p25/p25.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xC0FFEE;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static double gauss(void)
{
	double u = (rng() % 1000000 + 1) / 1000001.0, v = (rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rng();
}

// ── The channel model, at 240 kHz ──
#define FS		240000
#define SPAN		8			// symbols of pulse each side
#define SYMS_MAX	12000
#define WAVE_MAX	(SYMS_MAX * 100 + 4000)
#define AIR_MAX		(SYMS_MAX * 2)		// symbols at 4800 symbols/s: the wave holds twice those at 2400
#define LEAD		2000			// model samples before symbol 0
#define LPF_HALF	120

static double wave[WAVE_MAX];			// deviation, Hz
static uint32_t wave_n;
static uint8_t air[AIR_MAX];			// one dibit per byte
static uint32_t air_n;
static double lpf[2 * LPF_HALF + 1];
static int16_t tap[WAVE_MAX / 5];
static uint32_t tap_n;

// The transmit pulse for one symbol level, osr model samples per symbol, DC gain 1 (a run of one symbol sits at
// its nominal deviation)
static void make_pulse(double *h, int osr, uint8_t shape, double alpha)
{
	int n = 2 * SPAN * osr + 1;
	double rate = (double)FS / osr, sum = 0;

	for (int i = 0; i < n; i++) {
		double t = (double)(i - SPAN * osr) / FS, v = 0;

		if (shape == AIR_SHAPE_C4FM) {
			// TIA-102.BAAA-A 9.3: the raised cosine H(f) times the shaper P(f) = (pi f T) / sin(pi f T)
			double f1 = rate * (1 - alpha) / 2, f2 = rate * (1 + alpha) / 2;

			for (double f = 2.5; f < f2; f += 5) {
				double hf = f < f1 ? 1 : 0.5 + 0.5 * cos(M_PI * (f - f1) / (f2 - f1));
				double x = M_PI * f / rate;

				v += hf * x / sin(x) * cos(2 * M_PI * f * t);
			}
		} else if (shape == AIR_SHAPE_RRC) {
			double x = t * rate;

			if (fabs(x) < 1e-9)
				v = 1 - alpha + 4 * alpha / M_PI;
			else if (fabs(fabs(4 * alpha * x) - 1) < 1e-6)
				v = alpha / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
			else
				v = (sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * cos(M_PI * x * (1 + alpha))) /
				    (M_PI * x * (1 - (4 * alpha * x) * (4 * alpha * x)));
		} else {
			v = abs(i - SPAN * osr) * 2 < osr ? 1 : 0;	// a square pulse
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

// air[] to wave[]: the symbol clock ppm fast
static void modulate(const air_profile_t *p, double ppm)
{
	static double h[2 * SPAN * 100 + 1];
	int osr = FS / p->sym_rate;

	make_pulse(h, osr, p->shape, p->shape_param / 100.0);
	wave_n = LEAD + air_n * (uint32_t)osr + LEAD;
	memset(wave, 0, wave_n * sizeof(double));
	for (uint32_t k = 0; k < air_n; k++) {
		long at = LEAD + lround(k * osr / (1 + ppm * 1e-6)) - SPAN * osr;
		double lv = air_sym_level(air[k]) * (p->dev_outer_hz / 3.0);

		for (int i = 0; i <= 2 * SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)wave_n)
				wave[at + i] += lv * h[i];
	}
}

// The model sample of symbol k's center
static double sym_at(const air_profile_t *p, uint32_t k)
{
	return LEAD + (double)k * (FS / p->sym_rate);
}

// wave[] through the tap: its low-pass (hi_hz), sampled at rate from model sample phase on, counts_per_khz, an
// offset in counts and noise (sigma, counts)
static void sample_tap(uint32_t rate, uint32_t phase, double hi_hz, double counts_per_khz, double off, double sigma)
{
	uint32_t dec = FS / rate;
	double sum = 0;

	for (int i = -LPF_HALF; i <= LPF_HALF; i++) {
		double x = 2 * M_PI * hi_hz / FS * i, w = 0.54 + 0.46 * cos(M_PI * i / LPF_HALF);

		lpf[i + LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += lpf[i + LPF_HALF];
	}
	tap_n = 0;
	for (uint32_t j = phase + LPF_HALF; j + LPF_HALF < wave_n; j += dec) {
		double v = 0;

		for (int i = -LPF_HALF; i <= LPF_HALF; i++)
			v += wave[j + (uint32_t)i] * lpf[i + LPF_HALF];
		v = v / sum / 1000 * counts_per_khz + off + sigma * gauss();
		tap[tap_n++] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

// The time of model sample j on the tap's clock (tap sample 0 is at T0)
#define T0	0xFFFF0000u		// the clock wraps inside every run
static double model_us(uint32_t phase, double j)
{
	return (j - phase - LPF_HALF) * 1e6 / FS;
}

// ── What comes out ──
#define OUT_MAX		(AIR_MAX + 2000)
#define OUT_SYNCS	160
static uint8_t out_hard[OUT_MAX];
static int8_t out_soft[OUT_MAX];
static uint32_t out_n, out_total, out_chunks;
static struct { uint32_t at; uint8_t id; double t_us; } out_sync[OUT_SYNCS];
static uint8_t out_nsync;
static uint8_t out_flags_or, out_flags_and;
static uint32_t out_gap_chunks;
static air_framer_t *out_framer;

// ── Goldens: a CRC of everything the modem hands out (each chunk's time, length, sync, flags, hard and soft symbols),
// per run, for every P25 and NXDN signal below. They were recorded before the grid rule, the per-pattern gates and the
// sign gate went in, which must leave P25's and NXDN's output as it was ──
static uint32_t run_crc;
static bool run_on, log_on;
static uint32_t run_log[192];
static uint16_t run_nlog;

static uint32_t crc_add(uint32_t c, const void *data, size_t n)
{
	const uint8_t *p = data;

	while (n--) {
		c ^= *p++;
		for (uint8_t b = 0; b < 8; b++)
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
	}
	return c;
}

static void run_close(void)
{
	if (run_on && run_nlog < sizeof(run_log) / sizeof(run_log[0]))
		run_log[run_nlog++] = ~run_crc;
	run_on = false;
}

static void run_chunk(const air_chunk_t *c)
{
	uint8_t h[10] = { (uint8_t)c->t_us, (uint8_t)(c->t_us >> 8), (uint8_t)(c->t_us >> 16), (uint8_t)(c->t_us >> 24),
			  (uint8_t)c->nsyms, (uint8_t)(c->nsyms >> 8), (uint8_t)c->sync_at, (uint8_t)(c->sync_at >> 8),
			  c->sync_at >= 0 ? c->sync_id : 0, c->flags };

	run_crc = crc_add(run_crc, h, sizeof(h));
	run_crc = crc_add(run_crc, c->hard, (size_t)(c->nsyms + 3) / 4);
	run_crc = crc_add(run_crc, c->soft, c->nsyms);
}

static void on_chunk(const air_chunk_t *c, void *ctx)
{
	(void)ctx;
	CHECK(c->nsyms >= 1 && c->nsyms <= AIR_SOFTMODEM_CHUNK && c->hard && c->soft && c->sync_at < (int)c->nsyms, "chunk shape");
	out_chunks++;
	out_total += c->nsyms;
	out_flags_or |= c->flags;
	out_flags_and &= c->flags;
	if (run_on)
		run_chunk(c);
	if (c->flags & AIR_CHUNK_GAP)
		out_gap_chunks++;
	if (c->sync_at >= 0 && out_nsync < OUT_SYNCS) {
		out_sync[out_nsync].at = out_n + (uint32_t)c->sync_at;
		out_sync[out_nsync].id = c->sync_id;
		out_sync[out_nsync].t_us = (double)(uint32_t)(c->t_us - T0);
		out_sync[out_nsync].t_us += c->sync_at * 1e6 / 4800;	// the callers that read it run 4800 symbols/s
		out_nsync++;
	}
	for (uint16_t i = 0; i < c->nsyms && out_n < OUT_MAX; i++) {
		out_hard[out_n] = air_sym_get(c->hard, i);
		out_soft[out_n++] = c->soft[i];
	}
	if (out_framer)
		air_framer_rx(out_framer, c);
}

static void out_reset(void)
{
	run_close();
	if (log_on) {
		run_on = true;
		run_crc = 0xFFFFFFFFu;
	}
	out_n = out_total = out_chunks = out_gap_chunks = 0;
	out_nsync = 0;
	out_flags_or = 0;
	out_flags_and = 0xFF;
}

// The tap's samples into the modem, in calls of the size a board's buffer gives
static void feed(air_softmodem_t *m, uint32_t rate, uint32_t from, uint32_t to, uint16_t per_call)
{
	while (from < to) {
		uint16_t n = (uint16_t)(to - from < per_call ? to - from : per_call);

		air_softmodem_rx(m, tap + from, n, T0 + (uint32_t)(((uint64_t)from * 1000000) / rate));
		from += n;
	}
}

// ── Profiles ──
static air_profile_t dmr_like(void)
{
	air_profile_t p;

	memset(&p, 0, sizeof(p));
	p.sym_rate = 4800;
	p.levels = 4;
	p.shape = AIR_SHAPE_RRC;
	p.shape_param = 20;
	p.dev_outer_hz = 1944;
	p.sync_every_syms = 288;
	p.nsyncs = 2;
	p.sync[0].id = 7;
	p.sync[0].nsyms = 24;
	memcpy(p.sync[0].syms, (uint8_t[]){ 0x75, 0x5F, 0xD7, 0xDF, 0x75, 0xF7 }, 6);
	p.sync[1].id = 9;
	p.sync[1].nsyms = 24;
	memcpy(p.sync[1].syms, (uint8_t[]){ 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D }, 6);
	return p;
}

// Random symbols with sync pattern s of profile p every `every` symbols, the first at `first`
static void air_stream(const air_profile_t *p, uint8_t s, uint32_t n, uint32_t first, uint32_t every)
{
	air_n = 0;
	for (uint32_t k = 0; k < n; k++)
		air[air_n++] = p->levels == 2 ? (uint8_t)(rng() & 2) | 1 : (uint8_t)(rng() & 3);
	for (uint32_t at = first; at + p->sync[s].nsyms <= n; at += every)
		for (uint8_t i = 0; i < p->sync[s].nsyms; i++)
			air[at + i] = air_sym_get(p->sync[s].syms, i);
}

// Symbols out against the air from the first sync found on: errors, and the soft values' RMS distance from their
// level in soft units. False when no sync was found where one was sent
static bool compare(const air_profile_t *p, uint32_t first, uint32_t every, uint32_t *nerr, uint32_t *ncmp, double *rms)
{
	uint32_t o, a;
	double se = 0;

	*nerr = *ncmp = 0;
	*rms = 0;
	if (!out_nsync)
		return false;
	// Which sent sync it is, from the tap's time: the modem's delay is a few symbols, the spacing hundreds
	o = out_sync[0].at;
	a = first;
	if (every) {
		double t = out_sync[0].t_us * p->sym_rate / 1e6;	// in symbols from tap sample 0, about

		a = first + (uint32_t)lround((t - first - (double)LEAD / (FS / p->sym_rate)) / every) * every;
	}
	for (; o < out_n && a < air_n; o++, a++) {
		int want = air_sym_level(air[a]) * (AIR_SOFT_OUTER / 3);

		(*ncmp)++;
		if (out_hard[o] != air[a])
			(*nerr)++;
		se += (double)(out_soft[o] - want) * (out_soft[o] - want);
	}
	*rms = *ncmp ? sqrt(se / *ncmp) : 0;
	return true;
}

static void testFit(void)
{
	hal_baseband_t bb;
	air_profile_t p = p25_air_mode.profile, q;
	air_softmodem_t m;

	memset(&bb, 0, sizeof(bb));
	bb.rx_rate_hz = 8000;
	bb.rx_channels = 1;
	bb.rx_hi_hz = 3400;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "no tap functions: no fit");
	bb.rx_start = (bool (*)(void))abort;
	bb.rx_read = (uint16_t (*)(int16_t *, uint16_t))abort;
	CHECK(air_softmodem_rx_fit(&p, &bb) == (AIR_FIT_RX | AIR_FIT_SOFT), "8 kHz DC coupled tap carries P25");
	bb.rx_hi_hz = 2500;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "a tap cut at 2.5 kHz can't carry 4800 symbols/s");
	bb.rx_hi_hz = 3400;
	bb.rx_lo_hz = 300;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "an audio band tap (300 Hz up) can't carry four levels");
	q = p;
	q.levels = 2;
	q.sym_rate = 1200;
	bb.rx_lo_hz = 50;
	CHECK(air_softmodem_rx_fit(&q, &bb) != 0, "two levels at 1200 symbols/s ride a 50 Hz corner");
	q.sym_rate = 600;
	CHECK(air_softmodem_rx_fit(&q, &bb) == 0, "8 kHz is past twice the filtered rate of 600 symbols/s");
	bb.rx_lo_hz = 0;
	bb.rx_channels = 2;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "a split band is not written");
	bb.rx_channels = 1;
	bb.rx_rate_hz = 7000;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "7 kHz: too slow");
	bb.rx_rate_hz = 48000;
	CHECK(air_softmodem_rx_fit(&p, &bb) != 0, "48 kHz fits");
	bb.rx_rate_hz = 96000;
	CHECK(air_softmodem_rx_fit(&p, &bb) == 0, "96 kHz needs a decimator the board must do");
	CHECK(!air_softmodem_rx_open(&m, &p, 96000, 0, on_chunk, NULL) && air_softmodem_rx_open(&m, &p, 8000, 0, on_chunk, NULL), "open follows the rate");
	CHECK(sizeof(air_softmodem_t) <= 400, "state is %zu bytes", sizeof(air_softmodem_t));	// (three 8-byte pointers here)
}

// A stream of P25-shaped symbols at every tap phase and a few tap rates: no symbol wrong, soft values on their
// levels, every sync reported at its symbol, times right
static void testClean(void)
{
	static const uint32_t rates[] = { 8000, 16000, 24000, 48000 };
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;
	double worst_rms = 0, worst_t = 0;

	air_stream(&p, 0, 2600, 100, 864);
	modulate(&p, 0);
	for (uint8_t r = 0; r < 4; r++) {
		for (uint32_t phase = 0; phase < FS / rates[r]; phase += (r ? 3 : 1)) {
			uint32_t nerr, ncmp;
			double rms;

			sample_tap(rates[r], phase, 3600, 2000, 0, 0);
			CHECK(air_softmodem_rx_open(&m, &p, rates[r], 2000, on_chunk, NULL), "open");
			out_reset();
			feed(&m, rates[r], 0, tap_n, 256);
			CHECK(out_nsync == 3, "rate %u phase %u: %u syncs", rates[r], phase, out_nsync);
			if (!compare(&p, 100, 864, &nerr, &ncmp, &rms))
				continue;
			CHECK(nerr == 0 && ncmp > 2400, "rate %u phase %u: %u errors in %u", rates[r], phase, nerr, ncmp);
			CHECK(rms < 7, "rate %u phase %u: soft RMS error %.1f", rates[r], phase, rms);
			if (rms > worst_rms)
				worst_rms = rms;
			for (uint8_t i = 0; i < out_nsync; i++) {
				double want = model_us(phase, sym_at(&p, 100 + 864u * i)), d = out_sync[i].t_us - want;

				if (fabs(d) > worst_t)
					worst_t = fabs(d);
				CHECK(fabs(d) < 12 && out_sync[i].id == 0, "rate %u phase %u sync %u: time off %.0f us", rates[r], phase, i, d);
				CHECK(i == 0 || out_sync[i].at - out_sync[i - 1].at == 864, "sync spacing %u", out_sync[i].at - out_sync[i - 1].at);
			}
			// The first chunk says a gap and no timing; the stream after the first sync says neither
			CHECK(out_gap_chunks == 1 && (out_flags_or & AIR_CHUNK_FREE_RUN) && !(out_flags_and & AIR_CHUNK_FREE_RUN), "flags or %x and %x gaps %u", out_flags_or, out_flags_and, out_gap_chunks);
		}
	}
	printf("  clean: worst soft RMS error %.1f (of %d a level), worst sync time error %.0f us\n", worst_rms, AIR_SOFT_OUTER / 3, worst_t);
}

// Scale and offset errors, a tap of unknown scale, a slow and a fast symbol clock, small calls
static void testLevels(void)
{
	static const struct { double cpk, off, ppm; uint16_t told, per_call; const char *what; } t[] = {
		{ 2000, 0, 0, 2000, 256, "nominal" },
		{ 2600, 0, 0, 2000, 256, "30 % hot" },
		{ 1300, 0, 0, 2000, 256, "35 % low" },
		{ 2000, 1500, 0, 2000, 256, "+750 Hz offset" },
		{ 2000, -1500, 0, 2000, 256, "-750 Hz offset" },
		{ 2300, 9000, 0, 2000, 256, "a converter offset of 4.5 kHz worth, hot" },
		{ 2000, 0, 0, 0, 256, "unknown scale" },
		{ 300, -200, 0, 0, 256, "unknown scale, small" },
		{ 9000, 3000, 0, 0, 256, "unknown scale, large" },
		{ 2000, 0, 40, 2000, 256, "symbol clock 40 ppm fast" },
		{ 2000, 0, -40, 2000, 256, "symbol clock 40 ppm slow" },
		{ 2000, 300, 0, 2000, 7, "calls of 7 samples" },
	};
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 4400, 900, 864);
	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t nerr, ncmp;
		double rms;

		modulate(&p, t[i].ppm);
		sample_tap(8000, 11, 3600, t[i].cpk, t[i].off, 0);
		CHECK(air_softmodem_rx_open(&m, &p, 8000, t[i].told, on_chunk, NULL), "open");
		out_reset();
		feed(&m, 8000, 0, tap_n, t[i].per_call);
		CHECK(compare(&p, 900, 864, &nerr, &ncmp, &rms), "%s: no sync", t[i].what);
		// A clock 40 ppm off moves 0.035 of a symbol between two syncs: no errors still
		CHECK(out_nsync >= 4 && nerr == 0 && ncmp > 3000 && rms < 9, "%s: %u syncs, %u errors in %u, RMS %.1f", t[i].what, out_nsync, nerr, ncmp, rms);
	}
}

// Noise on the tap: the symbol error rate stays near what the levels allow, and syncs are still found
static void testNoise(void)
{
	// sigma in counts at 2000 counts/kHz: a level is 600 Hz = 1200 counts
	static const struct { double sigma; double ser_max; } t[] = { { 150, 0.0005 }, { 300, 0.006 }, { 450, 0.04 }, { 600, 0.10 } };
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 11000, 500, 864);
	modulate(&p, 0);
	for (uint8_t i = 0; i < 4; i++) {
		uint32_t nerr, ncmp;
		double rms;

		sample_tap(8000, 17, 3600, 2000, 400, t[i].sigma);
		air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL);
		out_reset();
		feed(&m, 8000, 0, tap_n, 256);
		CHECK(compare(&p, 500, 864, &nerr, &ncmp, &rms), "sigma %.0f: no sync", t[i].sigma);
		printf("  noise %.0f counts: %u syncs of 13, symbol errors %u of %u (%.2f %%), soft RMS %.1f\n", t[i].sigma, out_nsync, nerr, ncmp, ncmp ? 100.0 * nerr / ncmp : 0, rms);
		CHECK(ncmp > 9000 && (double)nerr / ncmp <= t[i].ser_max, "sigma %.0f: %u errors in %u", t[i].sigma, nerr, ncmp);
		CHECK(out_nsync >= (i < 3 ? 13 : 8), "sigma %.0f: %u syncs", t[i].sigma, out_nsync);
	}
	// Nothing but noise, 12 s of it, at every scale the level tracker meets: no sync
	for (uint8_t k = 0; k < 2; k++) {
		air_softmodem_rx_open(&m, &p, 8000, k ? 0 : 2000, on_chunk, NULL);
		out_reset();
		for (uint8_t rep = 0; rep < 12; rep++) {
			for (uint32_t i = 0; i < 8000; i++)
				tap[i] = (int16_t)lround(3000 * gauss());
			air_softmodem_rx(&m, tap, 8000, T0 + rep * 1000000u);
		}
		CHECK(out_nsync == 0 && out_total > 57000 && (out_flags_and & AIR_CHUNK_FREE_RUN), "noise only (%u): %u syncs, %u symbols", k, out_nsync, out_total);
	}
}

// The gates only save work: with them, and with every sample fitted, the same symbols, soft values and syncs come out
static void testGate(void)
{
	static uint8_t ref_hard[OUT_MAX];
	static int8_t ref_soft[OUT_MAX];
	static const double sigma[] = { 0, 300, 600, 900, 1500, 3000 };
	uint32_t runs = 0;

	for (uint8_t prof = 0; prof < 2; prof++) {
		air_profile_t p = prof ? dmr_like() : p25_air_mode.profile, ng;
		air_softmodem_t m;

		if (prof)
			air_softmodem_profile_prep(&p);
		ng = p;
		for (uint8_t k = 0; k < ng.nsyncs; k++)
			ng.sync[k].gate_up = ng.sync[k].gate_mask = ng.sync[k].sign_val = ng.sync[k].sign_mask = 0;

		air_stream(&p, prof, 6000, 300, prof ? 288 : 864);
		modulate(&p, 0);
		for (uint8_t i = 0; i < 6; i++) {
			for (uint8_t known = 0; known < 2; known++) {
				uint32_t ref_n, ref_at[OUT_SYNCS], diff = 0;
				uint8_t ref_nsync;
				double ref_t[OUT_SYNCS];

				sample_tap(8000, 5 + i, 3600, known ? 2000 : 700, 400, sigma[i]);
				CHECK(p.sync[0].gate_mask != 0, "profile %u has a gate", prof);
				CHECK(air_softmodem_rx_open(&m, &ng, 8000, known ? 2000 : 0, on_chunk, NULL), "open");	// no gate: every sample is fitted
				out_reset();
				feed(&m, 8000, 0, tap_n, 256);
				ref_n = out_n;
				ref_nsync = out_nsync;
				memcpy(ref_hard, out_hard, out_n);
				memcpy(ref_soft, out_soft, out_n);
				for (uint8_t k = 0; k < out_nsync; k++) {
					ref_at[k] = out_sync[k].at;
					ref_t[k] = out_sync[k].t_us;
				}
				air_softmodem_rx_open(&m, &p, 8000, known ? 2000 : 0, on_chunk, NULL);
				out_reset();
				feed(&m, 8000, 0, tap_n, 256);
				CHECK(out_n == ref_n && out_nsync == ref_nsync, "profile %u sigma %.0f: %u symbols, %u syncs with the gate, %u, %u without",
				      prof, sigma[i], out_n, out_nsync, ref_n, ref_nsync);
				for (uint32_t k = 0; k < out_n && k < ref_n; k++)
					diff += out_hard[k] != ref_hard[k] || out_soft[k] != ref_soft[k];
				for (uint8_t k = 0; k < out_nsync && k < ref_nsync; k++)
					diff += out_sync[k].at != ref_at[k] || out_sync[k].t_us != ref_t[k];
				CHECK(diff == 0, "profile %u sigma %.0f known %u: %u differences", prof, sigma[i], known, diff);
				CHECK(i > 1 || out_nsync >= 5, "profile %u sigma %.0f: %u syncs", prof, sigma[i], out_nsync);	// (unknown scale: a third of the level)
				runs++;
			}
		}
	}
	printf("  gate: %u runs the same with it and without\n", runs);
}

// The other pulse shapes and rates
static void testShapes(void)
{
	air_profile_t p = dmr_like(), q;
	air_softmodem_t m;
	uint32_t nerr, ncmp;
	double rms;

	// A root raised cosine pulse through the short filter: not matched, so some eye is lost; clean symbols still
	air_stream(&p, 1, 3000, 300, 288);
	modulate(&p, 0);
	sample_tap(8000, 5, 3600, 2000, -300, 0);
	CHECK(air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL), "open");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(compare(&p, 300, 288, &nerr, &ncmp, &rms) && out_sync[0].id == 9, "RRC: sync id %u", out_sync[0].id);
	printf("  RRC 4800: %u syncs, %u errors in %u, soft RMS %.1f\n", out_nsync, nerr, ncmp, rms);
	CHECK(nerr == 0 && out_nsync == 10 && rms < 12, "RRC: %u syncs %u errors RMS %.1f", out_nsync, nerr, rms);
	// 2400 symbols/s, four levels, a 20-symbol sync
	q = p;
	q.sym_rate = 2400;
	q.dev_outer_hz = 1050;
	q.nsyncs = 1;
	q.sync[0].nsyms = 20;
	air_stream(&q, 0, 1500, 200, 192);
	modulate(&q, 0);
	sample_tap(8000, 5, 3600, 2000, 100, 0);
	CHECK(air_softmodem_rx_open(&m, &q, 8000, 2000, on_chunk, NULL), "open 2400");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(out_nsync == 7, "2400: %u syncs", out_nsync);
	// Two levels, square pulses (as a 1200 baud FSK data mode at twice the rate)
	q = p;
	q.levels = 2;
	q.sym_rate = 2400;
	q.shape = AIR_SHAPE_NRZ;
	q.dev_outer_hz = 3000;
	q.nsyncs = 1;
	memcpy(q.sync[0].syms, (uint8_t[]){ 0x77, 0xDF, 0x5D, 0x7F, 0xD5, 0xDD }, 6);
	air_stream(&q, 0, 1500, 200, 192);
	modulate(&q, 0);
	sample_tap(8000, 5, 3600, 1000, 100, 100);
	CHECK(air_softmodem_rx_open(&m, &q, 8000, 1000, on_chunk, NULL), "open 2-level");
	out_reset();
	feed(&m, 8000, 0, tap_n, 256);
	CHECK(out_nsync == 7, "2-level: %u syncs", out_nsync);
}

// ── A P25 call through the tap, the modem, the framer and the receive hooks ──
static p25_rx_unit_t got[40];
static uint8_t got_air[40][P25_UNIT_BYTES_MAX];
static uint8_t ngot, nlost;

static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	(void)ctx;
	if (ngot < 40) {
		got[ngot] = *u;
		memcpy(got_air[ngot], u->air, (size_t)(u->nsyms + 3) / 4);
		ngot++;
	}
}

static void on_lost(void *ctx) { (void)ctx; nlost++; }

static void air_add(const uint8_t *unit, uint16_t nsyms)
{
	for (uint16_t i = 0; i < nsyms; i++)
		air[air_n++] = air_sym_get(unit, i);
}

static void testCall(void)
{
	static uint8_t fbuf[P25_UNIT_BYTES_MAX];
	static int8_t fsoft[P25_UNIT_SYMS_MAX];
	// quiet: samples of unmodulated carrier between the noise and the call, as many transmitters key up
	static const struct { double cpk, off, sigma; uint16_t told, quiet; uint8_t units_min; const char *what; } t[] = {
		{ 2000, 0, 0, 2000, 0, 6, "clean" },
		{ 1700, 800, 250, 2000, 0, 6, "low, offset, noise" },
		{ 2000, -500, 350, 0, 0, 5, "unknown scale, noise" },	// the header can go while the level settles
		// The discriminator goes quiet before the call: the level the modem follows must not lag it
		{ 2000, -500, 100, 0, 4000, 6, "unknown scale, quiet carrier first" },
	};
	uint8_t unit[P25_UNIT_BYTES_MAX], hdr[P25_HDR_BYTES], lcraw[P25_LC_BYTES], es[P25_ES_BYTES], out[16];
	uint8_t voice[4][P25_VOICE_FRAMES][P25_IMBE_BYTES];
	p25_lc_t lc = { .lcf = P25_LCF_GROUP, .tgid = 1001, .src = 1234567 }, lcgot;
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;
	air_framer_t fr;
	p25_rx_t rx;
	uint32_t at[6], call_from, call_to;

	fill(hdr, sizeof(hdr));
	fill(es, sizeof(es));
	p25_lc_build(&lc, lcraw);
	// Idle symbols, HDU, LDU1, LDU2, LDU1, LDU2, TDULC, idle symbols; outside them the tap is noise (no carrier)
	air_n = 0;
	for (uint16_t i = 0; i < 40; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	call_from = 0;
	at[0] = air_n;
	p25_unit_begin(unit, P25_HDU_SYMS, 0x293, P25_DUID_HDU, P25_SS_UNKNOWN);
	p25_hdu_put(unit, hdr);
	air_add(unit, P25_HDU_SYMS);
	for (uint8_t k = 0; k < 4; k++) {
		at[1 + k] = air_n;
		p25_unit_begin(unit, P25_LDU_SYMS, 0x293, k & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1, P25_SS_UNKNOWN);
		for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
			fill(voice[k][n], P25_IMBE_BYTES);
			p25_ldu_voice_put(unit, n, voice[k][n]);
		}
		if (k & 1)
			p25_ldu_es_put(unit, es);
		else
			p25_ldu_lc_put(unit, lcraw);
		air_add(unit, P25_LDU_SYMS);
	}
	at[5] = air_n;
	p25_unit_begin(unit, P25_TDULC_SYMS, 0x293, P25_DUID_TDULC, P25_SS_UNKNOWN);
	p25_tdulc_put(unit, lcraw);
	air_add(unit, P25_TDULC_SYMS);
	for (uint16_t i = 0; i < 20; i++)
		air[air_n++] = (uint8_t)((i & 2) ? 3 : 1);
	call_to = air_n;
	modulate(&p, 0);

	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		static int16_t all[WAVE_MAX / 30 + 16000];
		uint32_t n = 0, lead = 6000, phase = 3 + 8 * i, first, last;
		uint8_t bad_voice = 0, k0;

		sample_tap(8000, phase, 3600, t[i].cpk, t[i].off, t[i].sigma);
		// No carrier before and after: discriminator noise, well above the signal's deviation
		first = (uint32_t)((sym_at(&p, call_from) - phase - LPF_HALF) / 30);
		last = (uint32_t)((sym_at(&p, call_to - 1) - phase - LPF_HALF) / 30);
		for (uint32_t j = 0; j < lead; j++)
			all[n++] = (int16_t)lround(t[i].off + 2.5 * t[i].cpk * gauss());
		for (uint32_t j = 0; j < t[i].quiet; j++)
			all[n++] = (int16_t)lround(t[i].off + t[i].sigma * gauss());
		for (uint32_t j = first; j <= last; j++)
			all[n++] = tap[j];
		for (uint32_t j = 0; j < 4000; j++)
			all[n++] = (int16_t)lround(t[i].off + 2.5 * t[i].cpk * gauss());

		ngot = nlost = 0;
		p25_rx_init(&rx, 0x293, on_unit, on_lost, NULL);
		air_framer_init(&fr, &p25_air_mode, &rx, fbuf, fsoft, P25_UNIT_SYMS_MAX);
		CHECK(air_softmodem_rx_open(&m, &p, 8000, t[i].told, on_chunk, NULL), "open");
		out_reset();
		out_framer = &fr;
		for (uint32_t from = 0; from < n; from += 256)
			air_softmodem_rx(&m, all + from, (uint16_t)(n - from < 256 ? n - from : 256), T0 + from * 125u);
		out_framer = NULL;
		CHECK(ngot >= t[i].units_min && ngot <= 6, "%s: %u units", t[i].what, ngot);
		if (ngot < t[i].units_min)
			continue;
		k0 = (uint8_t)(6 - ngot);		// units missed at the start
		for (uint8_t g = 0; g < ngot; g++) {
			const p25_rx_unit_t *u = &got[g];
			uint8_t k = (uint8_t)(g + k0);
			// The unit's first symbol on the tap's clock: the lead of noise and the quiet carrier, then the call
			// from sample `first`
			double want = (lead + t[i].quiet - (double)first) * 125 + model_us(phase, sym_at(&p, at[k]));
			double d = (double)(uint32_t)(u->t_us - T0) - want;

			CHECK(fabs(d) < 30, "%s unit %u: time off %.0f us", t[i].what, k, d);
			if (k == 0) {
				CHECK(u->duid == P25_DUID_HDU && p25_hdu_get(got_air[g], out) >= 0 && !memcmp(out, hdr, P25_HDR_BYTES), "%s: HDU", t[i].what);
			} else if (k == 5) {
				CHECK(u->duid == P25_DUID_TDULC && p25_tdulc_get(got_air[g], out) >= 0 && !memcmp(out, lcraw, P25_LC_BYTES), "%s: TDULC", t[i].what);
			} else {
				CHECK(u->duid == ((k - 1) & 1 ? P25_DUID_LDU2 : P25_DUID_LDU1) && u->nsyms == P25_LDU_SYMS, "%s LDU %u: duid %x", t[i].what, k, u->duid);
				if ((k - 1) & 1)
					CHECK(p25_ldu_es_get(got_air[g], out) >= 0 && !memcmp(out, es, P25_ES_BYTES), "%s: ES %u", t[i].what, k);
				else
					CHECK(p25_ldu_lc_get(got_air[g], out) >= 0 && p25_lc_parse(out, &lcgot) && lcgot.tgid == 1001 && lcgot.src == 1234567, "%s: LC %u", t[i].what, k);
				for (uint8_t v = 0; v < P25_VOICE_FRAMES; v++)
					if (p25_ldu_voice_get(got_air[g], v, out) < 0 || memcmp(out, voice[k - 1][v], P25_IMBE_BYTES))
						bad_voice++;
			}
		}
		CHECK(bad_voice == 0, "%s: %u voice frames wrong", t[i].what, bad_voice);
		CHECK(nlost == 1, "%s: the end of the call reported %u times", t[i].what, nlost);
		printf("  call, %s: %u units, framer syncs %u misses %u rejects %u\n", t[i].what, ngot, fr.stats.syncs, fr.stats.misses, fr.stats.rejects);
	}
}

// Samples lost between two calls: the modem starts over and says so
static void testHole(void)
{
	air_profile_t p = p25_air_mode.profile;
	air_softmodem_t m;

	air_stream(&p, 0, 3000, 100, 864);
	modulate(&p, 0);
	sample_tap(8000, 0, 3600, 2000, 0, 0);
	air_softmodem_rx_open(&m, &p, 8000, 2000, on_chunk, NULL);
	out_reset();
	feed(&m, 8000, 0, 2048, 256);
	CHECK(out_gap_chunks == 1 && out_nsync == 2, "before: gaps %u syncs %u", out_gap_chunks, out_nsync);
	// 40 ms of samples never arrive (the time on the next call says so)
	feed(&m, 8000, 2048 + 320, tap_n, 256);
	CHECK(out_gap_chunks == 2 && out_nsync == 4, "after: gaps %u syncs %u", out_gap_chunks, out_nsync);
	// A reset by the caller: the same
	air_softmodem_rx_reset(&m);
	feed(&m, 8000, 0, 2048, 256);
	CHECK(out_gap_chunks == 3 && out_nsync == 6, "reset: gaps %u syncs %u", out_gap_chunks, out_nsync);
}

// NXDN at both rates, for the goldens: a transmission's start (the preamble's tail with the FSW, START_ONLY) and
// frames behind it, through taps of several scales, offsets, noise and clocks; each rate's signal also through the
// profile of the other mode at its symbol rate (NXDN 9600's through P25's, P25's through NXDN 9600's), where the
// patterns turn up in payload only; and noise alone
static void testGoldenNxdn(void)
{
	static const struct { double cpk, off, sigma, ppm; uint16_t told; uint8_t rate_x; } t[] = {
		{ 2000, 0, 0, 0, 2000, 1 },
		{ 2600, 700, 0, 0, 2000, 1 },
		{ 2000, -400, 0, 0, 0, 1 },
		{ 2000, 300, 250, 0, 2000, 1 },
		{ 2000, 0, 450, 0, 0, 1 },
		{ 2000, 0, 0, 40, 2000, 2 },
		{ 1500, 200, 100, -40, 0, 3 },
	};
	const air_profile_t *nx[2] = { &nxdn_air_mode.profile, &nxdn_air_mode_9600.profile };
	const air_profile_t *p25 = &p25_air_mode.profile;
	air_softmodem_t m;

	rng_state = 0x5EED0001u;
	for (uint8_t pi = 0; pi < 3; pi++) {
		// 0, 1: NXDN at 2400 and 4800 symbols/s; 2: P25
		const air_profile_t *p = pi < 2 ? nx[pi] : p25, *x = pi == 1 ? p25 : pi == 2 ? nx[1] : NULL;

		if (pi < 2) {
			air_stream(p, 0, 300 + 20 * NXDN_FRAME_SYMS + 100, 300, NXDN_FRAME_SYMS);
			for (uint8_t i = 0; i < p->sync[1].nsyms; i++)
				air[300 - p->sync[1].frame_at + i] = air_sym_get(p->sync[1].syms, i);
		} else {
			air_stream(p, 0, 4400, 300, 864);
		}
		for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			uint32_t rate = 8000u * t[i].rate_x * (p->sym_rate == 4800 && t[i].rate_x == 3 ? 2 : 1);

			modulate(p, t[i].ppm);
			sample_tap(rate, 3 + 2 * i, 3600, t[i].cpk, t[i].off, t[i].sigma);
			if (pi < 2) {
				CHECK(air_softmodem_rx_open(&m, p, rate, t[i].told, on_chunk, NULL), "open NXDN %u", pi);
				out_reset();
				feed(&m, rate, 0, tap_n, 256);
				CHECK(out_nsync >= (t[i].sigma > 300 ? 15 : 20), "NXDN %u signal %u: %u syncs", pi, i, out_nsync);
			}
			if (x) {
				CHECK(air_softmodem_rx_open(&m, x, rate, t[i].told, on_chunk, NULL), "open across %u", pi);
				out_reset();
				feed(&m, rate, 0, tap_n, 256);
			}
		}
	}
	for (uint8_t pi = 0; pi < 2; pi++) {
		air_softmodem_rx_open(&m, nx[pi], 8000, pi ? 2000 : 0, on_chunk, NULL);
		out_reset();
		for (uint8_t rep = 0; rep < 2; rep++) {
			for (uint32_t i = 0; i < 8000; i++)
				tap[i] = (int16_t)lround(3000 * gauss());
			air_softmodem_rx(&m, tap, 8000, T0 + rep * 1000000u);
		}
	}
}

// Every run's CRC against the goldens; with --goldens, the table to paste instead
static const uint32_t golden[] = {
	0x7C0B198D, 0x64CB8862, 0x3B6BB624, 0x5ADEF43A, 0x6882A75C, 0x7585FCB1,
	0xAC8CCAA6, 0x2D86267E, 0x5CE85FC8, 0x516A11E1, 0xE3AFA260, 0x3B8D94C0,
	0xBD815252, 0x9543A6E8, 0xE1858646, 0xF6AE23CE, 0xD8351E6D, 0xC01DBD2A,
	0x791E5183, 0xE3F00C89, 0x38B6B7D4, 0x622E0ADA, 0xE1A12034, 0xF62B3B9D,
	0x955A0381, 0x08FD8F0B, 0x4E730FF6, 0xA5E41D47, 0xB5254E8B, 0xE9ADE5C4,
	0xD327A92E, 0xB2063B14, 0x392006C8, 0x8461DE1C, 0x2F0DA2F2, 0x53AC9896,
	0x6319C70F, 0xFA76F62A, 0x33545F8E, 0x7B862372, 0x2E8D160A, 0x735BA072,
	0x39037462, 0x9ED4CC9B, 0x27DA1F53, 0xBA9142AF, 0x7417A94B, 0xE663339A,
	0x8E7064E5, 0x87E3833B, 0x46465639, 0x2D3C2BFC, 0x012E3815, 0xEAF40639,
	0x492104F2, 0x57D2318F, 0x6D7AAD1B, 0xFDFE981D, 0xE1B8A11F, 0xD4A15B5F,
	0xD4A15B5F, 0xF73C070A, 0xF73C070A, 0xAF54C6CB, 0xAF54C6CB, 0xD01EDAC2,
	0xD01EDAC2, 0xD4F7458F, 0xD4F7458F, 0x44B15C7D, 0x44B15C7D, 0x89E05A09,
	0x89E05A09, 0xD754CFE5, 0xD754CFE5, 0x25887FFC, 0x25887FFC, 0x4EEFCC9F,
	0x4EEFCC9F, 0x1599420D, 0x1599420D, 0xECD3672E, 0xECD3672E, 0x5F0C327C,
	0x5F0C327C, 0x3E3A125A, 0x3E3A125A, 0xEEC79225, 0xEEC79225, 0xA2CB05DF,
	0xA2CB05DF, 0xE447139B, 0xE447139B, 0x9B931523, 0x9B931523, 0x601794FD,
	0x601794FD, 0xE786A954, 0xE786A954, 0x56E433DE, 0x56E433DE, 0x901CCEEC,
	0x901CCEEC, 0xBB00DAE7, 0xBB00DAE7, 0xD420A984, 0xD420A984, 0x302589A3,
	0xB573AD2A, 0x95382A48, 0xAF4F519C, 0x9106171D, 0x3FF9A7B2, 0x781415DB,
	0x8F1362CC, 0xCABA8B03, 0x39F602DA, 0x0F93A686, 0xFCF02591, 0xEBDAC348,
	0xC0922CCC, 0xC9F4CC26, 0xF92650AE, 0x8699854C, 0x5BDF3714, 0xF7B3EEA6,
	0x189B5600, 0x758C7207, 0xD8660C29, 0xA954D32F, 0xA3D6225F, 0xD04E5BF8,
	0xB9F83F98, 0x3AF9835C, 0x316F21D7, 0x647740E3, 0x2AF1A00D, 0x7E32333D,
	0x0632FA51, 0x8F9C13A2, 0xAA5DD5F5, 0xCAE23999, 0x93D8753B, 0x64F2F7E1,
	0xDE8159DD,
};

static void testGoldens(bool print)
{
	uint16_t diff = 0;

	run_close();
	log_on = false;
	if (print) {
		printf("static const uint32_t golden[] = {");
		for (uint16_t i = 0; i < run_nlog; i++)
			printf("%s0x%08X,", i % 6 ? " " : "\n\t", run_log[i]);
		printf("\n};\n");
		return;
	}
	CHECK(run_nlog == sizeof(golden) / sizeof(golden[0]), "%u runs, %zu goldens", run_nlog, sizeof(golden) / sizeof(golden[0]));
	for (uint16_t i = 0; i < run_nlog && i < sizeof(golden) / sizeof(golden[0]); i++) {
		if (run_log[i] != golden[i]) {
			if (diff < 8)
				printf("  golden run %u: CRC %08X, was %08X\n", i, run_log[i], golden[i]);
			diff++;
		}
	}
	CHECK(diff == 0, "%u of %u runs differ from the goldens", diff, run_nlog);
	printf("  goldens: %u runs (P25, NXDN 4800 and 9600, the test's own profiles) byte-identical\n", run_nlog);
}

// ── M17's physical layer (docs/spec/m17.md 1), as the test's own profile: 4800 symbols/s, root raised cosine 0.5,
// 2400 Hz, a 192-symbol frame opened by an 8-symbol sync of outer symbols, and a preamble whose last 16 symbols with
// the first sync are a START_ONLY pattern. LSF is stream negated and packet BERT negated ──
enum { M17_PRE_LSF = 0x10, M17_PRE_BERT, M17_STREAM, M17_PACKET, M17_LSF, M17_BERT };
#define M17_FRAME	192
#define M17_FRAMES	120		// 4.8 s: a call long enough to meet payload that looks like a sync
#define M17_EOT_SYMS	192

static air_profile_t m17_profile(bool grid)
{
	static const struct { uint8_t id, nsyms, flags; int8_t frame_at; uint8_t syms[6]; } s[6] = {
		{ M17_PRE_LSF, 24, AIR_SYNC_START_ONLY, 16, { 0x77, 0x77, 0x77, 0x77, 0x55, 0xF7 } },
		{ M17_PRE_BERT, 24, AIR_SYNC_START_ONLY, 16, { 0xDD, 0xDD, 0xDD, 0xDD, 0xDF, 0x55 } },
		{ M17_STREAM, 8, 0, 0, { 0xFF, 0x5D } },
		{ M17_PACKET, 8, 0, 0, { 0x75, 0xFF } },
		{ M17_LSF, 8, 0, 0, { 0x55, 0xF7 } },
		{ M17_BERT, 8, 0, 0, { 0xDF, 0x55 } },
	};
	air_profile_t p;

	memset(&p, 0, sizeof(p));
	p.sym_rate = 4800;
	p.levels = 4;
	p.shape = AIR_SHAPE_RRC;
	p.shape_param = 50;
	p.dev_outer_hz = 2400;
	p.timing = AIR_TIMING_CONTINUOUS;
	p.sync_any_syms = M17_FRAME;
	p.idle = 0x77;
	p.flags = grid ? AIR_PROFILE_SYNC_GRID : 0;
	p.verify = AIR_VERIFY_M17;
	p.nsyncs = 6;
	for (uint8_t i = 0; i < 6; i++) {
		p.sync[i].id = s[i].id;
		p.sync[i].nsyms = s[i].nsyms;
		p.sync[i].max_errs = 1;
		p.sync[i].flags = s[i].flags;
		p.sync[i].frame_at = s[i].frame_at;
		memcpy(p.sync[i].syms, s[i].syms, sizeof(s[i].syms));
	}
	air_softmodem_profile_prep(&p);
	return p;
}

static const air_sync_t *sync_by_id(const air_profile_t *p, uint8_t id)
{
	for (uint8_t i = 0; i < p->nsyncs; i++)
		if (p->sync[i].id == id)
			return &p->sync[i];
	return NULL;
}

// A transmission into air[]: the preamble the first frame's kind wants (LSF and the rest +3 -3 ..., BERT -3 +3 ...),
// frames of an 8-symbol sync and random payload (the first of kind `first`, the others `then`), the EoT. The frames'
// first symbols and kinds in m17_at[], m17_kind[]. With check, each payload's last 16 symbols are a CRC of the frame's
// kind and the rest: what the test's head hook reads, standing in for the mode's frame checks
static uint32_t m17_at[M17_FRAMES];
static uint8_t m17_kind[M17_FRAMES];
static uint16_t m17_n;

static uint32_t m17_check(const uint8_t *dibits, uint8_t kind)
{
	return crc_add(crc_add(0xFFFFFFFFu, &kind, 1), dibits, M17_FRAME - 8 - 16);
}

static void m17_air(uint8_t first, uint8_t then, uint16_t frames, bool check)
{
	const air_profile_t p = m17_profile(true);

	air_n = 0;
	m17_n = 0;
	for (uint16_t k = 0; k < 192; k++)
		air[air_n++] = first == M17_BERT ? (k & 1 ? 1 : 3) : (k & 1 ? 3 : 1);
	for (uint16_t f = 0; f < frames && f < M17_FRAMES; f++) {
		uint8_t kind = f ? then : first;
		const air_sync_t *s = sync_by_id(&p, kind);

		m17_at[m17_n] = air_n;
		m17_kind[m17_n++] = kind;
		for (uint8_t i = 0; i < 8; i++)
			air[air_n++] = air_sym_get(s->syms, i);
		for (uint16_t i = 8; i < M17_FRAME; i++)
			air[air_n++] = (uint8_t)(rng() & 3);
		if (check) {
			uint32_t c = m17_check(&air[air_n - M17_FRAME + 8], kind);

			for (uint8_t i = 0; i < 16; i++)
				air[air_n - 16 + i] = (uint8_t)(c >> (30 - 2 * i) & 3);
		}
	}
	for (uint16_t k = 0; k < M17_EOT_SYMS; k++)
		air[air_n++] = air_sym_get((const uint8_t[]){ 0x55, 0x5D }, (uint16_t)(k & 7));
}

// The air symbol whose center the time t (us after tap sample 0, tap phase `phase`) is, on a symbol clock air_ppm fast
// (as modulate's)
static double air_ppm;

static uint32_t air_index(const air_profile_t *p, uint32_t phase, double t_us)
{
	return (uint32_t)lround((t_us * FS / 1e6 + phase + LPF_HALF - LEAD) * (1 + air_ppm * 1e-6) / (FS / p->sym_rate));
}

// Symbols out against the air from the first sync on, wherever it was found: errors in the compared ones
static bool compare_m17(const air_profile_t *p, uint32_t phase, uint32_t *nerr, uint32_t *ncmp)
{
	uint32_t o, a;

	*nerr = *ncmp = 0;
	if (!out_nsync)
		return false;
	o = out_sync[0].at;
	for (a = air_index(p, phase, out_sync[0].t_us); o < out_n && a < air_n; o++, a++) {
		(*ncmp)++;
		*nerr += out_hard[o] != air[a];
	}
	return true;
}

// The syncs reported against the frames sent: how many frames got theirs at their first symbol with their kind (the
// first frame's may come as its START_ONLY pattern, 16 symbols ahead), and how many syncs came anywhere else
static void m17_syncs(const air_profile_t *p, uint32_t phase, uint16_t *right, uint16_t *wrong, uint16_t *typed)
{
	*right = *wrong = *typed = 0;
	for (uint8_t i = 0; i < out_nsync; i++) {
		uint32_t a = air_index(p, phase, out_sync[i].t_us);
		uint8_t id = out_sync[i].id;
		bool hit = false;

		for (uint16_t f = 0; f < m17_n && !hit; f++) {
			uint8_t pre = m17_kind[f] == M17_BERT ? M17_PRE_BERT : M17_PRE_LSF;

			if (a == m17_at[f] || (f == 0 && a == m17_at[f] - 16 && id == pre)) {
				hit = true;
				if (id == m17_kind[f] || (f == 0 && id == pre))
					(*right)++;
				else
					(*typed)++;
			}
		}
		if (!hit)
			(*wrong)++;
	}
}

static void m17_run(const air_profile_t *p, uint32_t rate, uint32_t phase, double hi, double cpk, double off, double sigma,
		    uint16_t told)
{
	static air_softmodem_t m;

	sample_tap(rate, phase, hi, cpk, off, sigma);
	CHECK(air_softmodem_rx_open(&m, p, rate, told, on_chunk, NULL), "open M17");
	out_reset();
	feed(&m, rate, 0, tap_n, 256);
}

// Random payload re-times a modem that takes any sync: the grid rule keeps M17 whole. Each kind of transmission, taps
// of the copies' kind (8 kHz, unknown scale, a band edge near 3 kHz) and others
static void testM17(void)
{
	static const struct { uint32_t rate, phase; double hi, cpk, off, sigma, ppm; uint16_t told; uint8_t first, then; const char *what; } t[] = {
		{ 8000, 3, 3600, 1000, 0, 0, 0, 0, M17_LSF, M17_STREAM, "8 kHz, unknown scale" },
		{ 8000, 11, 3600, 1000, 0, 0, 0, 1000, M17_LSF, M17_STREAM, "known scale" },
		{ 8000, 17, 3000, 1000, 300, 0, 0, 0, M17_LSF, M17_STREAM, "3.0 kHz edge, offset" },
		{ 8000, 23, 3400, 1300, -200, 0, 0, 0, M17_LSF, M17_PACKET, "3.4 kHz edge, hot, packet" },
		{ 8000, 7, 3600, 250, 0, 0, 0, 0, M17_BERT, M17_BERT, "a quarter of the level, BERT" },
		{ 8000, 29, 3600, 1000, 0, 60, 0, 1000, M17_LSF, M17_STREAM, "a little noise" },
		{ 16000, 5, 3600, 1000, 0, 0, 0, 1000, M17_LSF, M17_STREAM, "16 kHz" },
		{ 48000, 2, 3600, 1000, 200, 0, 0, 0, M17_LSF, M17_STREAM, "48 kHz" },
		// Symbol clocks apart: the grid is counted in the receiver's symbols, which drift against the sender's
		{ 8000, 13, 3600, 1000, 0, 0, 300, 0, M17_LSF, M17_STREAM, "clock 300 ppm fast" },
		{ 8000, 19, 3600, 1000, 0, 0, -300, 1000, M17_LSF, M17_PACKET, "clock 300 ppm slow, packet" },
	};
	uint32_t err[2] = { 0, 0 }, cmp[2] = { 0, 0 };

	rng_state = 0x5EED0017u;
	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		m17_air(t[i].first, t[i].then, M17_FRAMES, false);
		air_ppm = t[i].ppm;
		for (uint8_t grid = 0; grid < 2; grid++) {
			air_profile_t p = m17_profile(grid);
			uint32_t nerr, ncmp;
			uint16_t right, wrong, typed;

			modulate(&p, t[i].ppm);
			m17_run(&p, t[i].rate, t[i].phase, t[i].hi, t[i].cpk, t[i].off, t[i].sigma, t[i].told);
			CHECK(compare_m17(&p, t[i].phase, &nerr, &ncmp), "M17 %s grid %u: no sync", t[i].what, grid);
			m17_syncs(&p, t[i].phase, &right, &wrong, &typed);
			err[grid] += nerr;
			cmp[grid] += ncmp;
			printf("  M17 %s, %s: %u symbol errors in %u, syncs %u right %u elsewhere %u mis-typed\n", t[i].what,
			       grid ? "grid" : "no grid", nerr, ncmp, right, wrong, typed);
			CHECK(ncmp > (M17_FRAMES - 1) * M17_FRAME, "M17 %s grid %u: %u compared", t[i].what, grid, ncmp);
			if (grid)
				CHECK(right == M17_FRAMES && wrong == 0 && typed == 0, "M17 %s: syncs %u right %u elsewhere %u mis-typed",
				      t[i].what, right, wrong, typed);
		}
	}
	air_ppm = 0;
	printf("  M17: symbol errors %.4f %% with the grid rule, %.1f %% without\n", 100.0 * err[1] / cmp[1], 100.0 * err[0] / cmp[0]);
	CHECK(err[1] * 10000ull <= cmp[1], "M17 grid: %u errors in %u", err[1], cmp[1]);
	CHECK(err[0] * 10ull >= cmp[0], "M17 without the grid rule: %u errors in %u (the failure the rule fixes)", err[0], cmp[0]);
}

// The gates only save work on M17's patterns too (all outer, 8 symbols, where the sign gate does most of it): the same
// output with them and with every sample fitted, with the grid rule and without, through scale and offset errors,
// taps of unknown scale (after noise, where the tracked level starts low), noise, and noise alone
static void testGateM17(void)
{
	static int16_t all[WAVE_MAX / 30 + 8000];
	static uint8_t ref_hard[OUT_MAX];
	static int8_t ref_soft[OUT_MAX];
	static uint32_t ref_at[OUT_SYNCS];
	static double ref_t[OUT_SYNCS];
	// lead: samples of discriminator noise (no carrier, 2.5 x the signal's scale) before the call
	static const struct { double cpk, off, sigma; uint16_t told, lead; } t[] = {
		{ 1000, 0, 0, 1000, 0 }, { 1300, 500, 0, 1000, 0 }, { 700, -500, 0, 1000, 0 }, { 1000, 1500, 0, 1000, 0 },
		{ 1000, 0, 0, 0, 0 }, { 350, 300, 100, 0, 0 }, { 3000, -800, 300, 0, 0 }, { 1000, 0, 0, 0, 4000 },
		{ 500, 700, 150, 0, 6000 }, { 1000, 0, 400, 1000, 0 }, { 1000, 1200, 800, 0, 2000 }, { 1000, 0, 1500, 0, 0 },
		{ 1000, 0, -1, 0, 0 },	// noise alone
	};
	uint32_t runs = 0, syncs = 0;

	rng_state = 0x5EED0018u;
	m17_air(M17_LSF, M17_STREAM, 40, false);
	for (uint8_t grid = 0; grid < 2; grid++) {
		air_profile_t p = m17_profile(grid), ng = p;
		air_softmodem_t m;

		modulate(&p, 0);
		for (uint8_t k = 0; k < ng.nsyncs; k++)
			ng.sync[k].gate_up = ng.sync[k].gate_mask = ng.sync[k].sign_val = ng.sync[k].sign_mask = 0;
		for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			uint32_t n = 0, ref_n, diff = 0;
			uint8_t ref_nsync;

			if (t[i].sigma >= 0) {
				sample_tap(8000, 3 + 5 * i, 3600, t[i].cpk, t[i].off, t[i].sigma);
				for (uint32_t j = 0; j < t[i].lead; j++)
					all[n++] = (int16_t)lround(t[i].off + 2.5 * t[i].cpk * gauss());
				memcpy(all + n, tap, tap_n * sizeof(tap[0]));
				n += tap_n;
			} else {
				for (; n < 16000; n++)
					all[n] = (int16_t)lround(3000 * gauss());
			}
			for (uint8_t gated = 0; gated < 2; gated++) {
				CHECK(air_softmodem_rx_open(&m, gated ? &p : &ng, 8000, t[i].told, on_chunk, NULL), "open");
				out_reset();
				for (uint32_t from = 0; from < n; from += 256)
					air_softmodem_rx(&m, all + from, (uint16_t)(n - from < 256 ? n - from : 256), T0 + from * 125u);
				if (gated)
					break;
				ref_n = out_n;
				ref_nsync = out_nsync;
				memcpy(ref_hard, out_hard, out_n);
				memcpy(ref_soft, out_soft, out_n);
				for (uint8_t k = 0; k < out_nsync; k++) {
					ref_at[k] = out_sync[k].at;
					ref_t[k] = out_sync[k].t_us;
				}
			}
			if (t[i].sigma < 0) {
				// Noise alone is where the sign gate is no proof (air_softmodem.c): a chance fit at a tracked level
				// far below the nominal, its offset past 1.5 of it, is refused. It only ever refuses
				CHECK(out_nsync <= ref_nsync, "M17 grid %u, noise: %u syncs with the gates, %u without", grid, out_nsync, ref_nsync);
				printf("  gate, M17, noise alone, grid %u: %u chance syncs with the gates, %u without\n", grid, out_nsync, ref_nsync);
				continue;
			}
			CHECK(out_n == ref_n && out_nsync == ref_nsync, "M17 grid %u case %u: %u symbols, %u syncs with the gates, %u, %u without",
			      grid, i, out_n, out_nsync, ref_n, ref_nsync);
			for (uint32_t k = 0; k < out_n && k < ref_n; k++)
				diff += out_hard[k] != ref_hard[k] || out_soft[k] != ref_soft[k];
			for (uint8_t k = 0; k < out_nsync && k < ref_nsync; k++)
				diff += out_sync[k].at != ref_at[k] || out_sync[k].t_us != ref_t[k];
			CHECK(diff == 0, "M17 grid %u case %u: %u differences", grid, i, diff);
			CHECK(t[i].sigma < 0 || t[i].sigma > 500 || !grid || out_nsync >= 38, "M17 case %u: %u syncs", i, out_nsync);
			syncs += out_nsync;
			runs++;
		}
	}
	printf("  gate, M17: %u runs (%u syncs) the same with the gates and without\n", runs, syncs);
}

// The polarity trap: LSF is stream negated, packet BERT negated, so an inverted receive path turns every sync into
// another kind's. Symbols can't tell, the frame checks behind them must: through the soft modem and the framer, with
// a head hook standing in for the mode's (the frame's kind and payload CRC), the right way round every frame comes out
// as its kind, inverted none comes out at all. And no START_ONLY pattern is taken from an inverted preamble
static struct { uint16_t n, typed; uint8_t kind[M17_FRAMES + 4]; } m17_got;

static uint8_t m17_kind_of(uint8_t id)
{
	return id == M17_PRE_LSF ? M17_LSF : id == M17_PRE_BERT ? M17_BERT : id;
}

static uint16_t m17_head(const air_frame_t *f, void *ctx)
{
	uint8_t d[M17_FRAME];
	uint32_t c = 0;

	(void)ctx;
	if (f->nsyms < M17_FRAME)
		return AIR_HEAD_AGAIN | M17_FRAME;	// (the real one says EoT at 24 first)
	for (uint16_t i = 0; i < M17_FRAME; i++)
		d[i] = air_sym_get(f->hard, i);
	for (uint8_t i = 0; i < 16; i++)
		c = c << 2 | d[M17_FRAME - 16 + i];
	return c == m17_check(&d[8], m17_kind_of(f->sync_id)) ? M17_FRAME : 0;
}

static void m17_frame(const air_frame_t *f, void *ctx)
{
	uint16_t at = m17_got.n;

	(void)ctx;
	if (at < M17_FRAMES + 4)
		m17_got.kind[at] = m17_kind_of(f->sync_id);
	if (at < m17_n && m17_got.kind[at] != m17_kind[at])
		m17_got.typed++;
	m17_got.n++;
}

static void m17_lost(void *ctx) { (void)ctx; }

static void testInvertedM17(void)
{
	static uint8_t fbuf[M17_FRAME / 4];
	static int8_t fsoft[M17_FRAME];
	static const struct { uint8_t first, then; } k[] = {
		{ M17_LSF, M17_STREAM }, { M17_LSF, M17_PACKET }, { M17_BERT, M17_BERT },
	};
	air_mode_t mode = { .name = "M17 (test)", .frame_syms = M17_FRAME, .head_syms = 24, .lost_frames = 3,
			    .head = m17_head, .rx_frame = m17_frame, .rx_lost = m17_lost };
	air_softmodem_t m;
	air_framer_t fr;

	mode.profile = m17_profile(true);
	rng_state = 0x5EED0019u;
	for (uint8_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		m17_air(k[i].first, k[i].then, 30, true);
		modulate(&mode.profile, 0);
		for (uint8_t inv = 0; inv < 2; inv++) {
			uint8_t start_only = 0;

			sample_tap(8000, 7 + 4 * i, 3600, inv ? -1000 : 1000, 0, 0);
			memset(&m17_got, 0, sizeof(m17_got));
			air_framer_init(&fr, &mode, NULL, fbuf, fsoft, M17_FRAME);
			CHECK(air_softmodem_rx_open(&m, &mode.profile, 8000, 0, on_chunk, NULL), "open");
			out_reset();
			out_framer = &fr;
			feed(&m, 8000, 0, tap_n, 256);
			out_framer = NULL;
			for (uint8_t s = 0; s < out_nsync; s++)
				start_only += out_sync[s].id == M17_PRE_LSF || out_sync[s].id == M17_PRE_BERT;
			printf("  M17 %s%s: modem syncs %u (START_ONLY %u), frames %u, of another kind %u\n", inv ? "inverted, " : "",
			       k[i].then == M17_STREAM ? "stream" : k[i].then == M17_PACKET ? "packet" : "BERT", out_nsync, start_only,
			       m17_got.n, m17_got.typed);
			if (inv) {
				CHECK(m17_got.n == 0, "inverted %u: %u frames came out (%u of another kind)", i, m17_got.n, m17_got.typed);
				CHECK(start_only == 0, "inverted %u: %u START_ONLY syncs", i, start_only);
			} else {
				CHECK(m17_got.n == m17_n && m17_got.typed == 0, "%u: %u frames of %u, %u of another kind", i, m17_got.n, m17_n,
				      m17_got.typed);
			}
		}
	}
}

// The grid rule's window (air_softmodem.c GRID_TOL, 2 symbols): while locked, a sync a symbol off the grid still
// re-times the modem, one 4 symbols off is not taken, nor the next on its new grid, until the lock ends (two spacings
// and two sync lengths after the last one taken) and a later frame's sync locks again. Frames 6 on are moved by `shift`
// symbols (a frame 5 of 192 + shift)
static bool m17_synced(const air_profile_t *p, uint32_t phase, uint16_t f)
{
	for (uint8_t i = 0; i < out_nsync; i++)
		if (air_index(p, phase, out_sync[i].t_us) == m17_at[f] && out_sync[i].id == m17_kind[f])
			return true;
	return false;
}

static void testGridEdges(void)
{
	static const int8_t shift[] = { -1, 1, -4, 4 };
	const uint16_t frames = 14, moved = 6;

	rng_state = 0x5EED001Cu;
	for (uint8_t i = 0; i < sizeof(shift) / sizeof(shift[0]); i++) {
		int sh = shift[i];
		uint32_t at;

		m17_air(M17_LSF, M17_STREAM, frames, false);
		at = m17_at[moved];
		memmove(air + at + sh, air + at, air_n - at);
		for (int k = 0; k < sh; k++)
			air[at + (uint32_t)k] = (uint8_t)(rng() & 3);
		air_n = (uint32_t)((int)air_n + sh);
		for (uint16_t f = moved; f < m17_n; f++)
			m17_at[f] = (uint32_t)((int)m17_at[f] + sh);
		{
			air_profile_t p = m17_profile(true);
			uint16_t before = 0, after = 0;
			bool a, b;

			modulate(&p, 0);
			m17_run(&p, 8000, 5 + i, 3600, 1000, 0, 0, 0);
			for (uint16_t f = 1; f < moved; f++)
				before += m17_synced(&p, 5 + i, f);
			for (uint16_t f = moved + 2; f < frames; f++)
				after += m17_synced(&p, 5 + i, f);
			a = m17_synced(&p, 5 + i, moved);
			b = m17_synced(&p, 5 + i, moved + 1);
			printf("  grid edge, frame %u moved %+d: frames 1-5 %u synced, 6 %u, 7 %u, 8-13 %u\n", moved, sh, before, a, b,
			       after);
			CHECK(before == moved - 1, "shift %d: %u of frames 1-5 synced", sh, before);
			if (sh == 1 || sh == -1)
				CHECK(a && b, "shift %d: a sync inside the window not taken", sh);
			else
				CHECK(!a && !b, "shift %d: a sync off the grid was taken while locked", sh);
			CHECK(after == frames - moved - 2, "shift %d: %u of frames 8-13 synced (locked again)", sh, after);
		}
	}
}

// The gates only save work on NXDN's patterns with the sign gate on as well (the debug link's bench prepares all four
// words on every pattern): the 10-symbol FSW and the 19-symbol preamble pattern, at both rates, through scale and
// offset errors, known and unknown scale, and noise
static void testGateNxdn(void)
{
	static uint8_t ref_hard[OUT_MAX];
	static int8_t ref_soft[OUT_MAX];
	static uint32_t ref_at[OUT_SYNCS];
	static double ref_t[OUT_SYNCS];
	static const struct { double cpk, off, sigma; uint16_t told; } t[] = {
		{ 2000, 0, 0, 2000 }, { 2600, 600, 0, 2000 }, { 1400, -500, 150, 2000 }, { 2000, 0, 450, 2000 },
		{ 2000, 0, 0, 0 }, { 700, 300, 100, 0 }, { 5000, -800, 600, 0 }, { 2000, 1500, 300, 0 },
	};
	const air_profile_t *nx[2] = { &nxdn_air_mode.profile, &nxdn_air_mode_9600.profile };
	uint32_t runs = 0, syncs = 0;

	rng_state = 0x5EED001Bu;
	for (uint8_t pi = 0; pi < 2; pi++) {
		air_profile_t p = *nx[pi], ng;
		air_softmodem_t m;

		air_softmodem_profile_prep(&p);
		CHECK(p.sync[0].sign_mask != 0 && p.sync[1].sign_mask != 0, "NXDN %u: sign gates", pi);
		ng = p;
		for (uint8_t k = 0; k < ng.nsyncs; k++)
			ng.sync[k].gate_up = ng.sync[k].gate_mask = ng.sync[k].sign_val = ng.sync[k].sign_mask = 0;
		air_stream(&p, 0, 300 + 20 * NXDN_FRAME_SYMS + 100, 300, NXDN_FRAME_SYMS);
		for (uint8_t i = 0; i < p.sync[1].nsyms; i++)
			air[300 - p.sync[1].frame_at + i] = air_sym_get(p.sync[1].syms, i);
		modulate(&p, 0);
		for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			uint32_t ref_n = 0, diff = 0;
			uint8_t ref_nsync = 0;

			sample_tap(8000, 3 + 4 * i, 3600, t[i].cpk, t[i].off, t[i].sigma);
			for (uint8_t gated = 0; gated < 2; gated++) {
				CHECK(air_softmodem_rx_open(&m, gated ? &p : &ng, 8000, t[i].told, on_chunk, NULL), "open");
				out_reset();
				feed(&m, 8000, 0, tap_n, 256);
				if (gated)
					break;
				ref_n = out_n;
				ref_nsync = out_nsync;
				memcpy(ref_hard, out_hard, out_n);
				memcpy(ref_soft, out_soft, out_n);
				for (uint8_t k = 0; k < out_nsync; k++) {
					ref_at[k] = out_sync[k].at;
					ref_t[k] = out_sync[k].t_us;
				}
			}
			CHECK(out_n == ref_n && out_nsync == ref_nsync, "NXDN %u case %u: %u symbols, %u syncs with the gates, %u, %u without",
			      pi, i, out_n, out_nsync, ref_n, ref_nsync);
			for (uint32_t k = 0; k < out_n && k < ref_n; k++)
				diff += out_hard[k] != ref_hard[k] || out_soft[k] != ref_soft[k];
			for (uint8_t k = 0; k < out_nsync && k < ref_nsync; k++)
				diff += out_sync[k].at != ref_at[k] || out_sync[k].t_us != ref_t[k];
			CHECK(diff == 0, "NXDN %u case %u: %u differences", pi, i, diff);
			CHECK(t[i].sigma > 300 || out_nsync >= 15, "NXDN %u case %u: %u syncs", pi, i, out_nsync);
			syncs += out_nsync;
			runs++;
		}
	}
	printf("  gate, NXDN with sign gates: %u runs (%u syncs) the same with the gates and without\n", runs, syncs);
}

// Each mode's const profile writes its gate words as literals: they are what the prep computes from its patterns (the
// sign words may be 0: P25 and NXDN leave that gate off). And the prep itself, on one pattern worked by hand
static void testLiterals(void)
{
	static const struct { const air_profile_t *p; uint8_t verify; const char *name; } t[] = {
		{ &p25_air_mode.profile, AIR_VERIFY_P25, "P25" },
		{ &nxdn_air_mode.profile, AIR_VERIFY_NXDN, "NXDN 4800" },
		{ &nxdn_air_mode_9600.profile, AIR_VERIFY_NXDN, "NXDN 9600" },
	};
	air_sync_t s;
	air_profile_t q;

	for (uint8_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		const air_profile_t *p = t[i].p;

		CHECK(p->verify == t[i].verify, "%s: verify %x", t[i].name, p->verify);
		for (uint8_t k = 0; k < p->nsyncs; k++) {
			const air_sync_t *y = &p->sync[k];

			s = *y;
			air_softmodem_sync_prep(&s);
			CHECK(y->gate_up == s.gate_up && y->gate_mask == s.gate_mask, "%s sync %u: gate 0x%04X/0x%04X, the prep says 0x%04X/0x%04X",
			      t[i].name, k, y->gate_up, y->gate_mask, s.gate_up, s.gate_mask);
			CHECK((y->sign_val == 0 && y->sign_mask == 0) || (y->sign_val == s.sign_val && y->sign_mask == s.sign_mask),
			      "%s sync %u: sign 0x%04X/0x%04X, the prep says 0x%04X/0x%04X", t[i].name, k, y->sign_val, y->sign_mask,
			      s.sign_val, s.sign_mask);
			CHECK(s.gate_mask != 0, "%s sync %u has a step gate", t[i].name, k);
		}
	}
	CHECK(!(p25_air_mode.profile.flags & AIR_PROFILE_SYNC_GRID), "P25's units differ in length: no grid");
	// M17's stream sync, -3 -3 -3 -3 +3 +3 -3 +3 (bit 0 the last): signs + at 0, 2, 3; all outer; steps up at 0 and 3,
	// down at 1
	memset(&s, 0, sizeof(s));
	s.nsyms = 8;
	s.syms[0] = 0xFF;
	s.syms[1] = 0x5D;
	s.sign_val = 0xFFFF;
	air_softmodem_sync_prep(&s);
	CHECK(s.sign_val == 0x0D && s.sign_mask == 0xFF && s.gate_up == 0x09 && s.gate_mask == 0x0B, "stream sync: %04X %04X %04X %04X",
	      s.sign_val, s.sign_mask, s.gate_up, s.gate_mask);
	// Inner symbols are not in the sign gate; past 16 symbols back nothing is
	s.syms[1] = 0x0D;		// -3 -3 -3 -3 +1 +1 -3 +3
	air_softmodem_sync_prep(&s);
	CHECK(s.sign_mask == 0xF3 && s.sign_val == 0x0D, "inner symbols: %04X %04X", s.sign_mask, s.sign_val);
	s.nsyms = 24;
	memcpy(s.syms, (uint8_t[]){ 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 }, 6);
	air_softmodem_sync_prep(&s);
	CHECK(s.sign_val == 0xFFFF && s.sign_mask == 0xFFFF && s.gate_mask == 0, "one level: %04X %04X %04X", s.sign_val, s.sign_mask, s.gate_mask);
	s.nsyms = 200;			// (a bad length reads no further than the pattern's bytes)
	air_softmodem_sync_prep(&s);
	CHECK(s.sign_mask == 0xFFFF, "long: %04X", s.sign_mask);
	// The profile-wide prep: every pattern, none past nsyncs
	q = m17_profile(true);
	q.sync[6] = q.sync[2];
	q.sync[6].gate_mask = 0x1234;
	for (uint8_t k = 0; k < q.nsyncs; k++)
		q.sync[k].gate_up = q.sync[k].gate_mask = q.sync[k].sign_val = q.sync[k].sign_mask = 0;
	air_softmodem_profile_prep(&q);
	CHECK(q.sync[2].sign_val == 0x0D && q.sync[2].gate_mask == 0x0B && q.sync[0].sign_mask == 0xFFFF && q.sync[6].gate_mask == 0x1234,
	      "profile prep");
}

// What the receive costs the host, M17 against P25 on the same tap (8 kHz, unknown scale): a signal of each mode's
// frames, and noise. The two modems take the input in turns, 256 samples each, each turn timed: whatever core and
// load a stretch of the run meets, both meet it. Best-of runs timed one modem after the other were not enough: on a
// loaded machine with cores of two speeds the ratio came out past 1.3 one time in a few, and below 0.9 too. The median
// of COST_REPS such runs is the ratio
#define COST_REPS	9
static double cpu_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_nothing(const air_chunk_t *c, void *ctx)
{
	(void)c;
	(void)ctx;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static void testCost(void)
{
	static int16_t sig[3][WAVE_MAX / 30];
	static uint32_t sig_n[3];
	static air_softmodem_t m[2];
	air_profile_t pm = m17_profile(true);
	const air_profile_t *p25 = &p25_air_mode.profile, *prof[2] = { p25, &pm };

	rng_state = 0x5EED001Au;
	m17_air(M17_LSF, M17_STREAM, 60, false);
	modulate(&pm, 0);
	sample_tap(8000, 5, 3600, 1000, 0, 0);
	memcpy(sig[1], tap, tap_n * sizeof(tap[0]));
	sig_n[1] = tap_n;
	air_stream(p25, 0, air_n, 100, 864);
	modulate(p25, 0);
	sample_tap(8000, 5, 3600, 2000, 0, 0);
	memcpy(sig[0], tap, tap_n * sizeof(tap[0]));
	sig_n[0] = tap_n;
	for (sig_n[2] = 0; sig_n[2] < sig_n[0]; sig_n[2]++)
		sig[2][sig_n[2]] = (int16_t)lround(3000 * gauss());
	for (uint8_t noise = 0; noise < 2; noise++) {
		double ratio[COST_REPS], best[2] = { 1e9, 1e9 }, secs;
		uint32_t n = sig_n[noise ? 2 : 0] < sig_n[noise ? 2 : 1] ? sig_n[noise ? 2 : 0] : sig_n[noise ? 2 : 1];

		secs = n / 8000.0;
		for (uint8_t rep = 0; rep < COST_REPS; rep++) {
			double t[2] = { 0, 0 };

			for (uint8_t k = 0; k < 2; k++)
				air_softmodem_rx_open(&m[k], prof[k], 8000, 0, on_nothing, NULL);
			for (uint32_t from = 0; from < n; from += 256) {
				uint16_t len = (uint16_t)(n - from < 256 ? n - from : 256);

				for (uint8_t k = 0; k < 2; k++) {
					double t0 = cpu_s();

					air_softmodem_rx(&m[k], sig[noise ? 2 : k] + from, len, T0 + from * 125u);
					t[k] += cpu_s() - t0;
				}
			}
			ratio[rep] = t[1] / t[0];
			for (uint8_t k = 0; k < 2; k++)
				if (t[k] < best[k])
					best[k] = t[k];
		}
		qsort(ratio, COST_REPS, sizeof(ratio[0]), cmp_double);
		printf("  cost, %s: P25 %.2f ms, M17 %.2f ms of host CPU a second of it (%.2f x, runs %.2f to %.2f)\n",
		       noise ? "noise" : "signal", 1e3 * best[0] / secs, 1e3 * best[1] / secs, ratio[COST_REPS / 2], ratio[0],
		       ratio[COST_REPS - 1]);
		CHECK(ratio[COST_REPS / 2] <= 1.3, "%s: M17 costs %.2f x P25", noise ? "noise" : "signal", ratio[COST_REPS / 2]);
	}
}

#if AIR_SOFTMODEM_TEST_M17
// ── m17_air_mode's profile (core/m17/m17_rx.c) writes its gate words as literals: they are what the prep computes,
// the sign words on for every pattern; and its patterns are the ones the M17 cases above model ──
#undef M17_EOT_SYMS		// (this file's, the same 192 as m17.h's)
#include "m17/m17_engine.h"

static void testLiteralsM17(void)
{
	const air_profile_t *p = &m17_air_mode.profile;
	const air_profile_t q = m17_profile(true);
	air_sync_t s;

	CHECK(p->sym_rate == 4800 && p->levels == 4 && p->shape == AIR_SHAPE_RRC && p->shape_param == 50 && p->dev_outer_hz == 2400 &&
	      p->timing == AIR_TIMING_CONTINUOUS && p->sync_every_syms == 0 && p->sync_any_syms == M17_FRAME && p->idle == 0x77 &&
	      p->flags == AIR_PROFILE_SYNC_GRID && p->verify == AIR_VERIFY_M17 && p->nsyncs == 6, "M17: the profile");
	CHECK(m17_air_mode.frame_syms == M17_FRAME && m17_air_mode.head_syms == 24 && m17_air_mode.head && m17_air_mode.rx_frame &&
	      m17_air_mode.rx_lost, "M17: the mode");
	for (uint8_t k = 0; k < p->nsyncs; k++) {
		const air_sync_t *y = &p->sync[k];

		s = *y;
		air_softmodem_sync_prep(&s);
		CHECK(y->gate_up == s.gate_up && y->gate_mask == s.gate_mask && y->sign_val == s.sign_val && y->sign_mask == s.sign_mask,
		      "M17 sync %u: 0x%04X/0x%04X 0x%04X/0x%04X, the prep says 0x%04X/0x%04X 0x%04X/0x%04X", k, y->gate_up, y->gate_mask,
		      y->sign_val, y->sign_mask, s.gate_up, s.gate_mask, s.sign_val, s.sign_mask);
		CHECK(s.gate_mask != 0 && s.sign_mask != 0, "M17 sync %u: both gates on", k);
		CHECK(y->nsyms == q.sync[k].nsyms && y->max_errs == q.sync[k].max_errs && y->flags == q.sync[k].flags &&
		      y->frame_at == q.sync[k].frame_at && !memcmp(y->syms, q.sync[k].syms, sizeof y->syms),
		      "M17 sync %u: the pattern the cases above model", k);
	}
	printf("  M17: m17_air_mode's %u patterns, gate literals the prep's\n", p->nsyncs);
}
#endif

int main(int argc, char **argv)
{
	bool print = argc > 1 && !strcmp(argv[1], "--goldens");

	testFit();
	log_on = true;
	testClean();
	testLevels();
	testNoise();
	testGate();
	testShapes();
	testCall();
	testHole();
	testGoldenNxdn();
	testGoldens(print);
	if (print)
		return 0;
	testLiterals();
#if AIR_SOFTMODEM_TEST_M17
	testLiteralsM17();
#endif
	testM17();
	testGateM17();
	testGateNxdn();
	testGridEdges();
	testInvertedM17();
	testCost();
	printf("airSoftmodemTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
