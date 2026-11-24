// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 sections (docs/spec/debuglink.md 3.7, docs/spec/m17.md, the radio core) and RADIO op 44 (3.8). Five parts,
// as one reply holds about 1 kB: 0 `m17` the call heard, what PTT does, the settings, the signature state, the key;
// 1 `m17_stats` the engine's and the session's counters, the signature bench, the time, the entropy pool;
// 2 `m17_data` the last call's packet and signature, the packet, SMS and signature counters; 3 `m17_voice` the voice
// path and the BER test; 4 `heard`, the stations heard (generic; M17 its only source). Every M17 part says
// "m17.built=0" without CONFIG_M17, the heard part "heard.built=0" without CONFIG_HEARD_LIST.
//
// Addresses are text (m17_addr_text): a callsign, an extended address as 12 hex digits, broadcast ALL, none "-".
// Nothing secret is read here, so nothing secret can be printed: the signing key is its fingerprint, a traffic key not
// even that, no nonce, no pool or generator state, no SMS or META text (counts only).
//
// The op reaches the radio task only through the radio_req_m17_* requests (radio.h); the time goes to utc_set, its
// sequence count's one writer being this task (nothing else sets a host time). Names in '\0'-separated lists, fields
// in tables: the code is in every M17 build, the smallest included (flash: docs/spec/m17.md, the memory section)
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_M17
#include <stddef.h>
#include "hal_time.h"
#include "m17/m17.h"
#include "m17/m17_voice.h"
#include "radio/radio_m17.h"
#include "radio/radio_air_modes.h"
#include "position/position_utc.h"
#if CONFIG_HEARD_LIST
#include "radio/radio_heard.h"
#endif
#if CONFIG_ENTROPY
#include "crypto/entropy.h"
#endif

// The counters' layouts the loops below walk: 32-bit words in declaration order, each run's first and last
_Static_assert(offsetof(radio_m17_stats_t, begins) == 0u && offsetof(radio_m17_stats_t, tx_syms) == 22u * 4u &&
	       offsetof(radio_m17_stats_t, pkt_rx) == 23u * 4u && offsetof(radio_m17_stats_t, sms_busy) == 30u * 4u &&
	       offsetof(radio_m17_stats_t, sig_valid) == 31u * 4u &&
	       offsetof(radio_m17_stats_t, key_damaged) == 39u * 4u &&
	       offsetof(radio_m17_stats_t, verify_us) == 41u * 4u, "radio_m17_stats_t: the runs of words below");
_Static_assert(offsetof(m17_voice_stats_t, rx_in) == 0u && offsetof(m17_voice_stats_t, lent) == 18u * 4u,
	       "m17_voice_stats_t: 19 words");
_Static_assert(RADIO_M17_BERT_MODE == 1u << 19, "the flag names below: one a bit");
#if CONFIG_M17_BERT
_Static_assert(offsetof(radio_m17_bert_t, frames) == 4u && offsetof(radio_m17_bert_t, tx_ms) == 7u * 4u,
	       "radio_m17_bert_t: state, then 7 words");
#endif

#define UNIX_2020	1577836800u	// 2020-01-01: an earlier time (other than 0, clearing it) is a host's mistake

// The n-th word of a list ("a\0b\0"), "?" past its end
static const char *nth(const char *w, unsigned n)
{
	while (n-- > 0 && *w != '\0') {
		w += strlen(w) + 1;
	}
	return *w != '\0' ? w : "?";
}

// One key per word of names, prefix + word = the next 32-bit value of rec from its byte at (a byte pointer into the
// whole record: a uint32_t pointer walked past one member would leave that member's object)
static void u32s(dl_text_t *t, const char *prefix, const char *names, const void *rec, size_t at)
{
	const uint8_t *p = (const uint8_t *)rec + at;
	for (; *names != '\0'; names += strlen(names) + 1, p += sizeof(uint32_t)) {
		uint32_t w;
		memcpy(&w, p, sizeof w);
		dl_text(t, "%s%s=%u\n", prefix, names, (unsigned)w);
	}
}

