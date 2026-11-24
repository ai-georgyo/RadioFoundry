// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 in the codeplug (m17_codeplug.h, docs/spec/m17-codeplug.md): kind 6 and system type 6 on the generic records of
// docs/spec/channel-modes.md. Bytes to values and back; reads only, but for a channel and the radio-wide record written
// from the radio
#include "config.h"
#if CONFIG_M17	// the whole file
#include <string.h>
#include "m17/m17_codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "crypto/crypto_wipe.h"

#define CONV_RESERVED	0xF8u	// byte 0 bits 3-7: kept as found when a block is written over
#define WIDE_RESERVED	0xFCu	// the radio-wide flags' bits 2-7: the same
#define SYSTEMS		0xFFu	// system numbers 0-254 (0xFF is none)
#define ENTRIES		0x100u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// --- A channel's parameter block (m17-codeplug.md 2) ---

void m17_conv_decode(const uint8_t *b, int len, m17_conv_t *out)
{
	uint8_t f = len >= 1 ? b[0] : 0u;
	uint8_t tx = len >= 3 ? b[2] : 0u;
	uint8_t enc = len >= 10 ? b[9] : (uint8_t)M17_ENC_NONE;

	out->squelch = M17_CP_SQUELCH(f) <= 2u ? (uint8_t)M17_CP_SQUELCH(f) : 0u;	// 3 is no squelch: the CAN's
	out->mode1600 = (f & M17_CP_1600) != 0;
	out->can = len >= 2 ? (uint8_t)(b[1] & M17_CAN_MAX) : 0u;
	out->tx_can = (tx & M17_CP_TX_CAN_SET) != 0 ? (uint8_t)(tx & M17_CAN_MAX) : out->can;
	// A destination cut short reads as zero, which is broadcast: never a part of an address
	out->dst = len >= 9 ? m17_addr_get(&b[3]) : 0u;
	if (out->dst == 0u)
		out->dst = M17_ADDR_BROADCAST;
	// An encryption this build doesn't have, or one the spec doesn't define, is none: the channel sends in the clear
	// only where it was set up to, never in a cipher the receiver can't know
	out->tx_enc = (enc == M17_ENC_SCRAMBLER && CONFIG_M17_SCRAMBLER) || (enc == M17_ENC_AES && CONFIG_M17_AES) ?
		      enc : (uint8_t)M17_ENC_NONE;
	out->key_id = len >= 12 ? rd16(&b[10]) : 0u;
}

void m17_conv_encode(const m17_conv_t *c, uint8_t out[M17_CP_CONV_LEN])
{
	uint8_t can = c->can & M17_CAN_MAX, tx = c->tx_can & M17_CAN_MAX;
	uint64_t dst = c->dst == M17_ADDR_BROADCAST ? 0u : c->dst;

	out[0] = (uint8_t)((c->squelch <= 2u ? c->squelch : 0u) | (c->mode1600 ? M17_CP_1600 : 0u));
	out[1] = can;
	out[2] = tx == can ? 0u : (uint8_t)(M17_CP_TX_CAN_SET | tx);
	m17_addr_put(&out[3], dst);
	out[9] = c->tx_enc == M17_ENC_SCRAMBLER || c->tx_enc == M17_ENC_AES ? c->tx_enc : (uint8_t)M17_ENC_NONE;
	out[10] = (uint8_t)c->key_id;
	out[11] = (uint8_t)(c->key_id >> 8);
}

// --- The radio-wide record (m17-codeplug.md 3) ---

void m17_wide_decode(const uint8_t *b, int len, m17_wide_t *out)
{
	uint8_t f = len >= 7 ? b[6] : 0u;
	uint8_t st = len >= 8 ? b[7] : (uint8_t)M17_STATION_FIXED;
	int n = 0;

	memset(out, 0, sizeof(*out));
	out->src = len >= 6 ? m17_addr_get(b) : 0u;
	out->sign = (f & M17_CP_WIDE_SIGN) != 0;
	out->gnss = (f & M17_CP_WIDE_GNSS) != 0;
	// The station types the spec defines (m17.md 4.2); a reserved one reads as the zero default
	out->station = st <= M17_STATION_HANDHELD || st == M17_STATION_OTHER ? st : (uint8_t)M17_STATION_FIXED;
	// The text up to its first zero byte: the rest is padding
	while (n < (int)M17_TEXT_MAX && 8 + n < len && b[8 + n] != 0u) {
		out->text[n] = b[8 + n];
		n++;
	}
	out->text_len = (uint8_t)n;
}

