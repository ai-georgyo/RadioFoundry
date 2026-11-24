// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's M17 sections and RADIO op 44 (core/debuglink/debuglink_view_m17.c; docs/spec/debuglink.md 3.7,
// 3.8) against a made-up radio core: every key of each part, the formats (addresses as text, words), each part fitting
// one GET_VIEW reply with every field at its widest, no secret ever printed although the state the view could reach
// holds them (the SMS and META text, the public key, the bench's d), and each sub-op's request with its lengths and
// ranges refused. Built three ways: every M17 part on (DLM17_ALL), M17 alone, M17 off (m17.built=0, the op
// unavailable).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_frame.h"
#include "radio/radio.h"
#if CONFIG_M17
#include "m17/m17.h"
#include "m17/m17_engine.h"
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
#if CONFIG_NET_M17
#include "net/m17_net.h"
#include "dmr/ipGateway.h"
#endif
#endif

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t now_ms = 5000000u;
uint32_t hal_millis(void) { return now_ms; }

#if CONFIG_M17
// The secrets the state holds: the m17.md 7.5 bench key d, a META and an SMS text. None may reach the view
static const char d_hex[] = "3F9BC000FAB83AED4551EC40A5F6A7C1";
#define SECRET_TEXT	"SECRET-TEXT-17"

// --- The radio core as the view and the op use it ---
static radio_m17_settings_t f_set, f_run;
static bool f_running;
static radio_m17_details_t f_det;
static bool f_has_det;
static radio_m17_key_t f_key;
static radio_m17_stats_t f_stats;
static m17_voice_stats_t f_voice;
static uint8_t f_bench = RADIO_M17_BENCH_NONE;
static uint32_t f_fit = 0x8003u;
static int f_resets;

uint32_t radio_m17_fit(void) { return f_fit; }
void radio_m17_settings(radio_m17_settings_t *out) { *out = f_set; }
bool radio_m17_in_force(radio_m17_settings_t *out)
{
	if (f_running) {
		*out = f_run;
	}
	return f_running;
}
bool radio_m17_details(radio_m17_details_t *out)
{
	*out = f_det;
	return f_has_det;
}
bool radio_m17_key(radio_m17_key_t *out)
{
	*out = f_key;
	return CONFIG_M17_SIG;
}
void radio_m17_stats(radio_m17_stats_t *out) { *out = f_stats; }
void radio_m17_reset_stats(void) { f_resets++; }
void m17_voice_stats(m17_voice_stats_t *out) { *out = f_voice; }
void m17_voice_reset_stats(void) { f_resets++; }
uint8_t radio_m17_bench_result(void) { return f_bench; }
const char *radio_mode_key(uint8_t mode)
{
	return mode == RADIO_MODE_M17 ? "m17" : mode == RADIO_MODE_DMR ? "dmr" : "?";
}

// The requests the op makes, as the radio task would take them
static radio_m17_settings_t r_set;
static int r_set_n, r_bert_n, r_bench_n, r_sms_n, r_utc_n;
static uint8_t r_bench_what, r_bench_phase, r_bench_mode, r_utc_src;
static uint32_t r_utc_s, r_utc_ms;
void radio_req_m17_settings(const radio_m17_settings_t *s)
{
	r_set = *s;
	r_set_n++;
}
void radio_req_m17_bench(uint8_t what, uint8_t phase, uint8_t mode)
{
	r_bench_what = what;
	r_bench_phase = phase;
	r_bench_mode = mode;
	r_bench_n++;
}
void utc_set(uint32_t unix_s, uint32_t ms, uint8_t src)
{
	r_utc_s = unix_s;
	r_utc_ms = ms;
	r_utc_src = src;
	r_utc_n++;
}

// The UTC source: one, aged f_utc_age_ms (position_utc_now's contract: true while its age fits max_age_ms)
static uint8_t f_utc_src = UTC_SRC_NONE;
static uint32_t f_utc_age_ms, f_utc_unix;
bool position_utc_now(uint32_t now, uint32_t max_age_ms, uint32_t *unix_s, uint8_t *src)
{
	(void)now;
	bool ok = f_utc_src != UTC_SRC_NONE && f_utc_age_ms <= max_age_ms;
	if (src != NULL) {
		*src = ok ? f_utc_src : UTC_SRC_NONE;
	}
	if (ok && unix_s != NULL) {
		*unix_s = f_utc_unix + f_utc_age_ms / 1000u;
	}
	return ok;
}

