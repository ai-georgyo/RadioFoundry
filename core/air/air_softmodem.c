// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (air_softmodem.h, docs/spec/air-interface.md 6).
//
// Input samples -> minus the tap's offset -> polyphase interpolator to five samples per symbol -> integrate and
// dump -> the line (a ring of the last 28 symbols of filtered samples). On every filtered sample the newest symbol
// spaced samples in the line are fitted to each sync pattern (sample = offset + level x pattern); where the fit's
// error passes through a minimum, that sample is a symbol center, and the fit's level and offset are the signal's.
// Symbols leave from the far end of the line, so a sync is sampled on the timing it gave.
//
// The cost is per filtered sample (24,000 a second at 4800 symbols), so that path is kept short (../dsp/fxp.h has
// the rules and the kernels): a pattern is fitted only where its gates of one bit per symbol say it can be there, the
// input history is laid out twice so the interpolator reads one run, and nothing in it divides or needs 64 bits.
#include "air_softmodem.h"

#include <string.h>

#include "../dsp/fxp.h"
#include "air_syms.h"

#define SPS		AIR_SOFTMODEM_SPS
#define LINE		AIR_SOFTMODEM_LINE
#define TAPS		AIR_SOFTMODEM_TAPS
#define PHASES		12			// interpolator phases per input sample
#define LINE_LVL	16			// line units per symbol level at nominal: +3 is 48, headroom to 127
#define EMIT_AT		(LINE - 2 * SPS)	// a symbol leaves when its center is this far back
#define Q_NONE		0xFFFFu
// A fit's error: 256 x the mean square distance of the samples from the fitted line, in symbol levels. 92 is 0.6
// of a level RMS, where slicing already fails often: a sync worse than that gives no timing worth taking
#define Q_MAX		92
#define LVL_NOM		(LINE_LVL * 256)
#define LVL_MAX		(LVL_NOM * 3 / 2)
#define LVL_MIN_KNOWN	(LVL_NOM * 7 / 12)	// as the framer's limits
#define LVL_MIN_TRACKED	(LVL_NOM / 4)		// the tracked level sits low after noise: the fit corrects it
#define OFF_MAX		(LINE_LVL * 3 / 2 * 256)	// half the outer level
#define TRACK_SHIFT_MIN	4
#define TRACK_SHIFT_MAX	10
#define HOLE_US		2000			// input time off by more: samples were lost
#define GAIN_MAX	(1u << 20)		// 16 line units per summed count
// The gates (gate(), below), per pattern, from its air_sync_t words (air_softmodem_sync_prep). Each spares only fits
// that would fail:
//  - Steps: the steps of four levels or more between its neighboring symbols, as bits. One missing (a step the wrong
//    way, or none) costs 8 levels squared at least, two 10.7 (they may share a symbol), whatever the level and
//    offset (the samples are compared with each other). Over Q_MAX: 256 x 8 / n for one, so a pattern of up to
//    STEP_TOL_SYMS symbols is fitted with none missing (256 x 8 / 16 = 128); 256 x 10.7 / 24 = 113 for two, so a
//    longer one with one missing at most. (Of its 16 newest steps; random samples pass a 24-symbol pattern with 8
//    large steps one time in 28, one with 11 one time in 170.)
//  - Signs: the signs of its outer symbols among its 16 newest. Each wrong one is an outer symbol on the wrong side
//    of zero, which costs (3 - |offset| / level)^2 levels squared at least: 2.25 while the fitted offset is within
//    1.5 levels (OFF_MAX at the nominal level). A pattern passes with SIGN_TOL of them wrong at most, the most that
//    can still be under Q_MAX: one more costs 256 x 2.25 x (SIGN_TOL + 1) / n, over Q_MAX (1 for 8 to 12 symbols, 2
//    for 13 to 18, 3 for 19 to 24; random samples pass an 8-symbol pattern one time in 28). Past that offset (in
//    noise, a tracked level far below the nominal with an offset left over) it can refuse a chance fit that would
//    have passed; the tests show the same output with the gates and without on signals with scale and offset errors
//    and on taps of unknown scale.
#define STEP_TOL_SYMS	16u
#define SIGN_TOL(nsyms)	((unsigned)(nsyms) * Q_MAX / 576u)	// 576 = 256 x 2.25
#define SIGN_TOL1_SYMS	12u				// the longest pattern SIGN_TOL allows one
_Static_assert(SIGN_TOL(SIGN_TOL1_SYMS) == 1 && SIGN_TOL(SIGN_TOL1_SYMS + 1) == 2, "SIGN_TOL1_SYMS");
_Static_assert(SIGN_TOL(8) == 1 && SIGN_TOL(AIR_SYNC_SYMS_MAX) == 3, "SIGN_TOL");
// The grid rule (AIR_PROFILE_SYNC_GRID, allowed()): how far from a whole number of the profile's sync spacing a sync
// may fall, in symbols, while a lock holds. The count it is read from moves by one symbol at most at each re-timing,
// and 2 is 1 % of symbol clock error over a 192-symbol frame (two crystals are 100 times closer)
#define GRID_TOL	2u