void m17_wide_encode(const m17_wide_t *w, uint8_t out[M17_CP_WIDE_LEN])
{
	uint8_t n = w->text_len <= M17_TEXT_MAX ? w->text_len : (uint8_t)M17_TEXT_MAX;

	memset(out, 0, M17_CP_WIDE_LEN);
	m17_addr_put(out, w->src);
	out[6] = (uint8_t)((w->sign ? M17_CP_WIDE_SIGN : 0u) | (w->gnss ? M17_CP_WIDE_GNSS : 0u));
	out[7] = w->station;
	for (uint8_t i = 0; i < n && w->text[i] != 0u; i++)
		out[8 + i] = w->text[i];	// a zero byte would end the text: what follows it is padding
}

void m17_wide_load(m17_wide_t *out)
{
	uint8_t b[M17_CP_WIDE_LEN];

	m17_wide_decode(b, codeplug_mode_settings(CODEPLUG_KIND_M17, b, sizeof b), out);
}

bool m17_wide_save(const m17_wide_t *w)
{
	uint8_t b[M17_CP_WIDE_LEN], was[M17_CP_WIDE_LEN];
	int len = codeplug_mode_settings(CODEPLUG_KIND_M17, was, sizeof was);

	m17_wide_encode(w, b);
	if (len >= 7)
		b[6] |= (uint8_t)(was[6] & WIDE_RESERVED);	// a later firmware's flags stay as they were
	// The first 60 bytes; what a later layout appended after them stays (codeplug-settings.md 4)
	codeplug_kv_op_t op = { (uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17),
				(uint16_t)(M17_CP_WIDE_LEN | CODEPLUG_KV_HEAD), b };
	return codeplug_kv_write(&op, 1);
}

uint64_t m17_own_addr(bool *bad)
{
	m17_wide_t w;
	char name[CODEPLUG_NAME_LEN + 1];
	uint64_t a;

	if (bad != NULL)
		*bad = false;
	m17_wide_load(&w);
	if (w.src != 0u) {
		if (w.src <= M17_ADDR_STD_MAX)
			return w.src;
		// Extended or broadcast: no station's callsign, and never a fall back to the radio name the user may not
		// mean: PTT is refused until it is fixed
		if (bad != NULL)
			*bad = true;
		return 0u;
	}
	codeplug_radio_name(name);
	return m17_addr_from_text(name, &a) ? a : 0u;
}

// --- Channels ---

// An M17 system's header: true when system n is one
static bool is_m17(uint8_t n, codeplug_system_t *s)
{
	uint8_t hdr[CODEPLUG_SYS_HDR];

	return codeplug_system(n, s, hdr, 0) >= 0 && s->type == CODEPLUG_SYSTYPE_M17;
}

bool m17_conv_load(uint16_t number, const channel_t *ch, m17_conv_cfg_t *out)
{
	uint8_t b[M17_CP_CONV_LEN + CODEPLUG_TAG_HEAD_MAX];
	codeplug_system_t s;
	int len;

	if (ch->kind != CODEPLUG_KIND_M17)
		return false;
	len = codeplug_tag_params(number, ch, b, M17_CP_CONV_LEN);
	if (len < 0)
		return false;
	m17_conv_decode(b, len < (int)M17_CP_CONV_LEN ? len : (int)M17_CP_CONV_LEN, &out->conv);
	out->src = m17_own_addr(NULL);
	out->system = CODEPLUG_SYSTEM_NONE;
	out->tx_allowed = true;
	if (ch->system != CODEPLUG_SYSTEM_NONE) {
		// A system that isn't here, or isn't an M17 one, allows nothing: transmit needs the system it names
		out->tx_allowed = false;
		if (is_m17(ch->system, &s)) {
			out->system = ch->system;
			out->tx_allowed = (s.flags & CODEPLUG_SYS_TX) != 0;
		}
	}
	return true;
}

bool m17_conv_save(uint16_t number, const channel_t *ch, const m17_conv_t *conv)
{
	channel_t c = *ch;
	uint8_t blk[M17_CP_CONV_LEN];
	uint8_t was[CODEPLUG_TAG_HEAD_MAX + 1u];
	codeplug_tag_t t0 = { 0 };
	// A channel of the kind already keeps the system it names
	uint8_t system = ch->kind == CODEPLUG_KIND_M17 ? ch->system : CODEPLUG_SYSTEM_NONE;
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_M17, .system = system, .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX };
	// The block's first byte as stored, read before the record write lets the tag follow: its reserved bits stay
	int len = codeplug_tag_read(number, &t0) && t0.kind == CODEPLUG_KIND_M17 ?
		  codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + number - 1u), was, sizeof was) : -1;
	uint8_t keep = len > (int)codeplug_tag_params_at(&t0) ? (uint8_t)(was[codeplug_tag_params_at(&t0)] & CONV_RESERVED) : 0u;

	// The safe stand-in: DMR, receive only (the tag carries transmit allowed). Under a tag that says the base record
	// is a channel of its own, codeplug_set_channel keeps that channel's mode instead
	c.kind = CODEPLUG_KIND_M17;
	c.system = system;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	m17_conv_encode(conv, blk);
	if (!codeplug_set_channel(number, &c))
		return false;
	// A tag of the kind that followed the record keeps its fallback, its other flags, its block's reserved bits and
	// what it stores after this block (an editor's or a later firmware's); else a new one, reserved bits 0
	blk[0] |= keep;
	if (codeplug_tag_rewrite(number, c.name, &t, blk, sizeof blk))
		return true;
	blk[0] &= (uint8_t)~CONV_RESERVED;
	return codeplug_tag_write(number, c.name, &t, blk, sizeof blk);
}

