// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17's own pages (docs/ux/m17.md 6, 13, 14): Call Details (the last call in full; a page any mode could fill, M17 is
// its first), the signing key and a station's key, the station keys, the BER test. And the pieces the shared screens
// read through the UI's scratch (ui_scratch): the last call in full and the radio-wide record, too big for the UI
// task's stack. No key's secret part is ever shown: the pages show public keys and fingerprints only. Built with
// CONFIG_M17; each sub-feature's page with its switch
#include <string.h>
#include "ui_nav.h"
#include "ui_m17.h"
#include "config.h"

#if CONFIG_M17
#include "m17/m17_codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "m17/m17_engine.h"
#include "m17/m17_packet.h"	// a packet's data types (the details name them)
#include "position/position_geo.h"
#include "position/position_utc.h"
#include "hal_time.h"
#if CONFIG_M17_SIG
#include "m17/m17_sig.h"
#endif

// The scratch's layout: the page text first, then the radio-wide record, then the last call in full (8-aligned)
#define M17_DET_AT(cap)		((cap) - ((sizeof(radio_m17_details_t) + 7u) & ~7u))
#define M17_WIDE_AT(cap)	(M17_DET_AT(cap) - ((sizeof(m17_wide_t) + 7u) & ~7u))

const radio_m17_details_t *ui_m17_details(void)
{
	size_t cap;
	char *s = ui_scratch(&cap);
	radio_m17_details_t *d = (radio_m17_details_t *)(void *)(s + M17_DET_AT(cap));
	return radio_m17_details(d) ? d : NULL;
}

m17_wide_t *ui_m17_wide(void)
{
	size_t cap;
	char *s = ui_scratch(&cap);
	m17_wide_t *w = (m17_wide_t *)(void *)(s + M17_WIDE_AT(cap));
	m17_wide_load(w);
	return w;
}

// The page text's buffer: what comes before the records
static void page_init(sbuf_t *b)
{
	size_t cap;
	char *s = ui_scratch(&cap);
	sb_init(b, s, M17_WIDE_AT(cap));
}

static const char *page_text(void)
{
	size_t cap;
	return ui_scratch(&cap);
}

// "Label: " then the value's start (the text page shows the label dim)
static void line(sbuf_t *b, const char *label)
{
	sb_chr(b, '\n');
	sb_str(b, label);
	sb_str(b, ": ");
}

#if CONFIG_M17_SIG
// Hex digits of bytes in groups of 4, groups apart (a fingerprint, a public key)
static void sb_hex_groups(sbuf_t *b, const uint8_t *p, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		if (i && (i & 1u) == 0)
			sb_chr(b, ' ');
		sb_hex(b, p[i], 2);
	}
}
#endif

// The text pages' scroll (Call Details, a key, a heard station: one is open at a time): the first line shown, the
// largest (from the last draw)
uint16_t ui_m17_page_first, ui_m17_page_max;

// --- Call Details (6): the last M17 call heard, a text page ---

// The encryption a call came with (M17_ENC_* << 4 | subtype): "None", "{Scrambler 16-bit|Scrambler 16}", "AES-256";
// in a build without encryption, only that there was some
static void sb_m17_enc(sbuf_t *b, uint8_t enc)
{
#if CONFIG_M17_SCRAMBLER || CONFIG_M17_AES
	static const tr_id_t scr[3] = {
		TRN(CHAN_VALUE_SCRAMBLER_8, TEXT_ALT_OPEN "Scrambler 8-bit" TEXT_ALT_NEXT "Scrambler 8" TEXT_ALT_CLOSE),
		TRN(CHAN_VALUE_SCRAMBLER_16, TEXT_ALT_OPEN "Scrambler 16-bit" TEXT_ALT_NEXT "Scrambler 16" TEXT_ALT_CLOSE),
		TRN(CHAN_VALUE_SCRAMBLER_24, TEXT_ALT_OPEN "Scrambler 24-bit" TEXT_ALT_NEXT "Scrambler 24" TEXT_ALT_CLOSE) };
	static const tr_id_t aes[3] = { TRX(VALUE_AES_128, "AES-128"), TRX(VALUE_AES_192, "AES-192"),
		TRX(VALUE_AES_256, "AES-256") };
	uint8_t type = (uint8_t)(enc >> 4), sub = (uint8_t)(enc & 3u);
	char t[24];
	bool more;
	sub = sub > 2 ? 2 : sub;
	gfx_text_alt(type == M17_ENC_SCRAMBLER ? tr(scr[sub]) : type == M17_ENC_AES ? tr(aes[sub]) :
		TR(VALUE_NONE, "None"), 0, t, sizeof t, &more);	// a page has room for the long form
	sb_str(b, t);
#else
	sb_str(b, (enc >> 4) != M17_ENC_NONE ? TR(VALUE_ON, "On") : TR(VALUE_NONE, "None"));
#endif
}

