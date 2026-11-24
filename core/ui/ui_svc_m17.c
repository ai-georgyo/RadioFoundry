// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17's SMS as a message provider (docs/ux/m17.md 15, ui_svc_msg.c): the radio core's inbox (radio_m17.h, 4
// messages, kept while the radio is on) as the Messages screens' rows, and the sends. M17 text is UTF-8; a row's text
// is the display's one-byte text (utf8_to_display), kept to UI_MSG_TEXT_MAX on a character boundary; what goes out is
// UTF-8 again (a Latin-1 letter two bytes, a Chinese character its three). The radio core tells no one of a new
// message or of a send's end, so ui_svc_m17_poll turns both into the UI's events: the banner of a message, and "Sent
// to" / "Not sent to" (M17 has no delivery report: never "Delivered"). Over the radio core's calls only, so the UI
// simulator runs it on its own inbox. Built with CONFIG_M17_PACKET
#include <string.h>
#include "ui_svc.h"
#include "ui_nav.h"
#include "gfx.h"
#include "config.h"

#if CONFIG_M17_PACKET
#include "radio/radio_m17.h"
#include "m17/m17.h"
#include "hal_time.h"

_Static_assert(RADIO_M17_SMS_INBOX <= UI_MSG_INDEX_MAX, "an M17 inbox index fits a message reference");

// A message read for a moment, in the UI's scratch: the radio core's record is too big for the UI task's stack
static radio_m17_sms_t *sms_at(uint16_t i)
{
	size_t cap;
	radio_m17_sms_t *m = (radio_m17_sms_t *)(void *)ui_scratch(&cap);
	_Static_assert(sizeof(radio_m17_sms_t) <= 512u, "the scratch holds a message");
	return i < RADIO_M17_SMS_INBOX && radio_m17_sms_get((uint8_t)i, m) ? m : NULL;
}

static uint16_t m17msg_count(void) { return radio_m17_sms_count(); }
static uint16_t m17msg_unread(void) { return radio_m17_sms_unread(); }

static bool m17msg_get(uint16_t i, ui_msg_t *out)
{
	const radio_m17_sms_t *m = sms_at(i);
	if (m == NULL)
		return false;
	memset(out, 0, sizeof *out);
	out->peer.kind = UI_ADDR_CALL;
	out->peer.net = UI_NET_M17;
	memcpy(out->peer.call, m->peer, sizeof out->peer.call);
	out->unread = m->unread;
	out->sent = m->sent;
	out->age_ms = hal_millis() - m->at_ms;
	// cut: kept short by the radio core, or here (a Latin-1 letter is one byte here, two in UTF-8: never longer)
	out->cut = !utf8_to_display(out->text, sizeof out->text, m->text, sizeof m->text) || m->cut;
	return true;
}

static void m17msg_read(uint16_t i)
{
	if (i < RADIO_M17_SMS_INBOX)
		radio_m17_sms_read((uint8_t)i);
}

static bool m17msg_delete(uint16_t i)
{
	return i < RADIO_M17_SMS_INBOX && radio_m17_sms_delete((uint8_t)i);
}

// The display's text as UTF-8 for the air, cut on a character boundary at what an SMS holds: ASCII as it is, a
// Latin-1 byte as its two bytes, a three-byte sequence (a Chinese character) as it is, the font's own glyph bytes as
// '?' (no one else has them)
static const char *to_utf8(const char *t)
{
	size_t cap, n = 0;
	char *o = ui_scratch(&cap);
	while (*t != '\0') {
		const uint8_t *c = (const uint8_t *)t;
		int len = 1;
		char e[3];
		size_t k = 1;
		e[0] = *t;
#if CONFIG_LANG_ZH
		if ((c[0] & 0xF0u) == 0xE0u && (gfx_utf8_next(t, &len), len == 3)) {
			memcpy(e, t, 3);
			k = 3;
		} else
#endif
		if (c[0] >= 0xA0u) {
			e[0] = (char)(0xC0u | (c[0] >> 6));
			e[1] = (char)(0x80u | (c[0] & 0x3Fu));
			k = 2;
		} else if (c[0] >= 0x80u) {
			e[0] = '?';
		}
		if (n + k > RADIO_M17_SMS_MAX)
			break;
		memcpy(o + n, e, k);
		n += k;
		t += len;
	}
	o[n] = '\0';
	return o;
}

static bool sms_send(const uint8_t to[M17_ADDR_BYTES], const char *text)
{
	return m17_addr_get(to) != 0 && radio_req_m17_sms(m17_addr_get(to), to_utf8(text));
}

