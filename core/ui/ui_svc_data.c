// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for text messages (the DMR message provider, ui_msg_dmr), call alert and radio check, on the DMR
// data service's API (core/dmr/dmr_data_api.h, safe from any task). Its events are polled (dmr_data_serial) and handed
// to the UI as ui_notify events, which become banners. Built only with the data core; otherwise the weak stubs stay.
#include "config.h"

#if CONFIG_DMR_DATA_CORE
#include <string.h>
#include "ui_svc.h"
#include "dmr/dmr_data_api.h"
#include "codeplug/codeplug.h"
#include "hal_time.h"

uint32_t ui_svc_feat_data(void)
{
	return (CONFIG_DMR_SMS ? UI_FEAT_SMS : 0) | (CONFIG_DMR_SIGNALING ? UI_FEAT_SIGNALING : 0) |
	       (CONFIG_ZONE_SHARE ? UI_FEAT_ZONE_SHARE : 0);
}

static void contact_name(uint32_t id, bool group, char *out, size_t len)
{
	contact_t c;
	out[0] = 0;
	if (codeplug_contact_find(id, group, &c)) {
		strncpy(out, c.name, len - 1);
		out[len - 1] = 0;
	}
}

#if CONFIG_DMR_SMS
// The DMR inbox as a message provider (ui_svc_msg.c). Its text is the display's already: the data service keeps a
// message's text as Latin-1 bytes, which the fonts draw
_Static_assert(DMR_DATA_INBOX_SIZE <= UI_MSG_INDEX_MAX, "a DMR inbox index fits a message reference");

static dmrDataMessage_t inbox_message;	// shared by the UI task; too big for its stack

static uint16_t dmr_msg_count(void)
{
	int n = sms_inbox_count();
	return (uint16_t)(n < 0 ? 0 : n);
}

static uint16_t dmr_msg_unread(void)
{
	int n = sms_inbox_unread();
	return (uint16_t)(n < 0 ? 0 : n);
}

static bool dmr_msg_get(uint16_t i, ui_msg_t *out)
{
	if (!sms_inbox_get(i, &inbox_message))
		return false;
	memset(out, 0, sizeof *out);
	ui_addr_set_dmr(&out->peer, inbox_message.src, false);	// the sender (a talkgroup message still has one)
	out->unread = inbox_message.unread;
	out->age_ms = hal_millis() - inbox_message.receivedAt;
	contact_name(inbox_message.src, false, out->name, sizeof out->name);
	strncpy(out->text, inbox_message.text, UI_MSG_TEXT_MAX);
	out->text[UI_MSG_TEXT_MAX] = 0;
	return true;
}

static void dmr_msg_read(uint16_t i)
{
	sms_inbox_mark_read(i);
}

static bool dmr_msg_delete(uint16_t i)
{
	return sms_inbox_delete(i);
}

static bool dmr_msg_send(const ui_msg_to_t *to, const char *text)
{
	// A message to a person asks for the acknowledgment ("Delivered") where its format has one; a group message
	// can't. The format (ETSI IP text, ETSI short data or TMS) is the radio's setting (dmrDataSettings_t smsFormat)
	bool group = ui_addr_group(&to->to);
	return sms_send(to->to.id, group, text, !group, DMR_SMS_FORMAT_DEFAULT);
}

static bool dmr_msg_reply(uint16_t i, const char *text)
{
	return sms_inbox_get(i, &inbox_message) && sms_send(inbox_message.src, false, text, true, (dmr_sms_format_t)inbox_message.format);
}

const ui_msg_provider_t ui_msg_dmr = {
	.net = UI_NET_DMR, .caps = UI_MSG_CAP_STATION, .text_max = UI_MSG_TEXT_MAX, .new_row = TR_NONE,
	.count = dmr_msg_count, .unread = dmr_msg_unread, .get = dmr_msg_get, .read = dmr_msg_read,
	.del = dmr_msg_delete, .send = dmr_msg_send, .reply = dmr_msg_reply,
};
#endif