// The interpolator: a windowed sinc low-pass at 0.45 of the input rate, 12 phases x 8 taps, each phase summing to
// 16384. Row p, tap t weighs the input sample t back from the newest; the output sits (47.5 - p) / 12 samples back
static const int16_t interp[PHASES][TAPS] = {
	{      0,    105,   -630,   1791,  14707,    735,   -394,     70 },
	{     -1,    143,   -884,   3021,  14382,   -130,   -189,     42 },
	{     -2,    182,  -1141,   4393,  13753,   -797,    -24,     20 },
	{     -4,    216,  -1376,   5867,  12843,  -1268,    101,      5 },
	{     -7,    240,  -1565,   7393,  11699,  -1557,    185,     -4 },
	{     -8,    248,  -1678,   8914,  10369,  -1685,    232,     -8 },
	{     -8,    232,  -1685,  10369,   8914,  -1678,    248,     -8 },
	{     -4,    185,  -1557,  11699,   7393,  -1565,    240,     -7 },
	{      5,    101,  -1268,  12843,   5867,  -1376,    216,     -4 },
	{     20,    -24,   -797,  13753,   4393,  -1141,    182,     -2 },
	{     42,   -189,   -130,  14382,   3021,   -884,    143,     -1 },
	{     70,   -394,    735,  14707,   1791,   -630,    105,      0 },
};