// To the callsign the composer typed, or the station the composer was opened on
static bool m17msg_send(const ui_msg_to_t *to, const char *text)
{
	uint64_t a;
	uint8_t dst[M17_ADDR_BYTES];
	if (to->to.kind == UI_ADDR_CALL)
		return sms_send(to->to.call, text);
	if (!m17_addr_from_text(to->call, &a))
		return false;
	m17_addr_put(dst, a);
	return sms_send(dst, text);
}

static bool m17msg_reply(uint16_t i, const char *text)
{
	uint8_t to[M17_ADDR_BYTES];
	const radio_m17_sms_t *m = sms_at(i);
	if (m == NULL)
		return false;
	memcpy(to, m->peer, sizeof to);	// before the text takes the scratch
	return sms_send(to, text);
}

// From an M17 channel only (the radio core sends on the M17 channel in use), and not one set to encrypt: packet mode
// has no encryption, and the radio core never sends in the clear what the channel asks to encrypt (it refuses the SMS)
static bool m17msg_send_ok(tr_id_t *why)
{
	const radio_view_t *v = ui_view();
	tr_id_t w = v->ch.mode != RADIO_MODE_M17 ? TRN(MSG_WHY_M17_CHANNEL, TEXT_ALT_OPEN "Send from an M17 channel"
			TEXT_ALT_NEXT "M17 channels only" TEXT_ALT_CLOSE) :
		(v->m17.flags & RADIO_M17_TX_ENC) ? TRN(MSG_WHY_PRIVACY_ON, TEXT_ALT_OPEN
			"Privacy on: messages can't be encrypted" TEXT_ALT_NEXT "Privacy on: no messages" TEXT_ALT_CLOSE) :
		TR_NONE;
	if (w == TR_NONE)
		return true;
	*why = w;
	return false;
}

static bool m17msg_new_ok(void)
{
	return (ui_svc_m17_caps() & UI_M17_SMS) != 0;
}

const ui_msg_provider_t ui_msg_m17 = {
	.net = UI_NET_M17, .caps = UI_MSG_CAP_STATION | UI_MSG_CAP_TO_CALL | UI_MSG_CAP_CALLSIGN,
	.text_max = RADIO_M17_SMS_MAX, .new_row = TRN(MSG_ROW_NEW_M17, "New M17 message"), .new_ok = m17msg_new_ok,
	.send_ok = m17msg_send_ok,
	.count = m17msg_count, .unread = m17msg_unread, .get = m17msg_get, .read = m17msg_read,
	.del = m17msg_delete, .send = m17msg_send, .reply = m17msg_reply,
};

// The inbox's newest message and the sends' outcomes as the UI's events
static uint32_t newest_ms;	// the newest received message's time taken
static uint8_t result_seq;	// radio_m17_sms_result's count taken (0: none yet)

void ui_svc_m17_poll(void)
{
	ui_event_t ev;
	uint8_t seq, to[M17_ADDR_BYTES];
	uint8_t r = radio_m17_sms_result(&seq, to);
	const radio_m17_sms_t *m = sms_at(0);
	uint32_t at = m != NULL && !m->sent ? m->at_ms : newest_ms;
	if (newest_ms == 0 || (int32_t)(hal_millis() - newest_ms) < 0) {
		// what was there before the UI (or before the clock started over: a test's new run): no banner for it
		newest_ms = m != NULL && !m->sent ? m->at_ms : hal_millis() - 1u;
		newest_ms += newest_ms == 0;
		result_seq = seq;
		return;
	}
	memset(&ev, 0, sizeof ev);
	ev.from.kind = UI_ADDR_CALL;
	ev.from.net = UI_NET_M17;
	if ((int32_t)(at - newest_ms) > 0 && m != NULL) {	// a newer one, not an older one left by a delete
		newest_ms = at;
		ev.kind = UI_EV_MESSAGE;
		memcpy(ev.from.call, m->peer, sizeof ev.from.call);
		ui_notify(&ev);		// index 0: the newest
	}
	if (seq != result_seq) {
		result_seq = seq;
		if (r == RADIO_M17_SMS_SENT || r == RADIO_M17_SMS_BUSY) {
			ev.kind = UI_EV_MESSAGE_SENT;
			ev.ok = r == RADIO_M17_SMS_SENT;
			ev.index = ev.ok ? 1u : 2u;	// sent (no confirmation); not sent: the channel stayed busy
			memcpy(ev.from.call, to, sizeof ev.from.call);
			ui_notify(&ev);
		}
	}
}
#endif
