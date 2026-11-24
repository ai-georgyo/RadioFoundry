// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Heard (docs/ux/m17.md 7; docs/spec/radio-features.md, the heard list): the stations heard lately, newest
// first, one row each: the caller, how long ago, how many times, what its latest was (signed, encrypted, data). OK
// opens a station's details, # its actions (talk to it, a message where SMS is built). The list is the radio core's
// (radio_heard.h), any mode's; M17 is its only source so far. Built with CONFIG_HEARD_LIST
#include <string.h>
#include "ui_nav.h"
#include "config.h"

#if CONFIG_HEARD_LIST
#include "radio/radio_heard.h"
#include "radio/radio_air_modes.h"
#include "ui_m17.h"
#include "position/position_geo.h"

// The caller as an actions target: an M17 callsign, else a DMR ID
static void heard_target(const radio_heard_t *h, ui_target_t *t)
{
	memset(t, 0, sizeof *t);
	if (h->flags & RADIO_HEARD_ADDR) {
		t->addr.kind = UI_ADDR_CALL;
		t->addr.net = UI_NET_M17;
		memcpy(t->addr.call, h->addr, sizeof t->addr.call);
	} else {
		ui_addr_set_dmr(&t->addr, h->id, false);
	}
}

static void sb_heard_who(sbuf_t *b, const radio_heard_t *h)
{
	ui_target_t t;
	heard_target(h, &t);
	ui_sb_addr(b, &t.addr);
}

static uint16_t heard_count(void) { return radio_heard_count(); }

// "N0CALL" / "2m x3 Sig OK": the age, the times heard where more than once, a failed or good signature's word
static void heard_item(uint16_t i, ui_item_t *it)
{
	radio_heard_t h;
	sbuf_t b;
	if (!radio_heard_get((uint8_t)i, &h))
		return;
	sb_init(&b, it->label, sizeof it->label);
	sb_heard_who(&b, &h);
	sb_init(&b, it->value, sizeof it->value);
	sb_age(&b, ui_st.now_ms - h.last_ms);
	if (h.count > 1) {
		sb_str(&b, " x");
		sb_u32(&b, h.count, 1);
	}
#if CONFIG_M17
	bool warn;
	tr_id_t w = (h.flags & RADIO_HEARD_SIG_BAD) ? ui_m17_sig_word(RADIO_M17_SIG_INVALID, &warn) :
		(h.flags & RADIO_HEARD_SIG_OK) ? ui_m17_sig_word(RADIO_M17_SIG_VALID, &warn) : TR_NONE;
	if (w != TR_NONE) {
		char word[16];
		bool more;
		gfx_text_alt(tr(w), 7, word, sizeof word, &more);	// the short form
		sb_chr(&b, ' ');
		sb_str(&b, word);
	}
#endif
	if (h.flags & RADIO_HEARD_SIG_BAD)
		it->flags |= UI_ITEM_WARN;
}

static bool heard_target_at(uint16_t i, ui_target_t *t)
{
	radio_heard_t h;
	if (!radio_heard_get((uint8_t)i, &h))
		return false;
	heard_target(&h, t);
	return true;
}

// The station the page shows: who it is, kept when the page opened and found again by that (radio_heard_find), not
// by its row. Every reception moves the rows, so a row number would show, and PTT call, whoever is there now. One
// station page is open at a time. Only the identity (8 bytes, the UI task's, read now and then: cold RAM where the
// board has it, as main RAM is short): the mode, RADIO_HEARD_ADDR, the address or the ID in its place
#define ONE_SET		0x80u	// one.flags: a station chosen (the page pushed by name, as a test does, has none)
static struct {
	uint8_t mode, flags;
	uint8_t who[6];		// the address, else the ID (big endian, its first four bytes)
} one RFOUNDRY_COLD_RAM;

static void one_ident(radio_heard_t *k)
{
	memset(k, 0, sizeof *k);
	k->mode = one.mode;
	k->flags = one.flags & RADIO_HEARD_ADDR;
	for (uint8_t n = 0; n < sizeof one.who; n++) {
		if (k->flags & RADIO_HEARD_ADDR)
			k->addr[n] = one.who[n];
		else if (n < 4)
			k->id = k->id << 8 | one.who[n];
	}
}

static void heard_select(uint16_t i, bool hold)
{
	radio_heard_t h;
	(void)hold;
	if (!radio_heard_get((uint8_t)i, &h))
		return;
	one.mode = h.mode;
	one.flags = (uint8_t)(ONE_SET | (h.flags & RADIO_HEARD_ADDR));
	for (uint8_t n = 0; n < sizeof one.who; n++)
		one.who[n] = (h.flags & RADIO_HEARD_ADDR) ? h.addr[n] : n < 4 ? (uint8_t)(h.id >> (24 - 8 * n)) : 0u;
	ui_push(SCR_HEARD_ONE, 0);
}

// The station as it is in the list now; false when it has left it (*h then its identity alone)
static bool one_find(radio_heard_t *h)
{
	radio_heard_t k;
	one_ident(&k);
	if (radio_heard_find(&k, h))
		return true;
	*h = k;
	return false;
}