static uint32_t gcd32(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

// The longest run between two syncs the profile sends, any length (0: none recurs)
static uint16_t sync_spacing(const air_profile_t *p)
{
	return p->sync_any_syms ? p->sync_any_syms : p->sync_every_syms;
}

void air_softmodem_sync_prep(air_sync_t *s)
{
	unsigned n = s->nsyms > AIR_SYNC_SYMS_MAX ? AIR_SYNC_SYMS_MAX : s->nsyms;

	s->gate_up = s->gate_mask = s->sign_val = s->sign_mask = 0;
	// Bit j: the symbol j back from the pattern's last (its step: from the symbol j + 1 back to it)
	for (unsigned j = 0; j < n && j < 16; j++) {
		int e = air_sym_level(air_sym_get(s->syms, (uint16_t)(n - 1 - j)));

		if (e > 0)
			s->sign_val |= (uint16_t)(1u << j);
		if (e == 3 || e == -3)
			s->sign_mask |= (uint16_t)(1u << j);
		if (j + 1 < n) {
			int d = e - air_sym_level(air_sym_get(s->syms, (uint16_t)(n - 2 - j)));

			if (d >= 4)
				s->gate_up |= (uint16_t)(1u << j);
			if (d >= 4 || d <= -4)
				s->gate_mask |= (uint16_t)(1u << j);
		}
	}
}

void air_softmodem_profile_prep(air_profile_t *p)
{
	for (uint8_t i = 0; i < p->nsyncs && i < AIR_SYNCS_MAX; i++)
		air_softmodem_sync_prep(&p->sync[i]);
}

static bool rate_ok(const air_profile_t *p, uint32_t rate_hz, uint16_t *step, uint16_t *den)
{
	uint32_t out = (uint32_t)p->sym_rate * SPS, g;

	// Below 8 kHz a 4800 symbol/s signal does not fit; above twice the filtered rate the interpolator (which does
	// not filter for the lower rate) would fold noise in
	if (!p->sym_rate || rate_hz < 8000 || rate_hz > 2 * out || rate_hz < (uint32_t)p->sym_rate * 5 / 3)
		return false;
	g = gcd32(rate_hz, out);
	if (rate_hz / g + out / g > 0xFFFF)
		return false;
	*step = (uint16_t)(rate_hz / g);
	*den = (uint16_t)(out / g);
	return true;
}

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	uint16_t step, den;

	if (!bb || !bb->rx_start || !bb->rx_read || bb->rx_channels != 1)
		return 0;		// a split band (two channels) is not written
	if (p->levels != 2 && p->levels != 4)
		return 0;
	if (!rate_ok(p, bb->rx_rate_hz, &step, &den))
		return 0;
	// The band: up to a little past half the symbol rate, and down to where a run of one level still holds (four
	// levels have no room for a sagging baseline)
	if (bb->rx_hi_hz < (uint32_t)p->sym_rate * 11 / 20)
		return 0;
	if (bb->rx_lo_hz > p->sym_rate / (p->levels == 4 ? 100 : 20))
		return 0;
	return AIR_FIT_RX | AIR_FIT_SOFT;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	memset(m->in, 0, sizeof m->in);
	memset(m->sum, 0, sizeof m->sum);
	m->acc = 0;
	m->in_at = m->sum_at = 0;
	m->pos = 0;
	m->ph = 0;
	m->ph_r = 0;
	m->line_at = m->line_n = 0;
	memset(m->steps, 0, sizeof m->steps);
	memset(m->signs, 0, sizeof m->signs);
	m->phase = 0;
	m->track_shift = TRACK_SHIFT_MIN;
	m->track_n = 1u << TRACK_SHIFT_MIN;
	m->locked = false;
	m->running = false;
	m->cand_q = m->cand_prev_q = Q_NONE;
	m->hold = 0;
	m->due = EMIT_AT - 1;
	m->sub = 0;
	m->scale = (uint16_t)((AIR_SOFT_OUTER / 3) * 256 / LINE_LVL);
	m->off = 0;
	m->since_sync = 0xFFFF;
	m->sync_wait = 0;
	m->gap = true;
	m->n = 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	uint16_t step, den;

	if ((p->levels != 2 && p->levels != 4) || !rate_ok(p, rate_hz, &step, &den))
		return false;
	memset(m, 0, sizeof *m);
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	m->rate_hz = rate_hz;
	m->step = step;
	m->den = den;
	// The integrate and dump over a symbol is the receive filter the C4FM pulse is made for, and close to a matched
	// one for a 2-level square pulse. A root raised cosine or Gaussian pulse gets a shorter one: it only smooths
	m->box = (p->shape == AIR_SHAPE_C4FM || p->shape == AIR_SHAPE_NRZ) ? SPS : 3;
	m->gain_known = counts_per_khz != 0;
	if (m->gain_known) {
		// The outer symbol (dev_outer_hz) to 3 x LINE_LVL, after the sum over box samples
		uint64_t outer = (uint64_t)p->dev_outer_hz * counts_per_khz;	// counts x 1000

		if (!outer)
			return false;
		outer = ((uint64_t)(3 * LINE_LVL) * 65536u * 1000u) / (outer * m->box);
		m->gain = outer > GAIN_MAX ? GAIN_MAX : (uint32_t)outer;
	} else {
		m->gain = 65536u / m->box;
		m->env = m->env_fast = 3 * LINE_LVL * 1024;
	}
	for (uint8_t i = 0; i < p->nsyncs && i < AIR_SYNCS_MAX; i++) {
		const air_sync_t *s = &p->sync[i];
		int16_t s1 = 0, s2 = 0;

		if (s->nsyms < 8 || s->nsyms > AIR_SYNC_SYMS_MAX)
			continue;
		for (uint8_t k = 0; k < s->nsyms; k++) {
			int8_t e = air_sym_level(air_sym_get(s->syms, k));

			s1 = (int16_t)(s1 + e);
			s2 = (int16_t)(s2 + e * e);
		}
		if (s->nsyms * s2 - s1 * s1 <= 0)
			continue;	// one level only: nothing to fit
		m->valid |= (uint8_t)(1u << i);
		if (s->flags & AIR_SYNC_START_ONLY)
			m->start_only |= (uint8_t)(1u << i);
	}
	air_softmodem_rx_reset(m);
	return true;
}