// --- Keys (m17-codeplug.md 4) ---

uint8_t m17_system_key(uint8_t system, uint8_t enc, uint16_t key_id, uint8_t key[32])
{
	codeplug_system_t s;
	uint8_t alg = enc == M17_ENC_SCRAMBLER ? (uint8_t)M17_CP_ALG_SCRAMBLER : enc == M17_ENC_AES ? (uint8_t)M17_CP_ALG_AES : 0u;
	int n;
	uint8_t any = 0;

	if (alg == 0u || !is_m17(system, &s))
		return 0;
	n = codeplug_traffic_key(system, alg, key_id, key, 32);
	for (int i = 0; i < n && i < 32; i++)
		any |= key[i];
	// A key of a length its algorithm doesn't have is missing, never cut or padded into another key. An all-zero
	// scrambler seed would keep the generator at zero and send the voice in the clear: missing too
	if (alg == M17_CP_ALG_SCRAMBLER ? (n >= 1 && n <= 3 && any != 0u) : (n == 16 || n == 24 || n == 32))
		return (uint8_t)n;
	crypto_wipe(key, 32);
	return 0;
}

// 1 <= d <= n - 1, n the order of P-256 (p256.md 2): d is not zero and d - n borrows. Every byte looked at, whatever
// d holds: d is a secret
static bool priv_ok(const uint8_t d[32])
{
	static const uint8_t order[32] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
	};
	unsigned borrow = 0;
	uint8_t any = 0;

	for (int i = 31; i >= 0; i--) {
		borrow = (((unsigned)d[i] - order[i] - borrow) >> 8) & 1u;
		any |= d[i];
	}
	return (borrow & (any != 0u)) != 0u;
}

// The secret read whole: static, not on the radio task's stack, and wiped before return
static uint8_t secret[M17_CP_SECRET_LEN];

bool m17_system_signing_key(uint8_t system, uint8_t d[32], uint8_t q_c[33], bool *has_q)
{
	codeplug_system_t s;
	int n = is_m17(system, &s) ? codeplug_system_secret(system, secret, sizeof secret) : -1;
	bool ok = (n == 32 || n >= (int)M17_CP_SECRET_LEN) && priv_ok(secret);

	*has_q = false;
	if (ok) {
		memcpy(d, secret, 32);
		// Q as the editor stored it; one that isn't a compressed key is left for the radio to derive
		*has_q = n >= (int)M17_CP_SECRET_LEN && (secret[32] == 2u || secret[32] == 3u);
		if (*has_q && q_c != NULL)
			memcpy(q_c, &secret[32], 33);
	}
	crypto_wipe(secret, sizeof secret);
	return ok;
}

// The M17 systems, one bit each
static void m17_systems(uint8_t bits[32])
{
	codeplug_kv_cursor_t c;
	uint16_t key;
	uint8_t type;
	int len;

	memset(bits, 0, 32);
	codeplug_kv_begin(&c, CODEPLUG_KEY_SYSTEM, (uint16_t)(CODEPLUG_KEY_SYSTEM + SYSTEMS - 1u));
	while ((len = codeplug_kv_next(&c, &key, &type, 1)) >= 0) {
		if (len >= (int)CODEPLUG_SYS_HDR && type == CODEPLUG_SYSTYPE_M17) {
			key = (uint16_t)(key - CODEPLUG_KEY_SYSTEM);
			bits[key >> 3] |= (uint8_t)(1u << (key & 7u));
		}
	}
}

static bool bit(const uint8_t *bits, unsigned n) { return (bits[n >> 3] & (1u << (n & 7u))) != 0; }

uint8_t m17_signing_system(uint8_t system)
{
	uint8_t bits[32];

	m17_systems(bits);
	if (system != CODEPLUG_SYSTEM_NONE && bit(bits, system) && codeplug_system_secret(system, NULL, 0) >= 0)
		return system;
	for (unsigned n = 0; n < SYSTEMS; n++) {
		if (bit(bits, n) && codeplug_system_secret((uint8_t)n, NULL, 0) >= 0)
			return (uint8_t)n;
	}
	return CODEPLUG_SYSTEM_NONE;
}