#if CONFIG_M17_SIG
// A date and time from seconds since 2020-01-01 UTC (M17's timestamps): "2026-11-22 14:03:11 UTC"
static void sb_m17_time(sbuf_t *b, uint32_t ts)
{
	// days to the civil date (proleptic Gregorian), counting from 2000-03-01 (a year starting in March puts the leap
	// day last)
	uint32_t days = ts / 86400u + 7245u, s = ts % 86400u;	// 2020-01-01 is day 7245 from 2000-03-01
	uint32_t era = days / 146097u, doe = days % 146097u;
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u), mp = (5u * doy + 2u) / 153u;
	uint32_t d = doy - (153u * mp + 2u) / 5u + 1u, m = mp < 10u ? mp + 3u : mp - 9u;
	sb_u32(b, 2000u + era * 400u + yoe + (m <= 2u), 4);
	sb_chr(b, '-');
	sb_u32(b, m, 2);
	sb_chr(b, '-');
	sb_u32(b, d, 2);
	sb_chr(b, ' ');
	sb_u32(b, s / 3600u, 2);
	sb_chr(b, ':');
	sb_u32(b, s / 60u % 60u, 2);
	sb_chr(b, ':');
	sb_u32(b, s % 60u, 2);
	sb_str(b, " UTC");
}
#endif

// The signature's line (fields false) or, signed, who signed it, to whom, when, and whether that is in time (true)
static void cd_sig(sbuf_t *b, const radio_m17_details_t *d, bool fields)
{
	char t[40];
	bool warn, more;
	tr_id_t w = ui_m17_sig_word(d->sig, &warn);
	switch (d->sig) {
	case RADIO_M17_SIG_NO_KEY: w = TRN(CALL_VALUE_SIG_NO_KEY, TEXT_ALT_OPEN "Signed: no key to check" TEXT_ALT_NEXT
		"No key" TEXT_ALT_CLOSE); break;
#if CONFIG_M17_SIG
	case RADIO_M17_SIG_LATE: w = TRN(CALL_VALUE_SIG_LATE, TEXT_ALT_OPEN "Not checked: joined late" TEXT_ALT_NEXT
		"Joined late" TEXT_ALT_CLOSE); break;
	case RADIO_M17_SIG_LOST: w = TRN(CALL_VALUE_SIG_LOST, TEXT_ALT_OPEN "Not checked: frames lost" TEXT_ALT_NEXT
		"Frames lost" TEXT_ALT_CLOSE); break;
	case RADIO_M17_SIG_CHECKING: w = TRN(CALL_VALUE_SIG_NOT_CHECKED, TEXT_ALT_OPEN "Signed: not checked"
		TEXT_ALT_NEXT "Not checked" TEXT_ALT_CLOSE); break;
#endif
	default: break;
	}
	if (w == TR_NONE)
		return;
	if (fields)
		goto signer;
	sb_chr(b, '\n');
#if CONFIG_M17_SIG
	if (d->sig == RADIO_M17_SIG_SIGNER_DIFFERS) {	// "Signed by W1AW"
		gfx_text_alt(TR(CALL_VALUE_SIG_BY, TEXT_ALT_OPEN "Signed by " TEXT_ALT_NEXT "By " TEXT_ALT_CLOSE), 0, t,
			sizeof t, &more);
		sb_str(b, t);
		ui_sb_m17(b, d->signer);
	} else
#endif
	{
		gfx_text_alt(tr(w), 0, t, sizeof t, &more);
		sb_str(b, t);
	}
	return;
signer:
#if CONFIG_M17_SIG
	if (m17_addr_get(d->signer) == 0)
		return;
	line(b, TR(CALL_LABEL_SIGNER, "Signer"));
	ui_sb_m17(b, d->signer);
	line(b, TR(CALL_LABEL_SIGNED_TO, "Signed to"));
	ui_sb_m17(b, d->sig_dst);
	line(b, TR(CALL_LABEL_SIGNED_AT, "Signed at"));
	if (d->sig_ts == 0)
		sb_str(b, TR(CALL_VALUE_TIME_NOT_SENT, "Not sent"));
	else
		sb_m17_time(b, d->sig_ts);
	// No trusted clock: the radio never calls a call fresh (m17.md 7.4)
	line(b, TR(CALL_LABEL_TIME_CHECK, "Time check"));
	tr_id_t fresh = d->sig == RADIO_M17_SIG_REPLAY ? TRN(CALL_VALUE_SIG_REPLAY, TEXT_ALT_OPEN
		"Repeat of an earlier call" TEXT_ALT_NEXT "Repeat" TEXT_ALT_CLOSE) : d->sig_fresh == RADIO_M17_FRESH_YES ?
		TRN(CALL_VALUE_TIME_IN_TIME, "In time") : d->sig_fresh == RADIO_M17_FRESH_NO ?
		TRN(CALL_VALUE_TIME_OLD, "Too old") : TRN(CALL_VALUE_TIME_NO_CLOCK, TEXT_ALT_OPEN "Not done: no clock"
		TEXT_ALT_NEXT "No clock" TEXT_ALT_CLOSE);
	gfx_text_alt(tr(fresh), 0, t, sizeof t, &more);
	sb_str(b, t);
#endif
}