static int line_back(const air_softmodem_t *m, unsigned back)
{
	int at = (int)m->line_at - 1 - (int)back;

	return m->line[at < 0 ? at + LINE : at];
}

static void flush(air_softmodem_t *m)
{
	uint8_t hard[AIR_SOFTMODEM_CHUNK / 4];
	air_chunk_t c;

	if (!m->n)
		return;
	memset(hard, 0, sizeof hard);
	for (unsigned i = 0; i < m->n; i++) {
		int v = m->soft[i];

		air_sym_put(hard, (uint16_t)i, m->p->levels == 2 ? (v >= 0 ? 1 : 3)
			    : v >= 2 * AIR_SOFT_OUTER / 3 ? 1 : v >= 0 ? 0 : v >= -2 * AIR_SOFT_OUTER / 3 ? 2 : 3);
	}
	c.t_us = m->t_chunk_us;
	c.nsyms = m->n;
	c.sync_at = m->sync_at;
	c.sync_id = m->sync_id;
	c.flags = m->flags;
	c.rssi_dbm = 0;
	c.hard = hard;
	c.soft = m->soft;
	m->n = 0;
	m->out(&c, m->ctx);
}

// The symbol whose center is due samples back leaves the line. t_us: the time of the input sample in hand, less the
// filters' delay
static void emit(air_softmodem_t *m, uint32_t t_us)
{
	int32_t x = line_back(m, m->due) * 16, v;
	bool sync;

	// Between two samples: straight line to the neighbor on the side the fit's minimum leaned to
	if (m->sub > 0)
		x += m->sub * (line_back(m, m->due - 1u) - line_back(m, m->due));
	else if (m->sub < 0)
		x -= m->sub * (line_back(m, m->due + 1u) - line_back(m, m->due));
	v = ((x - m->off / 16) * (int32_t)m->scale) / 4096;
	if (v > 127)
		v = 127;
	if (v < -127)
		v = -127;
	sync = m->sync_wait && --m->sync_wait == 0;
	if (sync && m->n && m->sync_at >= 0)
		flush(m);		// a chunk names one sync
	if (!m->n) {
		uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;

		// The line's newest sample sits pos / den of an input sample after t_us (an input sample in 1/16 us)
		m->t_chunk_us = t_us + (uint32_t)m->pos * (16000000u / m->rate_hz) / ((uint32_t)m->den * 16u)
				- (uint32_t)(((uint32_t)m->due * 16 - m->sub) * 62500u / out_rate);
		m->flags = m->gap ? AIR_CHUNK_GAP : 0;
		// Past the profile's sync spacing with none seen (or none ever): the timing is the last one's, or none.
		// (since_sync starts when a sync is found, two sync lengths before its symbols are all out)
		if (m->since_sync > (uint32_t)sync_spacing(m->p) + 2 * AIR_SYNC_SYMS_MAX)
			m->flags |= AIR_CHUNK_FREE_RUN;
		m->gap = false;
		m->sync_at = -1;
	}
	if (sync) {
		m->sync_at = m->n;
		m->sync_id = m->sync_wait_id;
	}
	m->soft[m->n] = (int8_t)v;
	m->n++;
	if (m->since_sync != 0xFFFF)
		m->since_sync++;
	if (m->n == AIR_SOFTMODEM_CHUNK)
		flush(m);
}