#if CONFIG_M17_PACKET
static uint64_t r_sms_dst;
static char r_sms_text[200];
static bool r_sms_ok = true;
static uint8_t f_sms_n = 4, f_sms_unread = 2, f_sms_result = RADIO_M17_SMS_SENT, f_sms_seq = 7;
uint8_t radio_m17_sms_count(void) { return f_sms_n; }
uint8_t radio_m17_sms_unread(void) { return f_sms_unread; }
uint8_t radio_m17_sms_result(uint8_t *seq, uint8_t to[6])
{
	if (seq != NULL) {
		*seq = f_sms_seq;
	}
	if (to != NULL) {
		memset(to, 0xFF, 6);
	}
	return f_sms_result;
}
// The inbox holds a text: the view must never read it out
bool radio_m17_sms_get(uint8_t i, radio_m17_sms_t *out)
{
	(void)i;
	memset(out, 0, sizeof *out);
	strcpy(out->text, SECRET_TEXT);
	return true;
}
bool radio_req_m17_sms(uint64_t dst, const char *utf8)
{
	r_sms_dst = dst;
	snprintf(r_sms_text, sizeof r_sms_text, "%s", utf8);
	r_sms_n++;
	return r_sms_ok;
}
#endif
#if CONFIG_M17_BERT
static uint8_t r_bert;
static radio_m17_bert_t f_bert;
void radio_m17_bert_get(radio_m17_bert_t *out) { *out = f_bert; }
void radio_req_m17_bert(uint8_t cmd)
{
	r_bert = cmd;
	r_bert_n++;
}
#endif
#if CONFIG_HEARD_LIST
static radio_heard_t f_heard[RADIO_HEARD_MAX];
static uint8_t f_heard_n;
uint8_t radio_heard_count(void) { return f_heard_n; }
bool radio_heard_get(uint8_t i, radio_heard_t *out)
{
	if (i >= f_heard_n) {
		return false;
	}
	*out = f_heard[i];
	return true;
}
#endif
#if CONFIG_ENTROPY
static uint8_t f_ent_src = ENTROPY_SRC_POOL;
static uint32_t f_ent_bits = 300;
uint8_t entropy_source(void) { return f_ent_src; }
uint32_t entropy_credited(void) { return f_ent_bits; }
#endif
#endif

// --- Helpers ---
static radio_view_t view;
static char text[DL_REPLY_MAX];

// One part as cmd_get_view formats it: the room after status, uptime and next, less the truncation mark's
static bool part(uint8_t p)
{
	dl_text_t t;
	dl_text_init(&t, text, DL_REPLY_MAX - 6u - 16u);
	debuglink_view_m17(p, &view, &t);
	text[t.len] = '\0';
	if (getenv("DLM17_DUMP") != NULL) {
		printf("--- part %u%s\n%s", p, t.full ? " (full)" : "", text);
	}
	return !t.full;
}

#if CONFIG_M17
// The value of key in text ("key=value\n"), NULL when absent
static const char *val(const char *key)
{
	static char v[128];
	size_t n = strlen(key);
	for (const char *l = text; *l != '\0';) {
		const char *e = strchr(l, '\n');
		size_t len = e != NULL ? (size_t)(e - l) : strlen(l);
		if (len > n && strncmp(l, key, n) == 0 && l[n] == '=') {
			size_t m = len - n - 1 < sizeof v - 1 ? len - n - 1 : sizeof v - 1;
			memcpy(v, l + n + 1, m);
			v[m] = '\0';
			return v;
		}
		l += len + (e != NULL);
	}
	return NULL;
}

static bool is(const char *key, const char *want)
{
	const char *v = val(key);
	if (v == NULL || strcmp(v, want) != 0) {
		printf("  %s=%s, want %s\n", key, v != NULL ? v : "(absent)", want);
		return false;
	}
	return true;
}

// Every line is key=value, keys unique within the part
static bool well_formed(void)
{
	char keys[200][48];
	int n = 0;
	for (const char *l = text; *l != '\0';) {
		const char *e = strchr(l, '\n'), *eq = strchr(l, '=');
		if (e == NULL || eq == NULL || eq > e || eq == l || (size_t)(eq - l) >= sizeof keys[0] || n == 200) {
			return false;
		}
		memcpy(keys[n], l, (size_t)(eq - l));
		keys[n][eq - l] = '\0';
		for (int i = 0; i < n; i++) {
			if (strcmp(keys[i], keys[n]) == 0) {
				printf("  key %s twice\n", keys[n]);
				return false;
			}
		}
		n++;
		l = e + 1;
	}
	return true;
}

// No secret: no SMS or META text, no hex of the public key or the bench's d, in either case
static bool no_secret(const char *pub_hex)
{
	char low[sizeof d_hex];
	for (size_t i = 0; i < sizeof d_hex; i++) {
		low[i] = (char)(d_hex[i] >= 'A' && d_hex[i] <= 'F' ? d_hex[i] + 32 : d_hex[i]);
	}
	return strstr(text, "SECRET") == NULL && strstr(text, d_hex) == NULL && strstr(text, low) == NULL &&
	       (pub_hex == NULL || strstr(text, pub_hex) == NULL);
}

static uint8_t op(const uint8_t *a, uint16_t len)
{
	return debuglink_m17_op(a, len);
}

static void put_addr(uint8_t a[6], uint64_t v)
{
	m17_addr_put(a, v);
}

