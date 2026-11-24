// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's M17 settings services (core/ui/ui_svc_radio.c) against radio_task.c's request slot: one copy of the
// settings waits for the radio task's turn, the latest request's. Each service changes one field (the CAN, the voice,
// the destination, digital monitor's squelch), so two before the radio task's turn must start the second from the
// first (radio_m17_settings_next), never from the settings still in force: else the first is lost (boot applies the
// CAN and the voice back to back). The radio core's other entry points are stubs
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio.h"
#include "radio/radio_m17.h"
#include "ui/ui_svc.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "input/input.h"
#include "dmr/dmrPrivacy.h"
#include "m17/m17_engine.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// --- radio_task.c's slot: the settings in force, the request waiting and whether one waits ---
static radio_m17_settings_t in_force, waiting;
static bool waits;
static unsigned posts;
static radio_view_t view;

void radio_m17_settings(radio_m17_settings_t *out) { *out = in_force; }
void radio_m17_settings_next(radio_m17_settings_t *out) { *out = waits ? waiting : in_force; }
void radio_req_m17_settings(const radio_m17_settings_t *s)
{
	waiting = *s;
	waits = true;
	posts++;
}

// The radio task's turn
static void take(void)
{
	if (waits)
		in_force = waiting;
	waits = false;
}

void radio_get_view(radio_view_t *v) { *v = view; }
bool radio_m17_fits(void) { return true; }

// --- Not under test ---
const board_t board;
radio_view_t ui_svc_view;
void ambeVoiceSetEncodeOptions(uint32_t opts) { (void)opts; }
bool codeplug_channel(uint16_t number, channel_t *out) { (void)number; (void)out; return false; }
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out) { (void)out; return false; }
bool codeplug_zone(uint16_t index, zone_t *out) { (void)index; (void)out; return false; }
bool dmrPrivacyKeyIsValid(dmrPrivacyType_t type, uint16_t key) { (void)type; (void)key; return false; }
void input_set_dtmf_block(bool blocked) { (void)blocked; }
void input_set_ptt_block(bool blocked) { (void)blocked; }
void input_set_timing(uint16_t long_ms, uint16_t repeat_ms) { (void)long_ms; (void)repeat_ms; }
void position_gps_enable(bool on) { (void)on; }
bool radio_bcast_capable(void) { return false; }
bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at) { (void)ref; (void)sel; (void)at; return false; }
void radio_get_settings(radio_settings_t *s) { memset(s, 0, sizeof *s); }
bool radio_get_vfo(uint8_t index, channel_t *out) { (void)index; (void)out; return false; }
void radio_req_beep(uint8_t beep) { (void)beep; }
void radio_req_dmr_color_code(uint8_t cc) { (void)cc; }
void radio_req_dmr_focus(uint32_t id, bool private_call) { (void)id; (void)private_call; }
void radio_req_dmr_hear_all(bool on) { (void)on; }
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { (void)type; (void)key; }
void radio_req_dmr_target(uint32_t id, bool private_call) { (void)id; (void)private_call; }
void radio_req_dmr_target_clear(void) { }
void radio_req_dmr_timeslot(uint8_t ts) { (void)ts; }
void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel) { (void)ref; (void)sel; }
void radio_req_mode(uint8_t mode) { (void)mode; }
void radio_req_monitor(bool down) { (void)down; }
void radio_req_retune(void) { }
void radio_req_roam(uint8_t cmd) { (void)cmd; }
void radio_req_scan(uint8_t cmd) { (void)cmd; }
void radio_req_vfo_copy(void) { }
void radio_req_vfo_sfr(uint8_t a) { (void)a; }
void radio_req_vfo_step_size(uint32_t hz) { (void)hz; }
void radio_req_vibrate(uint8_t pattern) { (void)pattern; }
void radio_req_volume_hold(bool hold) { (void)hold; }
void radio_set_mic_gain(uint8_t step) { (void)step; }
bool radio_set_ref_trim(int32_t ppb) { (void)ppb; return false; }
void radio_set_settings(const radio_settings_t *s) { (void)s; }
radio_vfo_copy_t radio_vfo_copy_check(const channel_t *ch, bool in_vfo)
{
	radio_vfo_copy_t r;
	(void)ch;
	(void)in_vfo;
	memset(&r, 0, sizeof r);
	return r;
}
bool radio_volume_knob(void) { return false; }
bool radio_volume_soft(void) { return false; }
void settings_read(settings_t *out) { memset(out, 0, sizeof *out); }
bool ui_svc_m17_write_block(const uint8_t *dst, bool enc_off) { (void)dst; (void)enc_off; return false; }
bool ui_svc_save_talk_target(uint32_t id, bool private_call) { (void)id; (void)private_call; return false; }
int32_t ui_svc_setting_get(uint8_t id) { (void)id; return 0; }

static void reset(bool vfo, uint8_t kind)
{
	memset(&in_force, 0, sizeof in_force);
	memset(in_force.dst, 0xFF, sizeof in_force.dst);
	waits = false;
	posts = 0;
	memset(&view, 0, sizeof view);
	view.vfo = vfo;
	view.ch.mode = RADIO_MODE_M17;
	view.ch.kind = kind;
}

int main(void)
{
	static const uint8_t n0call[6] = { 0x00, 0x00, 0x4B, 0x13, 0xD1, 0x06 };	// m17.md 10

	// Boot: the CAN, then the voice, before the radio task's turn: both in force
	reset(true, CODEPLUG_KIND_NONE);
	ui_svc_apply(UI_SET_M17_CAN, 5);
	ui_svc_apply(UI_SET_M17_VOICE, 1);
	CHECK(posts == 2 && in_force.can == 0);	// nothing in force yet
	take();
	CHECK(in_force.can == 5 && in_force.tx_can == 5 && in_force.mode1600);

	// The destination, digital monitor and the CAN before one turn, on a channel switched to M17
	reset(false, CODEPLUG_KIND_NONE);
	CHECK(ui_svc_m17_talk_to(n0call, UI_TALK_TEMPORARY));
	CHECK(ui_svc_hear_all(true));
	ui_svc_apply(UI_SET_M17_CAN, 9);
	take();
	CHECK(memcmp(in_force.dst, n0call, 6) == 0 && in_force.squelch == M17_SQUELCH_ANY && in_force.can == 9 &&
	      !in_force.mode1600);

	// After the turn a change starts from the settings in force (no stale request kept)
	in_force.mode1600 = true;	// another writer (the debug link) since
	ui_svc_apply(UI_SET_M17_CAN, 2);
	take();
	CHECK(in_force.can == 2 && in_force.mode1600 && in_force.squelch == M17_SQUELCH_ANY);

	// A stored M17 channel takes its block: the settings are left alone
	reset(false, CODEPLUG_KIND_M17);
	CHECK(!ui_svc_m17_talk_to(n0call, UI_TALK_TEMPORARY) && !ui_svc_hear_all(true) && posts == 0);

	printf("uiSvcM17Test: %d checks, %s\n", checks, fails ? "FAILED" : "ok");
	return fails != 0;
}