// A sync's last symbol center is `back` samples back: take its timing, level and offset
static void commit(air_softmodem_t *m, uint8_t back, uint16_t q_next)
{
	const air_sync_t *s = &m->p->sync[m->cand_sync];
	int32_t lvl = m->cand_lvl, off = m->cand_off;
	uint8_t first = (uint8_t)(back + (s->nsyms - 1) * SPS), d;
	int8_t sub = 0;

	// The level far from nominal (a tap of unknown or wrong scale): bring the line and the gain to it, so the
	// samples keep their resolution and their headroom
	if (lvl < LVL_NOM * 4 / 5 || lvl > LVL_NOM * 5 / 4) {
		int32_t f = (LVL_NOM * 256) / lvl;	// x 256

		for (unsigned i = 0; i < LINE; i++)
			m->line[i] = (int8_t)fxp_clamp((m->line[i] * f) / 256, -127, 127);
		m->gain = (m->gain * (uint32_t)f) >> 8;		// (f is 1024 at most: 32 bits hold it)
		if (m->gain > GAIN_MAX)
			m->gain = GAIN_MAX;
		m->env = (m->env / f) * 256 + ((m->env % f) * 256) / f;
		m->env_fast = (m->env_fast / f) * 256 + ((m->env_fast % f) * 256) / f;
		off = (off * f) / 256;
		lvl = LVL_NOM;
	}
	m->scale = (uint16_t)(((AIR_SOFT_OUTER / 3) * 65536) / lvl);
	m->off = (int16_t)off;
	// Where between the samples the error was least: a parabola through the candidate and its two neighbors
	if (m->cand_prev_q != Q_NONE && q_next != Q_NONE) {
		int32_t a = m->cand_prev_q, b = m->cand_q, c = q_next, curv = a - 2 * b + c;

		if (curv > 0) {
			int32_t x = (8 * (a - c)) / curv;

			sub = (int8_t)(x > 8 ? 8 : x < -8 ? -8 : x);
		}
	}
	// The next symbol to leave moves to the nearest center of the new timing: no symbol is lost or sent twice,
	// whatever the old timing was. It is still ahead of the sync's first symbol (EMIT_AT is past a sync's length)
	d = (uint8_t)((m->due + 5 * SPS - first % SPS) % SPS);
	if (d || sub != m->sub)
		flush(m);		// the symbols before are on the old timing: the next chunk's time is the new one
	m->sub = sub;
	m->due = (uint8_t)(d <= SPS / 2 ? m->due - d : m->due + SPS - d);
	m->sync_wait = (uint8_t)((m->due - first) / SPS + 1);
	m->sync_wait_id = s->id;
	m->since_sync = 0;
	m->locked = true;
	m->cand_q = Q_NONE;
	m->hold = SPS;
}

// The patterns that may start (or climb) a candidate now: every one, but while a lock of a SYNC_GRID profile holds
// and no sync is due, START_ONLY ones only (a new transmission, wherever it starts). The grid rule: a sync is due a
// whole number of sync spacings (one or more) after the one that set the timing, GRID_TOL either side, counted from
// pattern end to pattern end (a grid profile's patterns end where a frame's sync does)
static unsigned allowed(const air_softmodem_t *m)
{
	const air_profile_t *p = m->p;
	uint32_t every = sync_spacing(p), n = (uint32_t)m->since_sync + GRID_TOL;

	if (!(p->flags & AIR_PROFILE_SYNC_GRID) || !m->locked || !every)
		return m->valid;
	if (n >= every) {
		// (a lock ends two spacings and two sync lengths after its sync: this runs three times at most)
		do
			n -= every;
		while (n >= every);
		if (n <= 2 * GRID_TOL)
			return m->valid;
	}
	return m->valid & m->start_only;
}