static const char *cd_text(void)
{
	const radio_m17_details_t *d = ui_m17_details();
	const radio_m17_view_t *m = &ui_view()->m17;
	sbuf_t b;
	char t[40];
	bool more;
	if (d == NULL)
		return TR(CALL_EMPTY_DETAILS, "No M17 call heard yet");
	page_init(&b);
	sb_str(&b, TR(CALL_LABEL_FROM, "From"));
	sb_str(&b, ": ");
	ui_sb_m17(&b, d->orig);
	cd_sig(&b, d, false);	// the signature's word next: what a banner about it sends the user here for
	// the via is the reflector (extended callsign META) where it isn't SRC, else SRC relayed it
	if (m17_addr_get(d->via) != 0 && memcmp(d->via, d->src, sizeof d->src) != 0) {
		line(&b, TR(CALL_LABEL_REFLECTOR, "Reflector"));
		ui_sb_m17(&b, d->via);
	}
	if (memcmp(d->src, d->orig, sizeof d->src) != 0) {
		line(&b, TR(CALL_LABEL_SENT_BY, "Sent by"));
		ui_sb_m17(&b, d->src);
	}
	line(&b, TR(CALL_LABEL_TO, "To"));
	if (memcmp(d->dst, m->own, sizeof d->dst) == 0 && m17_addr_get(m->own) != 0)
		sb_str(&b, TR(CALL_VALUE_TO_YOU, "To you"));
	else
		ui_sb_m17(&b, d->dst);
	sb_str(&b, "\nCAN: ");
	sb_u32(&b, d->can, 1);
#if CONFIG_M17_PACKET
	if (d->packet) {
		// "Packet: IPv4, 120 bytes"; the protocol names are never translated
		static const char *const names[] = { NULL, "AX.25", "APRS", "6LoWPAN", "IPv4", "SMS", "Winlink" };
		line(&b, TR(CALL_LABEL_PACKET, "Packet"));
		if (d->pkt_type == M17_PKT_RAW) {
			sb_str(&b, TR(CALL_VALUE_PACKET_RAW, "Raw data"));
		} else if (d->pkt_type < sizeof names / sizeof names[0]) {
			sb_str(&b, names[d->pkt_type]);
		} else {
			sb_str(&b, TR(CALL_VALUE_PACKET_TYPE, "Data type "));
			sb_u32(&b, d->pkt_type, 1);
		}
		sb_str(&b, ", ");
		sb_u32(&b, d->pkt_len, 1);
		sb_str(&b, TR(CALL_UNIT_BYTES, " bytes"));
	} else
#endif
	{
		line(&b, TR(CALL_LABEL_VOICE, "Voice"));
		gfx_text_alt(d->voice == M17_DATA_DATA ? TR(CALL_VALUE_DATA_ONLY, "Data only") :
			d->voice == M17_DATA_VOICE_DATA ? TR(CHAN_VALUE_RATE_1600, TEXT_ALT_OPEN "1600 bit/s + data"
			TEXT_ALT_NEXT "1600 + data" TEXT_ALT_NEXT "1600" TEXT_ALT_CLOSE) : "3200 bit/s", 0, t, sizeof t,
			&more);
		sb_str(&b, t);
		line(&b, TR(LABEL_ENCRYPTION, "Encryption"));
		if ((d->enc >> 4) != M17_ENC_NONE && !d->played) {
			gfx_text_alt(TR(CALL_VALUE_ENC_NOT_PLAYED, TEXT_ALT_OPEN "Not played: no key" TEXT_ALT_NEXT "No key"
				TEXT_ALT_CLOSE), 0, t, sizeof t, &more);
			sb_str(&b, t);
		} else {
			sb_m17_enc(&b, d->enc);
		}
	}
	if (d->text[0] != '\0') {
		line(&b, TR(CALL_LABEL_TEXT, "Text"));
		char txt[M17_TEXT_MAX + 1];
		utf8_to_display(txt, sizeof txt, d->text, sizeof d->text);
		sb_str(&b, txt);
	}
	if (d->gnss) {
		ui_position_t me;
		line(&b, TR(TITLE_POSITION, "Position"));
		if (ui_svc_position(&me) && me.fix) {
			uint32_t meters;
			uint16_t bearing;
			position_range(me.lat_e7, me.lon_e7, d->lat_e7, d->lon_e7, &meters, &bearing);
			sb_range(&b, meters, bearing);
			sb_chr(&b, '\n');
		}
		sb_degrees(&b, d->lat_e7);
		sb_chr(&b, ' ');
		sb_degrees(&b, d->lon_e7);
		if (d->alt_ok) {
			sb_chr(&b, '\n');
			sb_str(&b, TR(POS_BODY_ALTITUDE, "Altitude "));
			sb_i32(&b, d->alt_dm / 10);
			sb_str(&b, " m");
		}
		if (d->vel_ok) {
			line(&b, TR(CALL_LABEL_SPEED, "Speed"));
			sb_u32(&b, (d->speed_kmh10 + 5u) / 10u, 1);
			sb_str(&b, " km/h, ");
			sb_u32(&b, d->bearing_deg, 1);
			sb_str(&b, GLYPH_DEGREE);
		}
		if (d->station <= M17_STATION_HANDHELD) {
			static const tr_id_t st[3] = { TRN(CALL_VALUE_STATION_FIXED, "Fixed station"),
				TRN(CALL_VALUE_STATION_MOBILE, "Mobile"), TRN(CALL_VALUE_STATION_HANDHELD, "Handheld") };
			line(&b, TR(CALL_LABEL_STATION, "Station"));
			sb_str(&b, tr(st[d->station]));
		}
	}
	cd_sig(&b, d, true);
	if (d->rssi_dbm != 0) {
		line(&b, TR(CALL_LABEL_SIGNAL, "Signal"));
		sb_i32(&b, d->rssi_dbm);
		sb_str(&b, " dBm");
	}
	if (d->end_ms != 0) {
		line(&b, TR(CALL_LABEL_HEARD, "Heard"));
		sb_age_long(&b, ui_st.now_ms - d->end_ms);
	}
	return page_text();
}