// An address as the view shows it, in s (M17_ADDR_TEXT) or a constant
static const char *addr_str(const uint8_t a[M17_ADDR_BYTES], char *s)
{
	uint64_t v = m17_addr_get(a);
	if (v == 0) {
		return "-";
	}
	return m17_addr_text(v, s) == M17_ADDR_KIND_BROADCAST ? "ALL" : s;
}

// A record's fields as keys, from a table: per word of names a field, FLD(type, offsetof(...))
enum { F_U8, F_U16, F_S16, F_U32, F_ADDR };
#define FLD(type, field)	{ (uint8_t)(field), type }
typedef struct {
	uint8_t at, type;
} field_t;

static void fields(dl_text_t *t, const char *prefix, const char *names, const field_t *f, const void *rec)
{
	for (; *names != '\0'; names += strlen(names) + 1, f++) {
		const uint8_t *p = (const uint8_t *)rec + f->at;
		char s[M17_ADDR_TEXT];
		uint32_t w = *p;
		uint16_t h;
		if (f->type == F_ADDR) {
			dl_text(t, "%s%s=%s\n", prefix, names, addr_str(p, s));
			continue;
		}
		if (f->type == F_U32) {
			memcpy(&w, p, sizeof w);
		} else if (f->type != F_U8) {
			memcpy(&h, p, sizeof h);
			w = f->type == F_S16 ? (uint32_t)(int32_t)(int16_t)h : h;
		}
		dl_text(t, f->type == F_S16 ? "%s%s=%d\n" : "%s%s=%u\n", prefix, names, (unsigned)w);
	}
}

// radio_m17_settings_t (the RAM settings, and those in force)
static const field_t set_f[] = { FLD(F_U8, offsetof(radio_m17_settings_t, can)),
				 FLD(F_U8, offsetof(radio_m17_settings_t, tx_can)),
				 FLD(F_U8, offsetof(radio_m17_settings_t, squelch)),
				 FLD(F_U8, offsetof(radio_m17_settings_t, mode1600)),
				 FLD(F_ADDR, offsetof(radio_m17_settings_t, dst)) };
#define SET_NAMES	"can\0tx_can\0squelch\0mode1600\0dst\0"

// radio_m17_view_t: the call heard, what PTT does
static const field_t view_f[] = { FLD(F_U8, offsetof(radio_m17_view_t, tx)),
				  FLD(F_U8, offsetof(radio_m17_view_t, tx_phase)), FLD(F_U8, offsetof(radio_m17_view_t, can)),
				  FLD(F_U8, offsetof(radio_m17_view_t, rx_can)), FLD(F_U8, offsetof(radio_m17_view_t, enc)),
				  FLD(F_U8, offsetof(radio_m17_view_t, tx_enc)),
				  FLD(F_U8, offsetof(radio_m17_view_t, call_seq)),
				  FLD(F_U8, offsetof(radio_m17_view_t, meta_seq)),
				  FLD(F_S16, offsetof(radio_m17_view_t, rssi_dbm)),
				  FLD(F_U16, offsetof(radio_m17_view_t, tot_s)), FLD(F_ADDR, offsetof(radio_m17_view_t, src)),
				  FLD(F_ADDR, offsetof(radio_m17_view_t, dst)), FLD(F_ADDR, offsetof(radio_m17_view_t, orig)),
				  FLD(F_ADDR, offsetof(radio_m17_view_t, via)), FLD(F_ADDR, offsetof(radio_m17_view_t, talk)),
				  FLD(F_ADDR, offsetof(radio_m17_view_t, own)) };
#define VIEW_NAMES	"tx\0tx_phase\0can\0rx_can\0enc\0tx_enc_type\0call_seq\0meta_seq\0rssi_dbm\0tot_s\0src\0dst\0" \
			"orig\0via\0talk\0call\0"

// radio_m17_details_t: the last call in full
static const field_t det_f[] = { FLD(F_U32, offsetof(radio_m17_details_t, pkt_type)),
				 FLD(F_U16, offsetof(radio_m17_details_t, pkt_len)),
#if CONFIG_M17_SIG
				 FLD(F_ADDR, offsetof(radio_m17_details_t, signer)),
				 FLD(F_U32, offsetof(radio_m17_details_t, sig_ts)),
#endif
};
#define DET_NAMES	"pkt_type\0pkt_len\0" DET_SIG_NAMES
#if CONFIG_M17_SIG
#define DET_SIG_NAMES	"signer\0sig_ts\0"
#else
#define DET_SIG_NAMES	""
#endif