// Fit the symbol-spaced samples that end `back` samples before the line's newest to each pattern of the set; returns
// the least error (past Q_MAX too: the sides of a peak place its bottom), Q_NONE when no pattern's slope is there;
// with `limit`, only an error below it (limit when none is). The pattern's constants (levels, sums, determinant) are
// worked out here, from its symbols: the state keeps none
static uint16_t search(const air_softmodem_t *m, unsigned back, unsigned set, uint16_t limit, uint8_t *which,
		       int16_t *lvl, int16_t *off)
{
	static const int8_t level[4] = { 1, 3, -1, -3 };	// air_sym_level's, read here a byte at a time
	int8_t x[AIR_SYNC_SYMS_MAX];
	const air_profile_t *p = m->p;
	int32_t lvl_min = m->gain_known ? LVL_MIN_KNOWN : LVL_MIN_TRACKED;
	int32_t sx = 0, sxx = 0;
	uint16_t best = limit;
	unsigned summed = 0, squared = 0, got = 0, valid = set & m->valid;
	int at = (int)m->line_at - 1 - (int)back;

	if (at < 0)
		at += LINE;
	for (unsigned i = 0; valid >> i; i++) {
		const air_sync_t *s = &p->sync[i];
		int32_t n = s->nsyms, sxe = 0, s1 = 0, s2 = 0, det, num, a, b, r, a2;
		uint32_t q;

		if (!(valid >> i & 1))
			continue;
		// x[k]: the sample k symbols back, as far as the longest pattern fitted so far
		for (; got < (unsigned)n; got++) {
			x[got] = m->line[at];
			at -= SPS;
			if (at < 0)
				at += LINE;
		}
		if (summed != s->nsyms) {
			sx = 0;
			for (int k = 0; k < n; k++)
				sx += x[k];
			summed = s->nsyms;
		}
		for (int k = 0; k < n; k += 4) {
			unsigned byte = s->syms[k >> 2];

			for (int j = k; j < k + 4 && j < n; j++, byte <<= 2) {
				int e = level[byte >> 6 & 3];

				sxe += x[n - 1 - j] * e;
				s1 += e;
				s2 += e * e;
			}
		}
		det = n * s2 - s1 * s1;
		num = n * sxe - s1 * sx;
		// Most samples stop here: no slope of the pattern's sign and size
		if (num * 256 < lvl_min * det)
			continue;
		a = (num * 256) / det;				// level, line units x 256
		b = (sx * 256 - a * s1) / n;			// offset, x 256
		if (a > LVL_MAX || b > OFF_MAX || b < -OFF_MAX)
			continue;
		if (squared != s->nsyms) {
			sxx = 0;
			for (int k = 0; k < n; k++)
				sxx += x[k] * x[k];
			squared = s->nsyms;
		}
		r = sxx - (a * sxe + b * sx) / 256;		// what the line leaves: the sum of squared errors
		if (r < 0)
			r = 0;
		a2 = ((a * a) >> 16) * n;			// (16 or more: a is lvl_min or more)
		// No better than the best so far (r << 8 and best x a2 are 2^30 at most): no divide
		if (best != Q_NONE && ((uint32_t)r << 8) >= (uint32_t)best * (uint32_t)a2)
			continue;
		q = ((uint32_t)r << 8) / (uint32_t)a2;
		if (q >= Q_NONE)
			q = Q_NONE - 1;
		if (q >= best)
			continue;
		best = (uint16_t)q;
		*which = (uint8_t)i;
		*lvl = (int16_t)a;
		*off = (int16_t)b;
	}
	return best;
}