// The state at its widest: every counter all ones, the longest texts, every list full
static void wide_state(void)
{
	memset(&f_stats, 0xFF, sizeof f_stats);
	memset(&f_voice, 0xFF, sizeof f_voice);
	memset(&view.m17, 0xFF, sizeof view.m17);
	view.m17.flags = 0xFFFFFFFFu;
	view.m17.rssi_dbm = -32768;
	view.m17.why = 0xFF;			// past the names: "?"
	put_addr(view.m17.src, 0xEE6B28000200ull);	// extended addresses: 12 hex digits, the widest text
	put_addr(view.m17.dst, 0xEE6B28000000ull);
	put_addr(view.m17.orig, 0xF00000000000ull);
	put_addr(view.m17.via, 0xFFFFFFFFFFFEull);
	put_addr(view.m17.talk, 0xEE6B28000001ull);
	put_addr(view.m17.own, 0xEE6B28000002ull);
	view.tx_deny = 0xFF;
	f_set.can = f_set.tx_can = f_set.squelch = 0xFF;
	f_set.mode1600 = true;
	put_addr(f_set.dst, 0xEE6B28000003ull);
	f_run = f_set;
	f_running = true;
	memset(&f_det, 0xFF, sizeof f_det);
	f_det.played = f_det.gnss = f_det.alt_ok = f_det.vel_ok = f_det.packet = true;
	strcpy(f_det.text, SECRET_TEXT);
	put_addr(f_det.signer, 0xEE6B28000004ull);
	f_det.sig_fresh = RADIO_M17_FRESH_NO;
	f_has_det = true;
	f_key.present = f_key.checked = f_key.damaged = true;
	f_bench = 0xFF;
	f_utc_src = UTC_SRC_HOST;
	f_utc_age_ms = UINT32_MAX;
	f_utc_unix = 0xFFFF0000u;
	f_fit = 0xFFFFFFFFu;
#if CONFIG_M17_BERT
	memset(&f_bert, 0xFF, sizeof f_bert);
	f_bert.rssi_dbm = -32768;
#endif
#if CONFIG_HEARD_LIST
	f_heard_n = RADIO_HEARD_MAX;
	for (unsigned i = 0; i < RADIO_HEARD_MAX; i++) {
		memset(&f_heard[i], 0xFF, sizeof f_heard[i]);
		f_heard[i].mode = RADIO_MODE_M17;
		f_heard[i].rssi_dbm = -32768;
		f_heard[i].last_ms = now_ms + 1001u;	// past the window for a stamp a little ahead: the widest age
		put_addr(f_heard[i].addr, 0xEE6B28000005ull + i);
		put_addr(f_heard[i].to, 0xEE6B28000100ull + i);
	}
#endif
#if CONFIG_ENTROPY
	f_ent_bits = UINT32_MAX;
#endif
#if CONFIG_M17_PACKET
	f_sms_n = f_sms_unread = f_sms_seq = 0xFF;
#endif
}

