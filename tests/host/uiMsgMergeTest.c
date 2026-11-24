// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The merged inbox (core/ui/ui_svc_msg.c) over two fake providers standing in for DMR and APRS: rows newest first, a
// tie to the provider first in the table, a merged row and a provider's own index map both ways, a send goes to the
// destination bearer's provider and a reply to the row's, unread sums. A provider's message is read only to compare
// ages while two inboxes have some left: a DMR read scans the codeplug's contacts, so one inbox alone costs none.
#include <stdio.h>
#include <string.h>
#include "ui/ui_svc.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// A fake inbox: ages newest first, the last send or reply it took
typedef struct {
	uint16_t n;
	uint32_t age[8];
	bool unread[8];
	int gets;		// get() calls
	int sent, replied, read, deleted;	// the last index read / deleted, how many sent / replied
	char last[16];
} fake_t;
static fake_t fd, fa;

static bool fake_get(fake_t *f, uint8_t net, uint16_t i, ui_msg_t *out)
{
	f->gets++;
	if (i >= f->n)
		return false;
	memset(out, 0, sizeof *out);
	out->peer.net = net;
	out->age_ms = f->age[i];
	out->unread = f->unread[i];
	snprintf(out->text, sizeof out->text, "%c%u", net == UI_NET_DMR ? 'd' : 'a', (unsigned)i);
	return true;
}
static bool fake_del(fake_t *f, uint16_t i)
{
	if (i >= f->n)
		return false;
	memmove(&f->age[i], &f->age[i + 1], (size_t)(f->n - i - 1u) * sizeof f->age[0]);
	memmove(&f->unread[i], &f->unread[i + 1], (size_t)(f->n - i - 1u) * sizeof f->unread[0]);
	f->n--;
	f->deleted = i;
	return true;
}
static uint16_t fake_unread(const fake_t *f)
{
	uint16_t u = 0;
	for (uint16_t i = 0; i < f->n; i++)
		u += f->unread[i];
	return u;
}

static uint16_t d_count(void) { return fd.n; }
static uint16_t d_unread(void) { return fake_unread(&fd); }
static bool d_get(uint16_t i, ui_msg_t *out) { return fake_get(&fd, UI_NET_DMR, i, out); }
static void d_read(uint16_t i) { fd.read = i; }
static bool d_del(uint16_t i) { return fake_del(&fd, i); }
static bool d_send(const ui_msg_to_t *to, const char *text) { (void)to; fd.sent++; strcpy(fd.last, text); return true; }
static bool d_reply(uint16_t i, const char *text) { fd.replied++; fd.read = i; strcpy(fd.last, text); return true; }
const ui_msg_provider_t ui_msg_dmr = { .net = UI_NET_DMR, .caps = UI_MSG_CAP_STATION, .count = d_count,
	.unread = d_unread, .get = d_get, .read = d_read, .del = d_del, .send = d_send, .reply = d_reply };

static uint16_t a_count(void) { return fa.n; }
static uint16_t a_unread(void) { return fake_unread(&fa); }
static bool a_get(uint16_t i, ui_msg_t *out) { return fake_get(&fa, UI_NET_APRS, i, out); }
static void a_read(uint16_t i) { fa.read = i; }
static bool a_del(uint16_t i) { return fake_del(&fa, i); }
static bool a_send(const ui_msg_to_t *to, const char *text) { (void)to; fa.sent++; strcpy(fa.last, text); return true; }
const ui_msg_provider_t ui_msg_aprs = { .net = UI_NET_APRS, .caps = UI_MSG_CAP_TO_CALL, .count = a_count,
	.unread = a_unread, .get = a_get, .read = a_read, .del = a_del, .send = a_send, .reply = NULL };

// The merged rows' texts, joined: "d0 a0 d1"
static const char *rows(void)
{
	static char s[64];
	s[0] = '\0';
	ui_msg_t m;
	for (uint16_t i = 0; ui_svc_msg_get(i, &m); i++) {
		if (s[0] != '\0')
			strcat(s, " ");
		strcat(s, m.text);
	}
	return s;
}