static void page_enter(void) { ui_m17_page_first = 0; }

static bool page_key(const input_event_t *ev)
{
	return ui_text_page_key(ev, &ui_m17_page_first, ui_m17_page_max);
}

static void cd_draw(void)
{
	ui_m17_page_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR(CALL_TITLE_DETAILS, "Call Details"),
		cd_text(), &ui_m17_page_first, false);	// no dim labels: a signature's words have colons of their own
}

// PTT here talks to the originator once, as on a message page: one row, its target
static uint16_t cd_count(void) { return 1; }

static bool cd_target(uint16_t i, ui_target_t *t)
{
	(void)i;
	const radio_m17_view_t *m = &ui_view()->m17;
	if (!(m->flags & RADIO_M17_HAS_DETAILS))
		return false;
	memset(t, 0, sizeof *t);
	t->addr.kind = UI_ADDR_CALL;
	t->addr.net = UI_NET_M17;
	memcpy(t->addr.call, m->orig, sizeof t->addr.call);
	return true;
}

const ui_scr_def_t ui_call_details_screen = {
	.name = "call-details", .flags = SF_FIELD | SF_NO_STRIP, .key = page_key, .draw = cd_draw, .enter = page_enter,
	.count = cd_count, .target = cd_target,
};

#if CONFIG_M17_SIG
// --- The signing key and a station's key (13.1): arg 0 ours, else the station key's index + 1 ---