// The UTC source in force (position_utc.h): the freshest at any age, and how old it is to the second. The interface
// gives the time, not the age: the age is the smallest window that still finds a source
static void time_keys(dl_text_t *t)
{
	uint32_t now = hal_millis(), unix_s = 0, lo = 0, hi = UINT32_MAX / 1000u - 1u;
	uint8_t src;
	if (position_utc_now(now, UINT32_MAX, &unix_s, &src)) {
		while (lo < hi) {
			uint32_t mid = lo + (hi - lo) / 2u;
			if (position_utc_now(now, mid * 1000u + 999u, NULL, NULL)) {
				hi = mid;
			} else {
				lo = mid + 1u;
			}
		}
	}
	dl_text(t, "time.src=%s\ntime.age_s=%u\ntime.unix=%u\n", nth("none\0gnss\0host\0", src), (unsigned)lo,
		(unsigned)unix_s);
}

// Part 0: the call heard now or last, what PTT does, the settings and those in force, the signature's state, the key
static void part_call(const radio_view_t *view, dl_text_t *t)
{
	const radio_m17_view_t *v = &view->m17;
	union {
		radio_m17_settings_t s;
#if CONFIG_M17_SIG
		radio_m17_key_t k;
#endif
	} u;
	// radio_m17_view_t.flags bit n is the n-th word, "-" for one m17.flags alone shows (enc and 1600 have keys of
	// their own: m17.enc, m17.voice)
	static const char flags[] = "on\0rx\0played\0-\0late\0lsf\0-\0text\0gnss\0packet\0bert\0-\0no_call\0call_bad\0-\0-\0"
		"-\0tx_sign\0tx_enc\0bert_mode\0";
	const char *w = flags;

	for (uint32_t bit = 1; *w != '\0'; bit <<= 1, w += strlen(w) + 1) {
		if (*w != '-') {
			dl_text(t, "m17.%s=%u\n", w, (v->flags & bit) != 0);
		}
	}
	dl_text(t, "m17.flags=0x%X\n", (unsigned)v->flags);
	fields(t, "m17.", VIEW_NAMES, view_f, v);
	// ext1 / ext2: an extended callsign named another originator, a reflector (radio_m17.h: orig and via)
	dl_text(t, "m17.fit=%u\nm17.deny=%u\nm17.type=%s\nm17.voice=%u\nm17.ext1=%u\nm17.ext2=%u\nm17.why=%s\n"
		"m17.sig=%s\n", (unsigned)radio_m17_fit(), view->tx_deny,
		(v->flags & RADIO_M17_PACKET) ? "packet" : (v->flags & RADIO_M17_BERT) ? "bert" : "stream",
		(v->flags & (RADIO_M17_DATA | RADIO_M17_PACKET | RADIO_M17_BERT)) ? 0u :
		(v->flags & RADIO_M17_1600) ? 1600u : 3200u,
		memcmp(v->orig, v->src, M17_ADDR_BYTES) != 0,
		m17_addr_get(v->via) != 0 && memcmp(v->via, v->src, M17_ADDR_BYTES) != 0,
		nth("none\0can\0addressed\0enc\0scr\0data\0", v->why),
		nth("none\0unsigned_known\0running\0checking\0valid\0signer_differs\0invalid\0no_key\0late\0lost\0old\0"
		    "replay\0", v->sig));
	radio_m17_settings(&u.s);
	fields(t, "m17.set.", SET_NAMES, set_f, &u.s);
	if (radio_m17_in_force(&u.s)) {
		fields(t, "m17.run.", SET_NAMES, set_f, &u.s);
	}
#if CONFIG_M17_SIG
	// The signing key: its state and fingerprint (m17_key_fp), never d
	char fp[2 * sizeof u.k.fp + 1];
	radio_m17_key(&u.k);
	for (unsigned i = 0; i < sizeof u.k.fp; i++) {
		dl_fmt(fp + 2 * i, 3, "%02X", u.k.present ? u.k.fp[i] : 0u);
	}
	dl_text(t, "m17.key=%s\nm17.key_fp=%s\n",
		!u.k.present ? "none" : !u.k.checked ? "unchecked" : u.k.damaged ? "damaged" : "ok", fp);
#endif
}