static void heard_context(uint16_t i)
{
	ui_target_t t;
	if (heard_target_at(i, &t))
		ui_open_actions(&t);
}

const ui_scr_def_t ui_heard_screen = {
	.name = "heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN(HEARD_TITLE, "Heard"),
	.hint = TRN(HINT_OK_CHOOSE_MORE_BACK, "OK choose  # more  Back"),
	.empty = TRN(HEARD_EMPTY, "No stations heard yet"),
	.count = heard_count, .item = heard_item, .select = heard_select, .context = heard_context,
	.target = heard_target_at,
};

// One station: who, the mode, when and how often, what it called, the signal, where it was, its latest's marks (the
// text pages' scroll is ui_m17.c's: one page is open at a time)

static const char *one_text(void)
{
	radio_heard_t h;
	size_t cap;
	char *s = ui_scratch(&cap);
	sbuf_t b;
	if (!(one.flags & ONE_SET))
		return TR(HEARD_EMPTY, "No stations heard yet");
	sb_init(&b, s, cap);
	if (!one_find(&h)) {	// pushed out by newer stations: who it was, and that it is gone (no target)
		sb_heard_who(&b, &h);
		sb_chr(&b, '\n');
		sb_str(&b, TR(HEARD_BODY_STATION_GONE, "No longer in list"));
		return s;
	}
	sb_heard_who(&b, &h);
	sb_chr(&b, ' ');
	sb_str(&b, radio_mode_name(h.mode));
	sb_chr(&b, '\n');
	sb_str(&b, TR(CALL_LABEL_HEARD, "Heard"));
	sb_str(&b, ": ");
	sb_age_long(&b, ui_st.now_ms - h.last_ms);
	if (h.count > 1) {
		sb_str(&b, ", x");
		sb_u32(&b, h.count, 1);
	}
	if (m17_addr_get(h.to) != 0) {
		sb_chr(&b, '\n');
		sb_str(&b, TR(CALL_LABEL_TO, "To"));
		sb_str(&b, ": ");
		ui_sb_m17(&b, h.to);
	}
	if (h.rssi_dbm != 0) {
		sb_chr(&b, '\n');
		sb_str(&b, TR(CALL_LABEL_SIGNAL, "Signal"));
		sb_str(&b, ": ");
		sb_i32(&b, h.rssi_dbm);
		sb_str(&b, " dBm");
	}
	if (h.flags & RADIO_HEARD_GNSS) {
		ui_position_t me;
		sb_chr(&b, '\n');
		sb_str(&b, TR(TITLE_POSITION, "Position"));
		sb_str(&b, ": ");
		if (ui_svc_position(&me) && me.fix) {
			uint32_t meters;
			uint16_t bearing;
			position_range(me.lat_e7, me.lon_e7, h.lat_e7, h.lon_e7, &meters, &bearing);
			sb_range(&b, meters, bearing);
			sb_chr(&b, '\n');
		}
		sb_degrees(&b, h.lat_e7);
		sb_chr(&b, ' ');
		sb_degrees(&b, h.lon_e7);
	}
	bool warn;
	tr_id_t w = (h.flags & RADIO_HEARD_SIG_BAD) ? ui_m17_sig_word(RADIO_M17_SIG_INVALID, &warn) :
		(h.flags & RADIO_HEARD_SIG_OK) ? ui_m17_sig_word(RADIO_M17_SIG_VALID, &warn) : TR_NONE;
	char word[40];
	bool more;
	if (w != TR_NONE) {
		gfx_text_alt(tr(w), 0, word, sizeof word, &more);
		sb_chr(&b, '\n');
		sb_str(&b, word);
	}
	if (h.flags & (RADIO_HEARD_ENC | RADIO_HEARD_DATA)) {
		sb_chr(&b, '\n');
		sb_str(&b, (h.flags & RADIO_HEARD_DATA) ? TR(RECENT_VALUE_DATA, "Data") : TR(HOME_CHIP_PRIVACY, "Priv"));
	}
	return s;
}

static void one_enter(void) { ui_m17_page_first = 0; }

// PTT here talks to the station once, as on Call Details: the station the page opened on while it is in the list
static uint16_t one_count(void) { return 1; }

static bool one_target(uint16_t i, ui_target_t *t)
{
	radio_heard_t h;
	(void)i;
	if (!(one.flags & ONE_SET) || !one_find(&h))
		return false;
	heard_target(&h, t);
	return true;
}

static void one_draw(void)
{
	ui_m17_page_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR(HEARD_TITLE, "Heard"), one_text(),
		&ui_m17_page_first, true);
}

static bool one_key(const input_event_t *ev)
{
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		ui_target_t t;
		if (one_target(0, &t))
			ui_open_actions(&t);
		return true;
	}
	return ui_text_page_key(ev, &ui_m17_page_first, ui_m17_page_max);
}

const ui_scr_def_t ui_heard_one_screen = {
	.name = "heard-station", .flags = SF_FIELD | SF_NO_STRIP, .key = one_key, .draw = one_draw, .enter = one_enter,
	.count = one_count, .target = one_target,
};
#endif