static const char *key_text(void)
{
	uint16_t arg = ui_frame()->arg;
	sbuf_t b;
	size_t cap;
	char *s = ui_scratch(&cap);
	page_init(&b);
	// the key read into the records' room (the wide record's and the details' part of the scratch)
	if (arg == 0) {
		radio_m17_key_t *k = (radio_m17_key_t *)(void *)(s + M17_WIDE_AT(cap));
		_Static_assert(sizeof(radio_m17_key_t) <= sizeof(m17_wide_t) + sizeof(radio_m17_details_t),
			       "the key fits the records' room");
		bool sign = ui_m17_wide()->sign;	// before the key: the record shares that room
		if (!radio_m17_key(k) || !k->present)
			return TR(M17_EMPTY_NO_SIGNING_KEY, "No signing key. Add one in the Codeplug Editor.");
		if (k->damaged)
			return TR(M17_BODY_KEY_DAMAGED,
				"The key doesn't match its public key. Write it again in the editor.");
		sb_str(&b, TR(M17_LABEL_SIGNS_AS, "Signs as"));
		sb_str(&b, ": ");
		uint8_t own[M17_ADDR_BYTES];
		m17_addr_put(own, m17_own_addr(NULL));
		if (m17_addr_get(own) != 0)
			ui_sb_m17(&b, own);
		else
			sb_str(&b, TR(VALUE_NOT_SET, "Not set"));
		line(&b, TR(M17_LABEL_FINGERPRINT, "Fingerprint"));
		sb_hex_groups(&b, k->fp, M17_KEY_FP_BYTES);
		line(&b, TR(SET_LABEL_SIGN_M17_CALLS, "Sign M17 calls"));
		sb_str(&b, sign ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		// what the signatures' timestamp comes from: a fresh UTC, else none (sent as 0)
		uint32_t unix_s;
		uint8_t src;
		line(&b, TR(M17_LABEL_CLOCK, "Clock"));
		char t[32];
		bool more;
		gfx_text_alt(!position_utc_now(hal_millis(), 60000u, &unix_s, &src) ? TR(M17_VALUE_CLOCK_NONE,
			TEXT_ALT_OPEN "None: time sent as 0" TEXT_ALT_NEXT "None" TEXT_ALT_CLOSE) : src == UTC_SRC_GNSS ?
			TR(M17_VALUE_CLOCK_GPS, "GPS time") : "UTC", 0, t, sizeof t, &more);
		sb_str(&b, t);
		line(&b, TR(M17_LABEL_PUBLIC_KEY, "Public key"));
		sb_chr(&b, '\n');
		sb_hex_groups(&b, k->q, sizeof k->q);
		return page_text();
	}
	uint64_t addr;
	uint8_t *q = (uint8_t *)s + M17_WIDE_AT(cap), fp[M17_KEY_FP_BYTES];
	if (!m17_station_key_at((uint16_t)(arg - 1u), &addr, q))
		return TR(M17_EMPTY_NO_STATION_KEYS, TEXT_ALT_OPEN "No station keys: add them in the editor" TEXT_ALT_NEXT
			"No station keys" TEXT_ALT_CLOSE);
	m17_key_fp(q, fp);
	sb_str(&b, TR(M17_LABEL_FINGERPRINT, "Fingerprint"));
	sb_str(&b, ": ");
	sb_hex_groups(&b, fp, M17_KEY_FP_BYTES);
	line(&b, TR(M17_LABEL_PUBLIC_KEY, "Public key"));
	sb_chr(&b, '\n');
	sb_hex_groups(&b, q, 33);
	return page_text();
}

// The title: "Signing Key", or a station's "Key for N0CALL"
static void key_draw(void)
{
	uint16_t arg = ui_frame()->arg;
	char title[24];
	sbuf_t b;
	uint64_t addr;
	uint8_t q[33], a[M17_ADDR_BYTES];
	sb_init(&b, title, sizeof title);
	if (arg != 0 && m17_station_key_at((uint16_t)(arg - 1u), &addr, q)) {
		sb_str(&b, TR(M17_TITLE_KEY_FOR, "Key for "));
		m17_addr_put(a, addr);
		ui_sb_m17(&b, a);
	} else {
		sb_str(&b, TR(M17_TITLE_SIGNING_KEY, "Signing Key"));
	}
	ui_m17_page_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, title, key_text(), &ui_m17_page_first, true);
}