// Part 1: the engine's and the session's counters, summed over the runs (radio_m17_stats_t, the first 23 words), the
// signature bench, the time and the entropy pool
static void part_stats(dl_text_t *t)
{
	radio_m17_stats_t c;
	radio_m17_stats(&c);
	u32s(t, "m17.c.", "begins\0begin_fails\0rx_calls\0tx_calls\0tx_filler\0frames\0lsf_ok\0lsf_bad\0lich_ok\0"
	     "lich_bad\0stream\0packet\0bert\0fec_bits\0can_rejects\0eot\0head_rejects\0enc_skipped\0expect_miss\0"
	     "fn_limit\0rx_chunks\0tx_underruns\0tx_syms\0", &c, offsetof(radio_m17_stats_t, begins));
	dl_text(t, "m17.c.sig_tail=%u\nm17.c.nonce_refused=%u\nm17.c.mem_lent=%u\n", c.sig_tail,
		(unsigned)c.nonce_refused, (unsigned)c.mem_lent);
#if CONFIG_M17_SIG
	// The signature bench's latest outcome; the latest signature's and verification's times (the bench's or a call's)
	dl_text(t, "m17.bench.ok=%s\nm17.sign_us=%u\nm17.verify_us=%u\n",
		nth("none\0running\0ok\0failed\0refused\0", radio_m17_bench_result()), (unsigned)c.sign_us,
		(unsigned)c.verify_us);
#endif
	time_keys(t);
#if CONFIG_ENTROPY
	// The pool's source and credit, never its state; nonce_ok: no AES key-up refused for want of a new nonce
	dl_text(t, "crypto.entropy=%s\ncrypto.ent_bits=%u\ncrypto.nonce_ok=%u\n",
		nth("none\0weak\0pool\0trng\0", entropy_source()), (unsigned)entropy_credited(), c.nonce_refused == 0);
#endif
}

// Part 2: the last call's packet and signature, packets and SMS (counts, never text), the signature counters
static void part_data(dl_text_t *t)
{
	union {
		radio_m17_stats_t c;
		radio_m17_details_t d;
	} u;
	// The last call in full (radio_m17_details): a packet's type and length, the signature's fields
	if (radio_m17_details(&u.d)) {
		fields(t, "m17.", DET_NAMES, det_f, &u.d);
#if CONFIG_M17_SIG
		dl_text(t, "m17.sig_fresh=%s\n", nth("unknown\0yes\0no\0", u.d.sig_fresh));
#endif
	}
	radio_m17_stats(&u.c);
#if CONFIG_M17_PACKET
	u32s(t, "m17.c.", "pkt_rx\0pkt_bad\0pkt_tx\0pkt_dropped\0sms_rx\0sms_tx\0sms_dropped\0sms_busy\0", &u.c,
	     offsetof(radio_m17_stats_t, pkt_rx));
	uint8_t seq;
	uint8_t r = radio_m17_sms_result(&seq, NULL);
	dl_text(t, "sms.n=%u\nsms.unread=%u\nsms.result=%s\nsms.result_seq=%u\n", radio_m17_sms_count(),
		radio_m17_sms_unread(), nth("none\0sent\0busy\0", r), seq);
#endif
#if CONFIG_M17_SIG
	u32s(t, "m17.c.", "sig_valid\0sig_invalid\0sig_nokey\0sig_incomplete\0sig_late\0sig_signed\0sig_keys_capped\0"
	     "key_checks\0key_damaged\0", &u.c, offsetof(radio_m17_stats_t, sig_valid));
#endif
}