// A station key entry (class 128): system, class, group, flags, then the 6-byte address and the compressed key
#define STATION_AT	CODEPLUG_SYS_ENTRY_HDR
typedef struct {
	uint8_t v[CODEPLUG_SYS_ENTRY_HDR + M17_CP_STATION_LEN];
} station_t;

// Entry key's value, when it is a station key of an M17 system: false for any other entry, or one too short or whose
// key isn't a compressed one
static bool station(const uint8_t *m17, const uint8_t *v, int len)
{
	return len >= (int)(CODEPLUG_SYS_ENTRY_HDR + M17_CP_STATION_LEN) && v[1] == M17_CP_ENTRY_STATION &&
	       bit(m17, v[0]) && (v[STATION_AT + M17_ADDR_BYTES] == 2u || v[STATION_AT + M17_ADDR_BYTES] == 3u);
}

// The station keys in entry number order, one bit each: those of system (when only_sys) or of the other systems,
// for addr (or any address with all), then the i-th one read into *e. False past the last
static bool station_pick(const uint8_t *m17, uint8_t system, bool only_sys, bool all, uint64_t addr, unsigned i,
			 station_t *e)
{
	uint8_t want[ENTRIES / 8];
	codeplug_kv_cursor_t c;
	uint16_t key;
	int len;

	memset(want, 0, sizeof want);
	codeplug_kv_begin(&c, CODEPLUG_KEY_SYS_CHAN, (uint16_t)(CODEPLUG_KEY_SYS_CHAN + ENTRIES - 1u));
	while ((len = codeplug_kv_next(&c, &key, e->v, sizeof e->v)) >= 0) {
		if (station(m17, e->v, len) && (all || (e->v[0] == system) == only_sys) &&
		    (all || m17_addr_get(&e->v[STATION_AT]) == addr)) {
			key = (uint16_t)(key - CODEPLUG_KEY_SYS_CHAN);
			want[key >> 3] |= (uint8_t)(1u << (key & 7u));
		}
	}
	for (unsigned n = 0; n < ENTRIES; n++) {
		if (bit(want, n) && i-- == 0u)
			return codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), e->v, sizeof e->v) >=
			       (int)sizeof e->v;
	}
	return false;
}

// How many station keys of addr station_pick finds in that part
static unsigned station_count(const uint8_t *m17, uint8_t system, bool only_sys, uint64_t addr)
{
	codeplug_kv_cursor_t c;
	station_t e;
	uint16_t key;
	unsigned n = 0;
	int len;

	codeplug_kv_begin(&c, CODEPLUG_KEY_SYS_CHAN, (uint16_t)(CODEPLUG_KEY_SYS_CHAN + ENTRIES - 1u));
	while ((len = codeplug_kv_next(&c, &key, e.v, sizeof e.v)) >= 0)
		n += station(m17, e.v, len) && (e.v[0] == system) == only_sys && m17_addr_get(&e.v[STATION_AT]) == addr;
	return n;
}

bool m17_station_key(uint8_t system, uint64_t addr, uint8_t i, uint8_t key[33])
{
	uint8_t m17[32];
	station_t e;
	unsigned own = 0;
	bool found;

	m17_systems(m17);
	if (system != CODEPLUG_SYSTEM_NONE && bit(m17, system)) {
		own = station_count(m17, system, true, addr);
		found = i < own ? station_pick(m17, system, true, false, addr, i, &e) :
			station_pick(m17, system, false, false, addr, i - own, &e);
	} else {
		// No M17 system of its own: every system's keys are "the others"
		found = station_pick(m17, CODEPLUG_SYSTEM_NONE, false, false, addr, i, &e);
	}
	if (found)
		memcpy(key, &e.v[STATION_AT + M17_ADDR_BYTES], 33);
	return found;
}

bool m17_station_key_at(uint16_t i, uint64_t *addr, uint8_t key[33])
{
	uint8_t m17[32];
	station_t e;

	m17_systems(m17);
	if (!station_pick(m17, CODEPLUG_SYSTEM_NONE, false, true, 0u, i, &e))
		return false;
	*addr = m17_addr_get(&e.v[STATION_AT]);
	memcpy(key, &e.v[STATION_AT + M17_ADDR_BYTES], 33);
	return true;
}

bool m17_station_known(uint64_t addr)
{
	uint8_t key[33];

	return m17_station_key(CODEPLUG_SYSTEM_NONE, addr, 0, key);
}

#endif	// CONFIG_M17
