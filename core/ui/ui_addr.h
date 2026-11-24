// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An address as it travels in the UI: a station or a group of any mode (targets, Recent, messages, banners). The text
// is made only where it is drawn (ui_sb_addr, ui_nav.h). Its own header so the drawing side (ui_screens.h) needs no
// services
#ifndef UI_ADDR_H
#define UI_ADDR_H

#include <stdint.h>
#include <stdbool.h>

enum { UI_ADDR_NONE, UI_ADDR_DMR_ID, UI_ADDR_DMR_TG, UI_ADDR_CALL };
enum { UI_NET_DMR, UI_NET_M17, UI_NET_APRS };	// the bearer that reaches it
#define UI_ADDR_ALL_CALL	0xFFFFFFu	// the DMR ID that is everyone (either kind)
// The kind and the bearer, then the number or the callsign over the same bytes. Two views of one 8-byte layout: a
// struct of kind, net and a union {id, call[6]} would align the union to 4 and take 12
typedef union {
	struct {
		uint8_t kind;		// UI_ADDR_*
		uint8_t net;		// UI_NET_*
		uint8_t call[6];	// UI_ADDR_CALL: a 48-bit base-40 address, most significant byte first (m17.md 10)
	};
	struct {
		uint8_t kind_net_[4];	// kind, net and call[0..1]: not used by an ID
		uint32_t id;		// UI_ADDR_DMR_ID, UI_ADDR_DMR_TG: the 24-bit ID (over call[2..5])
	};
} ui_addr_t;
_Static_assert(sizeof(ui_addr_t) == 8, "ui_addr_t is 8 bytes");

// A DMR person or talkgroup: what every target was before, an ID and a group flag. In place: an 8-byte struct would
// come back through memory. call[0..1] stay as they are: an ID's address never reads them (ui_addr_eq)
static inline void ui_addr_set_dmr(ui_addr_t *a, uint32_t id, bool group)
{
	a->kind = group ? UI_ADDR_DMR_TG : UI_ADDR_DMR_ID;
	a->net = UI_NET_DMR;
	a->id = id;
}
static inline bool ui_addr_group(const ui_addr_t *a) { return a->kind == UI_ADDR_DMR_TG; }
// The same station or group: kind, bearer and number (a talkgroup and a person of one ID differ). ui_app.c
bool ui_addr_eq(const ui_addr_t *a, const ui_addr_t *b);

#endif