const ui_scr_def_t ui_m17_key_screen = {
	.name = "m17-key", .flags = SF_NO_STRIP, .key = page_key, .draw = key_draw, .enter = page_enter,
};

// --- The station keys (13.2): every M17 system's station keys, found by callsign; OK opens one ---
// Each m17_station_key_at walks the settings area, so the count (n + 1 of them) is taken once per change of the keys
// (codeplug_kv_serial): the Settings row asks for it on every draw, the list on every pass. The UI task's alone, in
// cold RAM where the board has it (main RAM is short)
static uint16_t keys_n RFOUNDRY_COLD_RAM;
static uint16_t keys_serial RFOUNDRY_COLD_RAM;	// codeplug_kv_serial() | 0x100 when keys_n was counted, 0 never

static uint16_t keys_raw_count(void)
{
	uint16_t serial = (uint16_t)(codeplug_kv_serial() | 0x100u);
	if (keys_serial != serial) {
		uint64_t addr;
		uint8_t q[33];
		uint16_t n = 0;
		while (n < UI_MSG_INDEX_MAX && m17_station_key_at(n, &addr, q))
			n++;
		keys_n = n;
		keys_serial = serial;
	}
	return keys_n;
}

static void keys_label(sbuf_t *b, uint64_t addr)
{
	uint8_t a[M17_ADDR_BYTES];
	m17_addr_put(a, addr);
	ui_sb_m17(b, a);
}

static bool keys_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	uint64_t addr;
	uint8_t q[33];
	sbuf_t b;
	if (!m17_station_key_at(raw, &addr, q))
		return false;
	sb_init(&b, buf, len);
	keys_label(&b, addr);
	return true;
}

// The callsign, and the first two groups of its fingerprint: "N0CALL  3F9B C000" (one walk for both)
static void keys_item(uint16_t i, ui_item_t *it)
{
	uint16_t raw = ui_filter_raw(i);
	uint64_t addr;
	uint8_t q[33], fp[M17_KEY_FP_BYTES];
	sbuf_t b;
	if (!m17_station_key_at(raw, &addr, q))
		return;
	sb_init(&b, it->label, sizeof it->label);
	keys_label(&b, addr);
	m17_key_fp(q, fp);
	sb_init(&b, it->value, sizeof it->value);
	sb_hex_groups(&b, fp, 4);
}

static void keys_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_M17_KEY, (uint16_t)(ui_filter_raw(i) + 1u));
}

const ui_scr_def_t ui_m17_keys_screen = {
	.name = "m17-keys", .flags = SF_LIST | SF_FILTER, .prefix = TRN(M17_TITLE_STATION_KEYS, "Station Keys"),
	.hint = TRN(M17_HINT_KEYS, TEXT_ALT_OPEN "OK open  0-9 find  Back" TEXT_ALT_NEXT "OK open  Back" TEXT_ALT_CLOSE),
	.empty = TRN(M17_EMPTY_NO_STATION_KEYS, TEXT_ALT_OPEN "No station keys: add them in the editor" TEXT_ALT_NEXT
		"No station keys" TEXT_ALT_CLOSE),
	.count = ui_filter_count, .item = keys_item, .select = keys_select,
	.filter = &(const ui_filter_def_t){ .raw_count = keys_raw_count, .raw_label = keys_raw_label },
};
#endif