// Which patterns of the set can end at the newest sample (steps, signs: its phase's histories): their outer symbols'
// signs all there but SIGN_TOL, and their large steps all (up to STEP_TOL_SYMS symbols) or all but one. A pattern's
// words 0 / 0: that gate passes it. (Every sample comes here, and most stop at the first test)
static unsigned gate(const air_softmodem_t *m, unsigned set, unsigned steps, unsigned signs)
{
	const air_sync_t *s = m->p->sync;
	unsigned pass = 0;

	for (unsigned i = 0; set; i++, set >>= 1, s++) {
		unsigned v;

		if (!(set & 1))
			continue;
		v = (signs ^ s->sign_val) & s->sign_mask;
		if (v & (v - 1)) {
			// Two wrong or more (most samples stop here): only a longer pattern allows more than one. All but one of
			// them cleared, that one is the test's
			if (s->nsyms <= SIGN_TOL1_SYMS)
				continue;
			for (unsigned k = SIGN_TOL(s->nsyms); k > 1 && v; k--)
				v &= v - 1;
			if (v & (v - 1))
				continue;
		}
		v = (steps ^ s->gate_up) & s->gate_mask;
		if (s->nsyms > STEP_TOL_SYMS)
			v &= v - 1;
		if (!v)
			pass |= 1u << i;
	}
	return pass;
}

// One filtered sample v (summed input counts, before the gain) into the line; t_us as emit's
static void filtered(air_softmodem_t *m, int32_t v, uint32_t t_us)
{
	int32_t y = fxp_clamp(fxp_scale16(v, m->gain), -127, 127);
	unsigned at = m->line_at, ph = m->phase;
	unsigned steps = ((unsigned)m->steps[ph] << 1 | (unsigned)(y > m->line[at >= SPS ? at - SPS : at + LINE - SPS])) & 0xFFFFu;
	unsigned signs = ((unsigned)m->signs[ph] << 1 | (unsigned)(y > 0)) & 0xFFFFu;
	uint16_t q;
	uint8_t which = 0;
	int16_t lvl = 0, off = 0;

	m->line[at] = (int8_t)y;
	m->line_at = (uint8_t)(at + 1 == LINE ? 0 : at + 1);
	m->steps[ph] = (uint16_t)steps;
	m->signs[ph] = (uint16_t)signs;
	m->phase = (uint8_t)(ph + 1 == SPS ? 0 : ph + 1);
	if (m->line_n < LINE) {
		m->line_n++;
		return;
	}
	m->due++;
	// The fit, where it can matter: every pattern the grid rule allows on a peak being climbed, else those of them the
	// gates let through. A peak's near side is the sample before, fitted with every allowed pattern when a candidate
	// starts (a gated-out pattern can't start one, but its error on the near side places the bottom as it would
	// without the gates)
	if (m->hold) {
		m->hold--;
	} else if (m->cand_q != Q_NONE) {
		// Climbing a peak: on while the error still falls, taken as soon as it rises. The candidate's own pattern
		// stays in the set if the grid rule's window closes during the climb (its far side places the bottom)
		q = search(m, 0, allowed(m) | 1u << m->cand_sync, Q_NONE, &which, &lvl, &off);
		if (q < m->cand_q) {	// (a candidate is Q_MAX or better)
			m->cand_prev_q = m->cand_q;
			m->cand_q = q;
			m->cand_sync = which;
			m->cand_lvl = lvl;
			m->cand_off = off;
		} else {
			commit(m, 1, q);
		}
	} else {
		unsigned set = m->locked ? allowed(m) : m->valid, pass = gate(m, set, steps, signs);

		if (pass && (q = search(m, 0, pass, Q_MAX + 1, &which, &lvl, &off)) <= Q_MAX) {
			uint8_t w;
			int16_t l, o;

			m->cand_prev_q = search(m, 1, set, Q_NONE, &w, &l, &o);
			m->cand_q = q;
			m->cand_sync = which;
			m->cand_lvl = lvl;
			m->cand_off = off;
		}
	}
	while (m->due >= EMIT_AT) {
		emit(m, t_us);
		m->due = (uint8_t)(m->due - SPS);
	}
	// A lock ends when the syncs stop coming: the averages follow the input again
	if (m->locked && m->since_sync > (sync_spacing(m->p) ? 2u * sync_spacing(m->p) : 4800u) + 2 * AIR_SYNC_SYMS_MAX)
		m->locked = false;
}

