// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on the radio's screens (docs/ux/m17.md): the pieces the shared screens use (Home, the call screen, Recent,
// actions, the picker, the channel editor, Settings) and M17's own pages (ui_m17.c: Call Details, the signing key, the
// station keys, the BER test; the callsign entry is the text editor's, ui_menus.c). Inside the UI only, built with
// CONFIG_M17; every sub-feature's part with its switch (UI_M17_* says whether this radio runs it)
#ifndef UI_M17_H
#define UI_M17_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "fmt.h"
#include "i18n.h"
#include "ui_svc.h"
#include "ui_screens.h"

#if CONFIG_M17
#include "m17/m17.h"
#include "radio/radio_m17.h"

// An address as the screens show it: a callsign, "All call" for broadcast, "#" and 12 hex digits for an extended
// one (m17.md 13). ui_screens.c
void ui_sb_m17(sbuf_t *b, const uint8_t a[M17_ADDR_BYTES]);
// A signature's word after a call (RADIO_M17_SIG_*): long and short forms as text alternates; *warn: say it in the
// warning color. TR_NONE: nothing to say (unsigned, no key). ui_screens.c
tr_id_t ui_m17_sig_word(uint8_t sig, bool *warn);
// The last call heard in full (radio_m17_details) into the UI's shared scratch, or NULL when there is none; valid
// until the next user of the scratch (ui_menus.c ui_scratch). ui_m17.c
const radio_m17_details_t *ui_m17_details(void);
// The sub-features this radio runs now (ui_svc_m17_caps), each only where its switch is built
static inline bool ui_m17_has(uint8_t cap) { return (ui_svc_m17_caps() & cap) != 0; }

// The callsign entry (TEXT_SET_CALLSIGN, ui_menus.c): opened for our callsign, for a destination (the picker's Type a
// callsign..., the channel editor's), or for a message's recipient
// ... or for the reflector's entries (CONFIG_NET_M17: the reflector and module, its address, its port; in this order)
enum { UI_CALL_OWN, UI_CALL_DST, UI_CALL_EDITOR, UI_CALL_MSG, UI_CALL_REFL, UI_CALL_REFL_ADDRESS, UI_CALL_REFL_PORT };
void ui_callsign_open(uint8_t why);

// The call text's entry (printable ASCII, 52 characters), kept in the radio-wide record; ui_menus.c
void ui_call_text_open(void);
// A Settings row's value (docs/ux/m17.md 11): our callsign (0), the call text (1), the signing key (2), the station
// keys (3); ui_m17.c
void ui_m17_set_value(uint8_t row, ui_item_t *it);
// The M17 text pages' scroll (ui_m17.c; ui_heard.c's page too): one of them is open at a time
extern uint16_t ui_m17_page_first, ui_m17_page_max;
// Every UI pass with the screen on top (top: its ui_scr_t): the BER test's mode given back once its page is gone
void ui_m17_tick(uint8_t top);
// The settings rows' radio-wide record (our callsign, the call text, the two choices), read into the scratch
m17_wide_t *ui_m17_wide(void);
#if CONFIG_NET_M17
// The reflector (ui_m17.c; docs/spec/m17-ip.md 7): Home's place line while linked ("Linked M17-M17 C"; false: not
// linked), linked at all (the picker's ECHO and INFO), and the reflector's entries: their text to open with, the check
// before OK keeps it (false: a toast said why), and the value kept
bool ui_m17_refl_place(char *buf, size_t len);
bool ui_m17_refl_linked(void);
void ui_m17_refl_text(uint8_t why, char *t, size_t len);
bool ui_m17_refl_check(uint8_t why, const char *t);
void ui_m17_refl_kept(uint8_t why, const char *t);
#endif
#endif

#endif