#if CONFIG_M17_BERT
// --- The BER test (14): the engine in BERT mode while the page is open on an M17 channel ---
static bool bert_held;		// we asked for BERT mode: ui_m17_tick gives it back while the page isn't on top

static bool bert_here(void) { return ui_view()->ch.mode == RADIO_MODE_M17; }

static void bert_enter(void)
{
	if (bert_here() && !bert_held) {
		radio_req_m17_bert(RADIO_REQ_M17_BERT_MODE_ON);
		bert_held = true;
	}
}

static void bert_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	const radio_view_t *v = ui_view();
	radio_m17_bert_t k;
	char buf[40];
	sbuf_t b;
	int w = gfx_width();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(v, &ui_st, NULL, TR(M17_TITLE_BERT, "M17 BER Test")) + 2;
	if (!bert_here()) {	// the longest form that fits (font.h alternates)
		gfx_text_fit(L->small, TR(M17_WHY_BERT_M17_ONLY, TEXT_ALT_OPEN "Go to an M17 channel first" TEXT_ALT_NEXT
			"M17 channels only" TEXT_ALT_CLOSE), w, buf, sizeof buf);
		gfx_text(L->small, 0, y, buf, ui_col.warn);
		ui_draw_banner(&ui_st);
		ui_draw_bottom(&ui_st, TR(HINT_BACK, "Back"));
		return;
	}
	radio_m17_bert_get(&k);
	bool tx = v->state == RS_TX;
	static const tr_id_t states[4] = { TRN(M17_STATE_BERT_NO_SIGNAL, "No signal"),
		TRN(M17_STATE_BERT_SEARCHING, "Searching"), TRN(M17_STATE_BERT_LOCKED, "Locked"),
		TRN(M17_STATE_BERT_LOST, "Lock lost") };
	uint8_t st = k.state < 4 ? k.state : RADIO_M17_BERT_NO_SIGNAL;
	gfx_text(L->medium, 0, y, tx ? TR(STATE_SENDING, "SENDING") : tr(states[st]), tx ? ui_col.tx :
		st == RADIO_M17_BERT_LOCKED ? ui_col.rx : st == RADIO_M17_BERT_LOST ? ui_col.warn : ui_col.dim);
	y += L->medium->h;
	sb_init(&b, buf, sizeof buf);
	if (tx) {
		sb_mmss(&b, k.tx_ms / 1000u);	// how long it has been sending
	} else {
		sb_str(&b, tr(TRX(M17_LABEL_BER, "BER")));	// never translated
		sb_chr(&b, ' ');
		if (k.bits == 0) {
			sb_chr(&b, '-');
		} else {
			// the bit error rate in thousandths of a percent: "0.012%"
			uint32_t r = (uint32_t)(((uint64_t)k.errs * 100000u + k.bits / 2u) / k.bits);
			sb_u32(&b, r / 1000u, 1);
			sb_chr(&b, '.');
			sb_u32(&b, r % 1000u, 3);
			sb_chr(&b, '%');
		}
	}
	const font_t *big = gfx_text_width(L->large, buf) <= w ? L->large : L->medium;
	gfx_text(big, 0, y, buf, tx ? ui_col.tx : ui_col.fg);
	y += big->h;
	// "Errors 12 of 98304 bits" where it fits, else "12/98304"; sending: the frames sent. The alternates in a buffer
	// of their own: both counters at 10 digits take 60 bytes in English, and a cut one would lose the short form's
	// digits. Should a translation still overflow it, the short form alone
	char line[96];
	sb_init(&b, line, sizeof line);
	if (tx) {
		sb_str(&b, TR(M17_LABEL_FRAMES, "Frames"));
		sb_chr(&b, ' ');
		sb_u32(&b, k.tx_frames, 1);
	} else {
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, TR(M17_BODY_ERRORS, "Errors "));
		sb_u32(&b, k.errs, 1);
		sb_str(&b, TR(PART_OF, " of "));
		sb_u32(&b, k.bits, 1);
		sb_str(&b, TR(M17_UNIT_BITS, " bits"));
		sb_str(&b, TEXT_ALT_NEXT);
		sb_u32(&b, k.errs, 1);
		sb_chr(&b, '/');
		sb_u32(&b, k.bits, 1);
		sb_str(&b, TEXT_ALT_CLOSE);
		if (b.p >= b.end) {	// full: perhaps cut
			sb_init(&b, line, sizeof line);
			sb_u32(&b, k.errs, 1);
			sb_chr(&b, '/');
			sb_u32(&b, k.bits, 1);
		}
	}
	char fit[40];
	gfx_text_fit(L->small, line, w, fit, sizeof fit);
	gfx_text(L->small, 0, y, fit, ui_col.fg);
	y += L->small->h;
	if (!tx && y + L->small->h <= L->hint_y) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR(M17_LABEL_FRAMES, "Frames"));
		sb_chr(&b, ' ');
		sb_u32(&b, k.frames, 1);
		if (k.rssi_dbm != 0) {
			sb_str(&b, "  ");
			sb_i32(&b, k.rssi_dbm);
			sb_str(&b, " dBm");
		}
		gfx_text_fit(L->small, buf, w, fit, sizeof fit);
		gfx_text(L->small, 0, y, fit, ui_col.dim);
	}
	ui_draw_banner(&ui_st);
	ui_draw_bottom(&ui_st, TR(M17_HINT_BERT, TEXT_ALT_OPEN "PTT send  OK reset  Back" TEXT_ALT_NEXT
		"PTT send  OK reset" TEXT_ALT_CLOSE));
}