// A plain state: one call heard, settings, a key, a fresh host time
static void plain_state(void)
{
	memset(&f_stats, 0, sizeof f_stats);
	memset(&f_voice, 0, sizeof f_voice);
	memset(&view, 0, sizeof view);
	view.m17.flags = RADIO_M17_ON | RADIO_M17_RX | RADIO_M17_PLAYED | RADIO_M17_LSF | RADIO_M17_1600 |
			 RADIO_M17_BERT_MODE | RADIO_M17_TEXT;
	view.m17.can = 3;
	view.m17.rx_can = M17_CAN_ANY;
	view.m17.sig = RADIO_M17_SIG_SIGNER_DIFFERS;
	view.m17.why = M17_WHY_ADDRESSED;
	view.m17.rssi_dbm = -97;
	view.m17.tot_s = 180;
	view.m17.enc = M17_ENC_SCRAMBLER << 4 | M17_SCR_16;
	uint64_t a;
	CHECK(m17_addr_from_text("N0CALL", &a));
	put_addr(view.m17.src, a);
	put_addr(view.m17.orig, a);
	put_addr(view.m17.dst, M17_ADDR_BROADCAST);
	put_addr(view.m17.talk, 0xEE6B28123456ull);	// extended: hex
	CHECK(m17_addr_from_text("M17-REF A", &a));
	put_addr(view.m17.via, a);
	f_set = (radio_m17_settings_t){ .can = 5, .tx_can = 6, .squelch = M17_SQUELCH_ADDRESSED, .mode1600 = false };
	put_addr(f_set.dst, M17_ADDR_BROADCAST);
	f_running = false;
	memset(&f_det, 0, sizeof f_det);
	strcpy(f_det.text, SECRET_TEXT);
	f_det.sig_ts = 12345678;
	f_det.sig_fresh = RADIO_M17_FRESH_YES;
	f_has_det = true;
	memset(&f_key, 0, sizeof f_key);
	f_key.present = true;
	for (unsigned i = 0; i < sizeof f_key.q; i++) {
		f_key.q[i] = (uint8_t)(0xC0 + (i & 0x0F));
	}
	memcpy(f_key.fp, "\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8);
	f_bench = RADIO_M17_BENCH_OK;
	f_stats.sign_us = 81234;
	f_stats.verify_us = 85000;
	f_stats.begins = 2;
	f_stats.tx_syms = 4242;
	f_stats.sig_tail = 5;
	f_stats.nonce_refused = 0;
	f_stats.mem_lent = 1;
	f_stats.pkt_rx = 9;
	f_stats.sms_busy = 4;
	f_stats.sig_valid = 11;
	f_stats.key_damaged = 1;
	f_voice.rx_peak = 12000;
	f_voice.enc_us_max = 4321;
	f_voice.lent = 3;
	f_utc_src = UTC_SRC_HOST;
	f_utc_age_ms = 12999;
	f_utc_unix = 1700000000u;
	f_fit = 0x8003u;
#if CONFIG_M17_BERT
	memset(&f_bert, 0, sizeof f_bert);
	f_bert.state = RADIO_M17_BERT_LOCKED;
	f_bert.bits = 1000000;
	f_bert.errs = 250;
	f_bert.frames = 4049;
	f_bert.tx_frames = 77;
#endif
#if CONFIG_HEARD_LIST
	f_heard_n = 2;
	memset(f_heard, 0, sizeof f_heard);
	f_heard[0] = (radio_heard_t){ .mode = RADIO_MODE_M17, .flags = RADIO_HEARD_ADDR | RADIO_HEARD_SIG_OK, .count = 3,
				      .rssi_dbm = -80, .last_ms = now_ms - 12500u };
	memcpy(f_heard[0].addr, view.m17.src, 6);
	put_addr(f_heard[0].to, M17_ADDR_BROADCAST);
	f_heard[1] = (radio_heard_t){ .mode = RADIO_MODE_DMR, .id = 3100001, .count = 1, .last_ms = now_ms };
#endif
#if CONFIG_ENTROPY
	f_ent_src = ENTROPY_SRC_TRNG;
	f_ent_bits = 512;
#endif
#if CONFIG_M17_PACKET
	f_sms_n = 4;
	f_sms_unread = 2;
	f_sms_result = RADIO_M17_SMS_BUSY;
	f_sms_seq = 7;
#endif
}

static void test_keys(void)
{
	char pub[2 * sizeof f_key.q + 1];
	plain_state();
	for (unsigned i = 0; i < sizeof f_key.q; i++) {
		snprintf(pub + 2 * i, 3, "%02X", f_key.q[i]);
	}

	CHECK(part(0) && well_formed() && no_secret(pub));
	CHECK(is("m17.built", "1"));
	CHECK(is("m17.on", "1") && is("m17.rx", "1") && is("m17.played", "1") && is("m17.late", "0") &&
	      is("m17.lsf", "1") && is("m17.text", "1") && is("m17.gnss", "0") && is("m17.packet", "0") &&
	      is("m17.bert", "0") && is("m17.bert_mode", "1") && is("m17.no_call", "0") && is("m17.call_bad", "0"));
	CHECK(is("m17.tx_sign", "0") && is("m17.tx_enc", "0"));
	CHECK(is("m17.flags", "0x800E7") && val("m17.details") == NULL);
	CHECK(val("m17.-") == NULL);
	CHECK(is("m17.type", "stream") && is("m17.voice", "1600") && is("m17.can", "3") && is("m17.rx_can", "255"));
	CHECK(is("m17.enc", "17") && is("m17.rssi_dbm", "-97") && is("m17.tot_s", "180"));
	CHECK(is("m17.src", "N0CALL") && is("m17.orig", "N0CALL") && is("m17.dst", "ALL") && is("m17.via", "M17-REF A"));
	CHECK(is("m17.talk", "EE6B28123456") && is("m17.call", "-"));
	CHECK(is("m17.ext1", "0") && is("m17.ext2", "1"));
	CHECK(is("m17.why", "addressed") && is("m17.sig", "signer_differs"));
	CHECK(is("m17.fit", "32771") && is("m17.tx", "0") && is("m17.tx_phase", "0") && is("m17.deny", "0"));
	CHECK(is("m17.set.can", "5") && is("m17.set.tx_can", "6") && is("m17.set.squelch", "2") &&
	      is("m17.set.mode1600", "0") && is("m17.set.dst", "ALL"));
	CHECK(val("m17.run.can") == NULL);	// not running: nothing in force
#if CONFIG_M17_SIG
	CHECK(is("m17.key", "unchecked") && is("m17.key_fp", "0123456789ABCDEF"));
	CHECK(part(1) && is("m17.bench.ok", "ok") && is("m17.sign_us", "81234") && is("m17.verify_us", "85000"));
#else
	CHECK(val("m17.key") == NULL);
	CHECK(part(1) && val("m17.bench.ok") == NULL);
#endif
	CHECK(part(1) && is("time.src", "host") && is("time.age_s", "12") && is("time.unix", "1700000012"));

	// In force, an extended callsign naming another originator
	f_running = true;
	f_run = f_set;
	f_run.can = 9;
	f_run.mode1600 = true;
	put_addr(f_run.dst, 0xEE6B28000000ull);
	put_addr(view.m17.via, m17_addr_get(view.m17.src));
	put_addr(view.m17.orig, 1);	// "A" is base-40 1... any other standard address
	view.m17.flags = RADIO_M17_PACKET | RADIO_M17_DATA;
	f_det.packet = true;
	f_det.pkt_type = 5;
	f_det.pkt_len = 33;
	f_key.checked = true;
	f_key.damaged = true;
	CHECK(part(0) && well_formed() && no_secret(pub));
	CHECK(is("m17.run.can", "9") && is("m17.run.mode1600", "1") && is("m17.run.dst", "EE6B28000000"));
	CHECK(is("m17.type", "packet") && is("m17.voice", "0") && is("m17.ext1", "1") && is("m17.ext2", "0"));
	CHECK(part(2) && is("m17.pkt_type", "5") && is("m17.pkt_len", "33"));
	// Nothing that carries voice says 3200: a packet, a BERT run
	view.m17.flags = RADIO_M17_PACKET;
	CHECK(part(0) && is("m17.voice", "0") && is("m17.type", "packet"));
	view.m17.flags = RADIO_M17_BERT;
	CHECK(part(0) && is("m17.voice", "0") && is("m17.type", "bert"));
	view.m17.flags = 0;
	CHECK(part(0) && is("m17.voice", "3200") && is("m17.type", "stream"));
	// What PTT sends, by name: signed, encrypted (a test that keys up checks them before it does)
	view.m17.flags = RADIO_M17_TX_SIGN;
	CHECK(part(0) && is("m17.tx_sign", "1") && is("m17.tx_enc", "0") && is("m17.flags", "0x20000"));
	view.m17.flags = RADIO_M17_TX_ENC;
	CHECK(part(0) && is("m17.tx_sign", "0") && is("m17.tx_enc", "1") && is("m17.flags", "0x40000"));
	view.m17.flags = 0;
#if CONFIG_M17_SIG
	CHECK(part(0) && is("m17.key", "damaged"));
	f_key.damaged = false;
	CHECK(part(0) && is("m17.key", "ok"));
	f_key.present = false;
	CHECK(part(0) && is("m17.key", "none") && is("m17.key_fp", "0000000000000000"));
#endif
	f_has_det = false;
	CHECK(part(2) && val("m17.pkt_type") == NULL);

	// The time's age to the second, whatever the source's age; none
	static const uint32_t ages[] = { 0, 999, 1000, 59999, 60000, 3600123, 4000000000u };
	for (unsigned i = 0; i < sizeof ages / sizeof ages[0]; i++) {
		char want[16];
		f_utc_age_ms = ages[i];
		snprintf(want, sizeof want, "%u", ages[i] / 1000u);
		CHECK(part(1) && is("time.age_s", want) && is("time.src", "host"));
	}
	f_utc_src = UTC_SRC_GNSS;
	f_utc_age_ms = 1500;
	CHECK(part(1) && is("time.src", "gnss") && is("time.age_s", "1"));
	f_utc_src = UTC_SRC_NONE;
	CHECK(part(1) && is("time.src", "none") && is("time.age_s", "0") && is("time.unix", "0"));

	// Part 1: the engine's and the session's counters
	plain_state();
	CHECK(part(1) && well_formed() && no_secret(pub));
	CHECK(is("m17.built", "1") && is("m17.c.begins", "2") && is("m17.c.tx_syms", "4242"));
	CHECK(is("m17.c.sig_tail", "5") && is("m17.c.nonce_refused", "0") && is("m17.c.mem_lent", "1"));
	static const char *const c1[] = { "begin_fails", "rx_calls", "tx_calls", "tx_filler", "frames", "lsf_ok",
		"lsf_bad", "lich_ok", "lich_bad", "stream", "packet", "bert", "fec_bits", "can_rejects", "eot",
		"head_rejects", "enc_skipped", "expect_miss", "fn_limit", "rx_chunks", "tx_underruns" };
	for (unsigned i = 0; i < sizeof c1 / sizeof c1[0]; i++) {
		char k[48];
		snprintf(k, sizeof k, "m17.c.%s", c1[i]);
		CHECK(is(k, "0"));
	}

	// Part 2: the last call's packet and signature, packets and SMS (counts), signatures
	CHECK(part(2) && well_formed() && no_secret(pub));
	CHECK(is("m17.pkt_type", "0") && is("m17.pkt_len", "0"));
#if CONFIG_M17_SIG
	CHECK(is("m17.signer", "-") && is("m17.sig_ts", "12345678") && is("m17.sig_fresh", "yes"));
#else
	CHECK(val("m17.signer") == NULL && val("m17.sig_fresh") == NULL);
#endif
#if CONFIG_M17_PACKET
	CHECK(is("m17.c.pkt_rx", "9") && is("m17.c.sms_busy", "4") && is("m17.c.pkt_dropped", "0"));
	CHECK(is("sms.n", "4") && is("sms.unread", "2") && is("sms.result", "busy") && is("sms.result_seq", "7"));
#else
	CHECK(val("sms.n") == NULL && val("m17.c.pkt_rx") == NULL);
#endif
#if CONFIG_M17_SIG
	CHECK(is("m17.c.sig_valid", "11") && is("m17.c.key_damaged", "1") && is("m17.c.sig_keys_capped", "0"));
#else
	CHECK(val("m17.c.sig_valid") == NULL);
#endif
#if CONFIG_ENTROPY
	CHECK(part(1) && is("crypto.entropy", "trng") && is("crypto.ent_bits", "512") && is("crypto.nonce_ok", "1"));
	f_stats.nonce_refused = 1;
	f_ent_src = ENTROPY_SRC_WEAK;
	CHECK(part(1) && is("crypto.nonce_ok", "0") && is("crypto.entropy", "weak"));
#else
	CHECK(part(1) && val("crypto.entropy") == NULL);
#endif

	// Part 4: the heard list
	CHECK(part(4) && well_formed() && no_secret(pub));
#if CONFIG_HEARD_LIST
	CHECK(is("heard.built", "1") && is("heard.n", "2") && is("heard.0", "m17 N0CALL ALL 3 -80 12 0x03") &&
	      is("heard.1", "dmr 3100001 - 1 0 0 0x00") && val("heard.2") == NULL);
	// An entry the radio task stamped just after the view read the clock: age 0, not 49 days
	f_heard[1].last_ms = now_ms + 900u;
	CHECK(part(4) && is("heard.1", "dmr 3100001 - 1 0 0 0x00"));
	f_heard[1].last_ms = now_ms - 3600000u;
	CHECK(part(4) && is("heard.1", "dmr 3100001 - 1 0 3600 0x00"));
	// A callsign with a space inside (a reflector's module): one word, the space as '_', so the record keeps its
	// seven words
	uint64_t refl, spaced;
	CHECK(m17_addr_from_text("M17-M17 C", &refl) && m17_addr_from_text("AB 1", &spaced));
	put_addr(f_heard[0].to, refl);
	CHECK(part(4) && is("heard.0", "m17 N0CALL M17-M17_C 3 -80 12 0x03"));
	put_addr(f_heard[0].addr, spaced);
	CHECK(part(4) && is("heard.0", "m17 AB_1 M17-M17_C 3 -80 12 0x03"));
	memcpy(f_heard[0].addr, view.m17.src, 6);
	put_addr(f_heard[0].to, M17_ADDR_BROADCAST);
#else
	CHECK(is("heard.built", "0") && val("heard.n") == NULL);
#endif

	// Part 3: the voice path and the BER test
	CHECK(part(3) && well_formed() && no_secret(pub));
	CHECK(is("m17v.rx_peak", "12000") && is("m17v.enc_us_max", "4321") && is("m17v.lent", "3") &&
	      is("m17v.dec_us_max", "0") && is("m17v.rx_in", "0") && is("m17v.dec_blocks", "0"));
#if CONFIG_M17_BERT
	CHECK(is("m17b.state", "locked") && is("m17b.ber_ppm", "250") && is("m17b.frames", "4049") &&
	      is("m17b.bits", "1000000") && is("m17b.errs", "250") && is("m17b.tx_frames", "77") &&
	      is("m17b.locks", "0") && is("m17b.losses", "0"));
	f_bert.errs = 3000000000u;	// past what errs * 10^6 holds in 32 bits
	f_bert.bits = 4000000000u;
	CHECK(part(3) && is("m17b.ber_ppm", "750000"));
	f_bert.bits = 0;
	CHECK(part(3) && is("m17b.ber_ppm", "0"));
#else
	CHECK(val("m17b.state") == NULL);
#endif

	// RESET_COUNTERS: the radio core's and the voice path's
	f_resets = 0;
	debuglink_m17_reset();
	CHECK(f_resets == 2);
}

// Every part fits one reply with every field at its widest: nothing cut off by truncated=1
static void test_widest(void)
{
	char pub[2 * sizeof f_key.q + 1];
	wide_state();
	memset(f_key.q, 0x5A, sizeof f_key.q);
	for (unsigned i = 0; i < sizeof f_key.q; i++) {
		snprintf(pub + 2 * i, 3, "%02X", f_key.q[i]);
	}
	for (uint8_t p = 0; p < 5; p++) {
		bool fits = part(p);
		printf("part %u at its widest: %u bytes of %u\n", p, (unsigned)strlen(text), DL_REPLY_MAX - 22u);
		CHECK(fits && well_formed() && no_secret(pub));
	}
	CHECK(part(0) && is("m17.why", "?") && is("m17.sig", "?") && is("m17.rssi_dbm", "-32768"));
	CHECK(is("m17.dst", "EE6B28000000") && is("m17.via", "FFFFFFFFFFFE") && is("m17.src", "EE6B28000200"));
}

static void test_op(void)
{
	// Settings: exact length, ranges, no zero destination; broadcast all 0xFF
	uint8_t s[] = { DL_M17_SETTINGS, 15, 0, 2, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	r_set_n = 0;
	CHECK(op(s, sizeof s) == DL_ST_OK && r_set_n == 1);
	CHECK(r_set.can == 15 && r_set.tx_can == 0 && r_set.squelch == 2 && r_set.mode1600 &&
	      m17_addr_get(r_set.dst) == M17_ADDR_BROADCAST);
	CHECK(op(s, sizeof s - 1) == DL_ST_BAD_ARG);
	uint8_t s2[sizeof s + 1];
	memcpy(s2, s, sizeof s);
	s2[sizeof s] = 0;
	CHECK(op(s2, sizeof s2) == DL_ST_BAD_ARG);
	for (unsigned i = 1; i <= 4; i++) {
		uint8_t b[sizeof s];
		memcpy(b, s, sizeof s);
		b[i] = i <= 2 ? 16 : i == 3 ? 3 : 2;
		CHECK(op(b, sizeof b) == DL_ST_BAD_ARG);
	}
	memset(s + 5, 0, 6);
	CHECK(op(s, sizeof s) == DL_ST_BAD_ARG);
	s[10] = 1;
	CHECK(op(s, sizeof s) == DL_ST_OK && m17_addr_get(r_set.dst) == 1);
	CHECK(r_set_n == 2);

	// The benches
	uint8_t bs[] = { DL_M17_BENCH_SIG, 0 };
	r_bench_n = 0;
#if CONFIG_M17_SIG
	CHECK(op(bs, 1) == DL_ST_OK && r_bench_n == 1 && r_bench_what == RADIO_REQ_M17_BENCH_SIG);
#else
	CHECK(op(bs, 1) == DL_ST_UNAVAILABLE && r_bench_n == 0);
#endif
	CHECK(op(bs, 2) == DL_ST_BAD_ARG);
	uint8_t bc[] = { DL_M17_BENCH_CODEC, M17_VOICE_BENCH_DEC, 1, 0 };
	r_bench_n = 0;
	CHECK(op(bc, 3) == DL_ST_OK && r_bench_n == 1 && r_bench_what == RADIO_REQ_M17_BENCH_CODEC &&
	      r_bench_phase == M17_VOICE_BENCH_DEC && r_bench_mode == 1);
	CHECK(op(bc, 2) == DL_ST_BAD_ARG && op(bc, 4) == DL_ST_BAD_ARG);
	bc[1] = 3;
	CHECK(op(bc, 3) == DL_ST_BAD_ARG);
	bc[1] = M17_VOICE_BENCH_OFF;
	bc[2] = 2;
	CHECK(op(bc, 3) == DL_ST_BAD_ARG);
	CHECK(r_bench_n == 1);

	// BERT run and BER test mode
	uint8_t br[] = { DL_M17_BERT_RUN, 1, 0 };
	r_bert_n = 0;
#if CONFIG_M17_BERT
	CHECK(op(br, 2) == DL_ST_OK && r_bert == RADIO_REQ_M17_BERT_TX_ON);
	br[1] = 0;
	CHECK(op(br, 2) == DL_ST_OK && r_bert == RADIO_REQ_M17_BERT_TX_OFF);
	br[1] = 2;
	CHECK(op(br, 2) == DL_ST_OK && r_bert == RADIO_REQ_M17_BERT_RESET);
	uint8_t bm[] = { DL_M17_BER_MODE, 1 };
	CHECK(op(bm, 2) == DL_ST_OK && r_bert == RADIO_REQ_M17_BERT_MODE_ON);
	bm[1] = 0;
	CHECK(op(bm, 2) == DL_ST_OK && r_bert == RADIO_REQ_M17_BERT_MODE_OFF);
	bm[1] = 2;
	CHECK(op(bm, 2) == DL_ST_BAD_ARG);
	CHECK(r_bert_n == 5);
#else
	CHECK(op(br, 2) == DL_ST_UNAVAILABLE);
	uint8_t bm[] = { DL_M17_BER_MODE, 1 };
	CHECK(op(bm, 2) == DL_ST_UNAVAILABLE);
#endif
	br[1] = 3;
	CHECK(op(br, 2) == DL_ST_BAD_ARG && op(br, 1) == DL_ST_BAD_ARG);
	br[1] = 1;
	CHECK(op(br, 3) == DL_ST_BAD_ARG);

	// SMS: dst, then the text (no NUL), the longest a request carries
	uint8_t m[DL_REQ_MAX - 1u] = { DL_M17_SMS, 0, 0, 0, 0, 0, 0x2A };
	memcpy(m + 7, "hello", 5);
	r_sms_n = 0;
#if CONFIG_M17_PACKET
	CHECK(op(m, 12) == DL_ST_OK && r_sms_n == 1 && r_sms_dst == 0x2A && strcmp(r_sms_text, "hello") == 0);
	memset(m + 7, 'x', sizeof m - 7);
	CHECK(op(m, sizeof m) == DL_ST_OK && strlen(r_sms_text) == sizeof m - 7);
	r_sms_ok = false;
	CHECK(op(m, 12) == DL_ST_BUSY);
	r_sms_ok = true;
	m[9] = 0;
	CHECK(op(m, 12) == DL_ST_BAD_ARG);
	CHECK(r_sms_n == 3);
#else
	CHECK(op(m, 12) == DL_ST_UNAVAILABLE && r_sms_n == 0);
#endif
	CHECK(op(m, 7) == DL_ST_BAD_ARG && op(m, 3) == DL_ST_BAD_ARG);
	m[6] = 0;
	m[9] = 'x';
	CHECK(op(m, 12) == DL_ST_BAD_ARG);	// no destination

	// The time: Unix seconds from 2020 on, or 0 to forget it
	uint8_t tm[] = { DL_M17_TIME, 0x00, 0xF1, 0x53, 0x65, 0 };	// 0x6553F100 = 1700000000
	r_utc_n = 0;
	CHECK(op(tm, 5) == DL_ST_OK && r_utc_n == 1 && r_utc_s == 1700000000u && r_utc_src == UTC_SRC_HOST &&
	      r_utc_ms == now_ms);
	CHECK(op(tm, 4) == DL_ST_BAD_ARG && op(tm, 6) == DL_ST_BAD_ARG);
	memset(tm + 1, 0, 4);
	CHECK(op(tm, 5) == DL_ST_OK && r_utc_s == 0 && r_utc_src == UTC_SRC_NONE);
	tm[1] = 1;	// 1 s after 1970: not a host's clock
	CHECK(op(tm, 5) == DL_ST_BAD_ARG);
	CHECK(r_utc_n == 2);

	// Nothing, an unknown sub-op
	CHECK(op(tm, 0) == DL_ST_BAD_ARG);
	uint8_t u[] = { 8, 0 };
	CHECK(op(u, 2) == DL_ST_BAD_ARG);
	u[0] = DL_M17_NET;	// The reflector's sub-op: unlink, or unavailable without the client
#if CONFIG_NET_M17
	CHECK(op(u, 2) == DL_ST_OK);
#else
	CHECK(op(u, 2) == DL_ST_UNAVAILABLE);
#endif
	u[0] = 0xFF;
	CHECK(op(u, 1) == DL_ST_BAD_ARG);
}
#endif

#if CONFIG_NET_M17
// ---- M17 reflector. Sub-op 7 with the client built (debuglinkM17TestNet): the real m17_net.c on a fake IP
// gateway, our callsign from a fake radio-wide record ----
static uint64_t f_own;
uint64_t m17_own_addr(bool *bad)
{
	if (bad != NULL)
		*bad = false;
	return f_own;
}
static uint8_t f_frame[IPGW_LOCAL_MAX];
static uint8_t f_sent[16];
static int f_sent_len;
uint8_t *ipGatewayLocalPayload(void) { return f_frame; }
bool ipGatewayUdpBind(uint16_t port, ipGatewayUdpHandler handler) { (void)port; (void)handler; return true; }
bool ipGatewayUdpSend(uint32_t ip, uint16_t sport, uint16_t dport, const uint8_t *p, int n)
{
	(void)ip; (void)sport; (void)dport;
	f_sent_len = n;
	memcpy(f_sent, p, (size_t)(n < (int)sizeof f_sent ? n : (int)sizeof f_sent));
	return true;
}

// a: the link request, ip 192.0.2.1, port, text
static uint16_t net_req(uint8_t *a, uint8_t kind, uint16_t port, const char *name)
{
	size_t n = strlen(name);
	a[0] = DL_M17_NET;
	a[1] = kind;
	a[2] = 192;
	a[3] = 0;
	a[4] = 2;
	a[5] = 1;
	a[6] = (uint8_t)port;
	a[7] = (uint8_t)(port >> 8);
	memcpy(a + 8, name, n);
	return (uint16_t)(8u + n);
}

static void test_net_op(void)
{
	uint8_t a[32];
	uint16_t n;
	m17_net_status_t s;

	m17_net_start();
	(void)m17_addr_from_text("N0CALL", &f_own);
	// Link: the target as given, our callsign, CONN at the next poll
	n = net_req(a, 1, 17001, "M17-XYZ C");
	CHECK(op(a, n) == DL_ST_OK);
	m17_net_poll(1000u, true);
	m17_net_status(&s);
	CHECK(s.state == M17_NET_CONNECTING && s.t.ip == 0xC0000201u && s.t.port == 17001 && strcmp(s.t.name, "M17-XYZ") == 0 &&
	      s.t.module == 'C' && !s.t.listen && s.t.own == f_own);
	CHECK(f_sent_len == 11 && memcmp(f_sent, "CONN", 4) == 0 && f_sent[10] == 'C');
	// Listen only, lower case and a 6-character designation; port 0: 17000
	n = net_req(a, 2, 0, "urfxyz  a");
	CHECK(op(a, n) == DL_ST_OK);
	m17_net_poll(1010u, true);
	m17_net_status(&s);
	CHECK(s.t.listen && strcmp(s.t.name, "URFXYZ") == 0 && s.t.module == 'A' && s.t.port == 0);
	CHECK(f_sent_len == 11 && memcmp(f_sent, "LSTN", 4) == 0);
	// Unlink: 2 bytes exactly
	a[1] = 0;
	CHECK(op(a, 2) == DL_ST_OK);
	CHECK(op(a, 3) == DL_ST_BAD_ARG);
	m17_net_poll(1020u, true);
	CHECK(m17_net_state() == M17_NET_UNLINKING && f_sent_len == 10 && memcmp(f_sent, "DISC", 4) == 0);
	// Refusals: a kind not known, a text too short or too long or with a NUL, no module, an address that isn't
	// unicast, no callsign
	n = net_req(a, 3, 0, "M17-XYZ C");
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "A ");
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "M17-XYZ  C");
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "M17-XYZ C");
	a[10] = 0;
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "M17-XYZC");
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "M17-XYZ C");
	a[2] = 127;
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	a[2] = 224;
	CHECK(op(a, n) == DL_ST_BAD_ARG);
	n = net_req(a, 1, 0, "M17-XYZ C");
	f_own = 0;
	CHECK(op(a, n) == DL_ST_REFUSED);
	m17_net_poll(1030u, true);
	CHECK(m17_net_state() == M17_NET_UNLINKING);	// none of them reached the client
}
#endif