static uint32_t check_sent_ms;

_Static_assert((int)UI_ARS_OFF == (int)DMR_ARS_STATE_OFF &&
	       (int)UI_ARS_REGISTERING == (int)DMR_ARS_STATE_REGISTERING &&
	       (int)UI_ARS_REGISTERED == (int)DMR_ARS_STATE_REGISTERED &&
	       (int)UI_ARS_REFUSED == (int)DMR_ARS_STATE_REFUSED &&
	       (int)UI_ARS_NO_ANSWER == (int)DMR_ARS_STATE_NO_ANSWER, "ui_svc.h UI_ARS_*");

uint8_t ui_svc_ars_state(void)
{
	return (uint8_t)dmr_data_ars_state();
}

bool ui_svc_radio_check(uint32_t id)
{
	check_sent_ms = hal_millis();
	return dmr_signal_radio_check(id);
}

bool ui_svc_call_alert(uint32_t id)
{
	return dmr_signal_call_alert(id);
}

void ui_svc_poll(void)
{
#if CONFIG_DMR_EMERGENCY
	ui_svc_emergency_poll();
#endif
#if CONFIG_APRS_RX
	ui_svc_aprs_msg_poll();
#endif
	// The event's own serial, not dmr_data_serial(): that one also moves when a message is read or deleted, and the
	// last event would come again ("Message from" over "Message deleted", with its tone)
	static uint32_t seen;
	dmrDataEvent_t d = dmr_data_last_event();
	if (d.serial == seen)
		return;
	seen = d.serial;
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ui_addr_set_dmr(&ev.from, d.id, false);
	switch (d.kind) {
#if CONFIG_DMR_SMS
	case DMR_DATA_EV_SMS_RECEIVED: {
		ui_msg_t m;
		ev.kind = UI_EV_MESSAGE;
		ev.index = 0;	// the newest in the DMR inbox
		if (dmr_msg_get(0, &m)) {
			strncpy(ev.text, m.text, sizeof ev.text - 1);
		}
		break;
	}
#endif
	case DMR_DATA_EV_SMS_SENT:
	case DMR_DATA_EV_SMS_FAILED:
		ev.kind = UI_EV_MESSAGE_SENT;
		ev.ok = d.kind == DMR_DATA_EV_SMS_SENT;
		ev.index = (ev.ok && !sms_send_confirmed()) ? 1 : 0;	// on air, but nothing confirms delivery
		break;
	case DMR_DATA_EV_CALL_ALERT:
		ev.kind = UI_EV_CALL_ALERT;
		break;
	case DMR_DATA_EV_CALL_ALERT_ACKED:
	case DMR_DATA_EV_CALL_ALERT_FAILED:
		ev.kind = UI_EV_CALL_ALERT_RESULT;
		ev.ok = d.kind == DMR_DATA_EV_CALL_ALERT_ACKED;
		break;
	case DMR_DATA_EV_RADIO_CHECK_OK:
	case DMR_DATA_EV_RADIO_CHECK_FAILED:
		ev.kind = UI_EV_RADIO_CHECK;
		ev.ok = d.kind == DMR_DATA_EV_RADIO_CHECK_OK;
		ev.ms = d.atMs - check_sent_ms;
		break;
#if CONFIG_ZONE_SHARE
	case DMR_DATA_EV_ZONE_OFFER:
	case DMR_DATA_EV_ZONE_SENT:
	case DMR_DATA_EV_ZONE_FAILED:
		// ui_svc_zone.c fills these whole (the offer's text, a talkgroup's name)
		if (ui_svc_zone_event(d.kind, d.id, &ev))
			ui_notify(&ev);
		return;
#endif
	default:
		return;		// ARS changes and checks of us stay off the screen
	}
	contact_name(d.id, false, ev.name, sizeof ev.name);
	ui_notify(&ev);
}
#endif