static bool bert_key(const input_event_t *ev)
{
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS && bert_here()) {
		radio_req_m17_bert(RADIO_REQ_M17_BERT_RESET);
		ui_toast(TR(M17_TOAST_BERT_RESET, "Counts reset"));
		return true;
	}
	return false;
}

const ui_scr_def_t ui_m17_bert_screen = {
	.name = "m17-bert", .flags = SF_FIELD, .key = bert_key, .draw = bert_draw, .enter = bert_enter,
};
#endif

// --- Settings (11): the values of M17's action rows ---
void ui_m17_set_value(uint8_t row, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (row == 0) {
		// ours: the record's, or the radio name standing in where it is a callsign (what PTT sends)
		uint8_t a[M17_ADDR_BYTES];
		uint64_t own = m17_own_addr(NULL);
		if (own == 0) {
			sb_str(&b, TR(VALUE_NOT_SET, "Not set"));
			it->flags |= UI_ITEM_WARN;
			return;
		}
		m17_addr_put(a, own);
		ui_sb_m17(&b, a);
	} else if (row == 1) {
		// the call text where it fits a value whole, else that there is one. UTF-8 (m17-codeplug.md 3; another tool
		// may have written more than ASCII): as the display's text, its control bytes dropped, so none of them
		// reads as a text alternate's mark
		const m17_wide_t *w = ui_m17_wide();
		char t[M17_TEXT_MAX + 1];
		bool whole = utf8_to_display(t, sizeof t, (const char *)w->text, w->text_len);
		sb_str(&b, w->text_len == 0 ? TR(VALUE_NONE, "None") : whole && t[0] != '\0' && ui_text_cells(t) <= 12 ? t :
			TR(VALUE_SET, "Set"));
#if CONFIG_M17_SIG
	} else if (row == 2) {
		size_t cap;
		radio_m17_key_t *k = (radio_m17_key_t *)(void *)(ui_scratch(&cap) + M17_WIDE_AT(cap));
		bool set = radio_m17_key(k) && k->present;
		sb_str(&b, set ? TR(VALUE_SET, "Set") : TR(VALUE_NOT_SET, "Not set"));
	} else {
		sb_u32(&b, keys_raw_count(), 1);
#endif
	}
}

// Every UI pass: BERT mode while the page is on top, voice whenever it isn't (BACK, Home, Quick settings or the
// picker over it). Asked for again when the page is back on top: ui_pop runs no enter. The channel becoming M17
// with the page open asks too
void ui_m17_tick(uint8_t top)
{
#if CONFIG_M17_BERT
	if (top == SCR_M17_BERT) {
		bert_enter();
	} else if (bert_held) {
		radio_req_m17_bert(RADIO_REQ_M17_BERT_MODE_OFF);
		bert_held = false;
	}
#else
	(void)top;
#endif
}
#endif