// Part 3: the voice path (m17_voice_stats_t) and the BER test
static void part_voice(dl_text_t *t)
{
	m17_voice_stats_t s;
	m17_voice_stats(&s);
	u32s(t, "m17v.", "rx_in\0rx_frames\0rx_bad\0rx_concealed\0rx_calls\0rx_underruns\0rx_overflows\0rx_peak\0"
	     "tx_frames\0tx_encoded\0tx_dropped\0tx_mic_peak\0enc_us_max\0dec_us_max\0enc_us_sum\0dec_us_sum\0"
	     "enc_blocks\0dec_blocks\0lent\0", &s, 0);
#if CONFIG_M17_BERT
	radio_m17_bert_t b;
	radio_m17_bert_get(&b);
	dl_text(t, "m17b.state=%s\nm17b.ber_ppm=%u\nm17b.rssi_dbm=%d\n",
		nth("no_signal\0searching\0locked\0lost\0", b.state),
		b.bits != 0 ? (unsigned)((uint64_t)b.errs * 1000000u / b.bits) : 0u, b.rssi_dbm);
	u32s(t, "m17b.", "frames\0bits\0errs\0locks\0losses\0tx_frames\0tx_ms\0", &b,
	     offsetof(radio_m17_bert_t, frames));
#endif
}

#if CONFIG_HEARD_LIST
// An address as one word of a record: a space inside a callsign (base-40 digit 0, "M17-M17 C") as '_', which no
// callsign holds, so the record splits on spaces and '_' maps back
static const char *addr_word(const uint8_t a[M17_ADDR_BYTES], char *s)
{
	const char *w = addr_str(a, s);
	if (w == s) {
		for (char *p = s; *p != '\0'; p++) {
			*p = *p == ' ' ? '_' : *p;
		}
	}
	return w;
}

// Part 4, the heard section: the stations heard, any mode (radio_heard.h; its own section for the reply size).
// heard.<i>=mode from to count rssi_dbm age_s flags (RADIO_HEARD_*), 0 the newest; seven words, an address's spaces
// as '_' (addr_word)
static void part_heard(dl_text_t *t)
{
	radio_heard_t e;
	uint8_t n = radio_heard_count();
	uint32_t now = hal_millis();
	dl_text(t, "heard.n=%u\n", n);
	for (uint8_t i = 0; i < n && radio_heard_get(i, &e); i++) {
		char a[M17_ADDR_TEXT], b[M17_ADDR_TEXT];
		// An entry the radio task added after now was read is a little in the future: age 0, not 49 days
		uint32_t age_ms = e.last_ms - now <= 1000u ? 0u : now - e.last_ms;
		if ((e.flags & RADIO_HEARD_ADDR) == 0) {
			dl_fmt(a, sizeof a, "%u", (unsigned)e.id);
		}
		dl_text(t, "heard.%u=%s %s %s %u %d %u 0x%02X\n", i, radio_mode_key(e.mode),
			(e.flags & RADIO_HEARD_ADDR) ? addr_word(e.addr, a) : a, addr_word(e.to, b), e.count, e.rssi_dbm,
			(unsigned)(age_ms / 1000u), e.flags);
	}
}
#endif
#endif

void debuglink_view_m17(uint8_t part, const radio_view_t *v, dl_text_t *t)
{
	if (part == 4) {
		// The heard list is generic (it needs M17 today: its only source)
		dl_text(t, "heard.built=%u\n", CONFIG_HEARD_LIST);
#if CONFIG_HEARD_LIST
		part_heard(t);
#endif
		return;
	}
#if CONFIG_M17
	dl_text(t, "m17.built=1\n");
	if (part == 0) {
		part_call(v, t);
	} else if (part == 1) {
		part_stats(t);
	} else if (part == 2) {
		part_data(t);
	} else {
		part_voice(t);
	}
#else
	(void)part;
	(void)v;
	dl_text(t, "m17.built=0\n");
#endif
}

void debuglink_m17_reset(void)
{
#if CONFIG_M17
	radio_m17_reset_stats();
	m17_voice_reset_stats();
#endif
}