int main(void)
{
	// Nothing in either
	CHECK(ui_svc_msg_count() == 0 && ui_svc_msg_unread() == 0 && strcmp(rows(), "") == 0);
	CHECK(ui_svc_msg_of(0) == NULL && ui_svc_msg_row(UI_NET_DMR, 0) == UI_MSG_NO_ROW);
	CHECK(ui_svc_msg_nth(0) == &ui_msg_dmr && ui_svc_msg_nth(1) == &ui_msg_aprs && ui_svc_msg_nth(2) == NULL);
	CHECK(ui_svc_msg_net(UI_NET_APRS) == &ui_msg_aprs && ui_svc_msg_net(UI_NET_M17) == NULL);

	// One inbox alone: its order as it is, and no message read to get there
	fd = (fake_t){ .n = 3, .age = { 10, 20, 30 }, .unread = { true, false, true } };
	fd.gets = 0;
	uint16_t index = 0;
	uint8_t net = 0xFF;
	CHECK(ui_svc_msg_where(2, &net, &index) && net == UI_NET_DMR && index == 2);
	CHECK(ui_svc_msg_row(UI_NET_DMR, 2) == 2 && ui_svc_msg_row(UI_NET_DMR, 3) == UI_MSG_NO_ROW);
	CHECK(ui_svc_msg_of(3) == NULL);
	CHECK(fd.gets == 0);
	CHECK(strcmp(rows(), "d0 d1 d2") == 0 && ui_svc_msg_unread() == 2);

	// Both: merged by age, newest first; a tie goes to DMR (first in the table)
	fa = (fake_t){ .n = 3, .age = { 5, 20, 40 }, .unread = { true, true, false } };
	CHECK(ui_svc_msg_count() == 6 && ui_svc_msg_unread() == 4);
	CHECK(strcmp(rows(), "a0 d0 d1 a1 d2 a2") == 0);
	CHECK(ui_svc_msg_where(3, &net, &index) && net == UI_NET_APRS && index == 1);
	CHECK(ui_svc_msg_where(4, &net, &index) && net == UI_NET_DMR && index == 2);
	CHECK(!ui_svc_msg_where(6, &net, &index));
	CHECK(ui_svc_msg_row(UI_NET_APRS, 2) == 5 && ui_svc_msg_row(UI_NET_DMR, 0) == 1);
	CHECK(ui_svc_msg_row(UI_NET_APRS, 3) == UI_MSG_NO_ROW && ui_svc_msg_row(UI_NET_M17, 0) == UI_MSG_NO_ROW);
	// Once one inbox is used up the rest comes from the other without reading it
	fa.n = 1;	// a0 (5) only: d0 d1 d2 after it
	fd.gets = 0;
	CHECK(ui_svc_msg_row(UI_NET_DMR, 2) == 3);
	CHECK(fd.gets == 1);	// d0's age against a0's; d1 and d2 follow unread
	fa.n = 3;

	// Read, delete and reply reach row i's provider with its own index
	ui_svc_msg_read(3);
	CHECK(fa.read == 1);
	ui_svc_msg_read(2);
	CHECK(fd.read == 1);
	CHECK(ui_svc_msg_reply(4, "OK") && fd.replied == 1 && fd.read == 2 && strcmp(fd.last, "OK") == 0);
	CHECK(!ui_svc_msg_reply(0, "OK"));	// APRS's row: no reply in this fake
	CHECK(!ui_svc_msg_reply(6, "OK") && !ui_svc_msg_delete(6));
	CHECK(ui_svc_msg_delete(1) && fd.deleted == 0 && fd.n == 2);
	CHECK(strcmp(rows(), "a0 d0 a1 d1 a2") == 0);	// the fake renames by index: d1 is the old d2 (30)

	// A send goes to the destination's bearer
	ui_msg_to_t to;
	memset(&to, 0, sizeof to);
	ui_addr_set_dmr(&to.to, 9991234, false);
	CHECK(ui_svc_msg_send(&to, "Hi") && fd.sent == 1 && fa.sent == 0);
	memset(&to, 0, sizeof to);
	to.to.net = UI_NET_APRS;
	strcpy(to.call, "Q4WAB-9");
	CHECK(ui_svc_msg_send(&to, "Yo") && fa.sent == 1 && strcmp(fa.last, "Yo") == 0);
	to.to.net = UI_NET_M17;		// not built
	CHECK(!ui_svc_msg_send(&to, "No"));

	printf(failures ? "# uiMsgMergeTest FAILED (%d)\n" : "# uiMsgMergeTest PASSED\n", failures);
	return failures ? 1 : 0;
}