void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	uint32_t us_q = 1000000u / m->rate_hz, us_r = 1000000u % m->rate_hz, us_acc = 0;
	uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;
	// From the newest input sample back to the newest filtered one: the interpolator's 47.5 / 12 input samples
	// (less the phase, below) and half the integrate and dump
	uint32_t delay_us = (uint32_t)((47500000ull / 12) / m->rate_hz) + (uint32_t)(m->box - 1) * 500000u / out_rate;
	int32_t target = (m->p->levels == 4 ? 2 : 3) * LINE_LVL;
	unsigned box = m->box, den = m->den;
	// What a filtered sample's step adds to the interpolator phase: whole phases, and the rest in 1/den
	unsigned ph_step = (unsigned)m->step * PHASES / den, ph_rstep = (unsigned)m->step * PHASES % den;

	if (m->running) {
		int32_t d = (int32_t)(t_us - m->t_next_us);

		if (d > HOLE_US || d < -HOLE_US)
			air_softmodem_rx_reset(m);
	}
	m->running = true;
	for (unsigned i = 0; i < n; i++) {
		int32_t x = samples[i], v = x - (m->dc >> 10);
		unsigned at = (m->in_at + 1u) & (TAPS - 1u);

		if (!m->locked) {
			m->dc += (x * 1024 - m->dc) >> m->track_shift;
			if (!m->gain_known) {
				// A random 4-level signal averages 2 levels in magnitude, a 2-level one its only level, 3
				int32_t mag = v < 0 ? -v : v;

				m->env += (mag * 1024 - m->env) >> m->track_shift;
				m->env_fast += (mag * 1024 - m->env_fast) >> TRACK_SHIFT_MIN;
				if (m->env < 1024)
					m->env = 1024;
				// The level stepped by half or double (a signal after a quiet carrier, or the other way): the
				// slow averages would take hundreds of ms to follow, the gain clipping or starving every symbol
				// meanwhile. They start over at their fastest
				if (m->env_fast > 2 * m->env || 2 * m->env_fast < m->env) {
					m->env = m->env_fast < 1024 ? 1024 : m->env_fast;
					m->track_shift = TRACK_SHIFT_MIN;
					m->track_n = 1u << TRACK_SHIFT_MIN;
				}
				m->gain = (((uint32_t)target << 26) / (uint32_t)m->env) / box;
			}
			if (m->track_shift < TRACK_SHIFT_MAX && --m->track_n == 0) {
				m->track_shift++;
				m->track_n = (uint16_t)(1u << m->track_shift);
			}
		}
		// Twice, TAPS apart: the last TAPS samples are in[at + 1 ..], oldest first, whatever at is
		m->in[at] = m->in[at + TAPS] = fxp_sat16(v);
		m->in_at = (uint8_t)at;
		while (m->pos < den) {
			// (a row read backward is the mirror phase's row: the table is symmetric)
			int32_t y = fxp_sat16(fxp_dot16_8(interp[PHASES - 1 - m->ph], &m->in[at + 1]) >> 14);
			unsigned sa = m->sum_at, r = m->ph_r + ph_rstep;

			m->acc += y - m->sum[sa];
			m->sum[sa] = (int16_t)y;
			m->sum_at = (uint8_t)(sa + 1 == box ? 0 : sa + 1);
			filtered(m, m->acc, t_us - delay_us);
			m->pos = (uint16_t)(m->pos + m->step);
			m->ph = (uint8_t)(m->ph + ph_step);
			if (r >= den) {
				r -= den;
				m->ph++;
			}
			m->ph_r = (uint16_t)r;
		}
		m->pos = (uint16_t)(m->pos - den);
		m->ph = (uint8_t)(m->ph - PHASES);
		t_us += us_q;
		us_acc += us_r;
		if (us_acc >= m->rate_hz) {
			us_acc -= m->rate_hz;
			t_us++;
		}
	}
	m->t_next_us = t_us;
	flush(m);
}