int main(void)
{
	CHECK(debuglink_is_feature_op(DL_OP_M17));
	CHECK(DL_VIEW_HEARD == DL_VIEW_M17 + 4 && DL_VIEW_M17_NET == DL_VIEW_HEARD + 1 && DL_VIEW_COUNT == DL_VIEW_M17_NET + 1);
#if CONFIG_M17
	test_keys();
	test_widest();
	test_op();
#if CONFIG_NET_M17
	test_net_op();
#endif
	printf("debuglinkM17Test (packet %d, bert %d, sig %d, heard %d, entropy %d): %s\n", CONFIG_M17_PACKET,
	       CONFIG_M17_BERT, CONFIG_M17_SIG, CONFIG_HEARD_LIST, CONFIG_ENTROPY, failures ? "FAILED" : "ok");
#else
	for (uint8_t p = 0; p < 4; p++) {
		CHECK(part(p) && strcmp(text, "m17.built=0\n") == 0);
	}
	CHECK(part(4) && strcmp(text, "heard.built=0\n") == 0);
	uint8_t a[] = { DL_M17_TIME, 0, 0, 0, 0 };
	CHECK(debuglink_m17_op(a, sizeof a) == DL_ST_UNAVAILABLE && debuglink_m17_op(a, 0) == DL_ST_BAD_ARG);
	debuglink_m17_reset();
	printf("debuglinkM17Test (M17 off): %s\n", failures ? "FAILED" : "ok");
#endif
	return failures ? 1 : 0;
}