// RADIO op 44 (docs/spec/debuglink.md 3.8): a[0] the sub-op, then its arguments, each of an exact length
uint8_t debuglink_m17_op(const uint8_t *a, uint16_t len)
{
	if (len < 1) {
		return DL_ST_BAD_ARG;
	}
#if CONFIG_M17
	// Each sub-op's request length, its byte included; 0: 8 or more (the SMS: a destination and its text)
	static const uint8_t want[] = { 11, 2, 0, 1, 3, 2, 5 };
	if (a[0] >= sizeof want || (want[a[0]] != 0 ? len != want[a[0]] : len < 8)) {
		return DL_ST_BAD_ARG;
	}
	switch (a[0]) {
	case DL_M17_SETTINGS: {
		// can u8, tx_can u8, squelch u8 (M17_SQUELCH_*), mode u8 (0 3200, 1 1600), dst 6 bytes big endian (all 0xFF
		// broadcast; zero is no destination)
		if (a[1] > 15 || a[2] > 15 || a[3] > 2 || a[4] > 1 || m17_addr_get(a + 5) == 0) {
			return DL_ST_BAD_ARG;
		}
		radio_m17_settings_t s = { .can = a[1], .tx_can = a[2], .squelch = a[3], .mode1600 = a[4] != 0 };
		memcpy(s.dst, a + 5, sizeof s.dst);
		radio_req_m17_settings(&s);
		return DL_ST_OK;
	}
	case DL_M17_BERT_RUN:
	case DL_M17_BER_MODE:
		// u8: 0 off, 1 on; the run also 2: the counters start over
		if (a[1] > (a[0] == DL_M17_BERT_RUN ? 2 : 1)) {
			return DL_ST_BAD_ARG;
		}
#if CONFIG_M17_BERT
		radio_req_m17_bert(a[0] == DL_M17_BER_MODE ? (uint8_t)(RADIO_REQ_M17_BERT_MODE_OFF + a[1]) :
				   (uint8_t)(RADIO_REQ_M17_BERT_TX_OFF + a[1]));
		return DL_ST_OK;
#else
		return DL_ST_UNAVAILABLE;
#endif
	case DL_M17_SMS: {
		// dst 6 bytes, then 1.. bytes of UTF-8 (no NUL): one at a time, busy while the one before is on its way
		if (m17_addr_get(a + 1) == 0 || memchr(a + 7, 0, len - 7u) != NULL) {
			return DL_ST_BAD_ARG;
		}
#if CONFIG_M17_PACKET
		char text[DL_REQ_MAX];
		memcpy(text, a + 7, len - 7u);
		text[len - 7u] = '\0';
		return radio_req_m17_sms(m17_addr_get(a + 1), text) ? DL_ST_OK : DL_ST_BUSY;
#else
		return DL_ST_UNAVAILABLE;
#endif
	}
	case DL_M17_BENCH_SIG:
		// none: m17.md 7.5's vector signed and verified; m17.bench.ok, m17.sign_us, m17.verify_us show it
#if CONFIG_M17_SIG
		radio_req_m17_bench(RADIO_REQ_M17_BENCH_SIG, 0, 0);
		return DL_ST_OK;
#else
		return DL_ST_UNAVAILABLE;
#endif
	case DL_M17_BENCH_CODEC:
		// phase u8 (M17_VOICE_BENCH_*: 0 off, 1 encode, 2 decode), mode u8 (0 3200, 1 1600): m17v.enc_us_*, dec_us_*
		if (a[1] > M17_VOICE_BENCH_DEC || a[2] > 1) {
			return DL_ST_BAD_ARG;
		}
		radio_req_m17_bench(RADIO_REQ_M17_BENCH_CODEC, a[1], a[2]);
		return DL_ST_OK;
	case DL_M17_TIME: {
		// u32 Unix seconds, little endian, from a trusted host (UTC_SRC_HOST, aged from now); 0 forgets it
		uint32_t s = (uint32_t)a[1] | (uint32_t)a[2] << 8 | (uint32_t)a[3] << 16 | (uint32_t)a[4] << 24;
		if (s != 0 && s < UNIX_2020) {
			return DL_ST_BAD_ARG;
		}
		utc_set(s, hal_millis(), s != 0 ? UTC_SRC_HOST : UTC_SRC_NONE);
		return DL_ST_OK;
	}
	default:
		return DL_ST_BAD_ARG;
	}
#else
	(void)a;
	return DL_ST_UNAVAILABLE;
#endif
}
