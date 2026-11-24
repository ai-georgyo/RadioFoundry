// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One inbox over every mode that carries text (docs/spec/ui.md 2.6): each mode is a provider (ui_msg_provider_t) with
// its own inbox, newest first, and the Messages screens see their messages merged by age, newest first. A merged row
// is an index here; send goes to the provider of the destination's bearer, reply to the provider of the row.
// Providers are in the table only with their switches; the UI simulator defines them itself.
#include <stddef.h>
#include "ui_svc.h"
#include "config.h"

// In table order, which breaks a tie in age (the order the screens merged them in before: DMR, then APRS)
static const ui_msg_provider_t *const providers[] = {
#if CONFIG_DMR_SMS
	&ui_msg_dmr,
#endif
#if CONFIG_APRS_AFSK
	&ui_msg_aprs,
#endif
#if CONFIG_M17_PACKET
	&ui_msg_m17,
#endif
	NULL
};
#define N_PROVIDERS	(sizeof providers / sizeof providers[0] - 1u)
_Static_assert(N_PROVIDERS <= 8, "a merge cursor per provider");

const ui_msg_provider_t *ui_svc_msg_nth(uint8_t n)
{
	for (uint8_t k = 0; providers[k] != NULL; k++)	// not n < N_PROVIDERS: none built makes that always false
		if (k == n)
			return providers[k];
	return NULL;
}

const ui_msg_provider_t *ui_svc_msg_net(uint8_t net)
{
	for (uint8_t n = 0; providers[n] != NULL; n++)
		if (providers[n]->net == net)
			return providers[n];
	return NULL;
}

uint16_t ui_svc_msg_count(void)
{
	uint32_t n = 0;
	for (uint8_t p = 0; providers[p] != NULL; p++)
		n += providers[p]->count();
	return (uint16_t)(n > UI_MSG_INDEX_MAX ? UI_MSG_INDEX_MAX : n);
}

uint16_t ui_svc_msg_unread(void)
{
	uint16_t n = 0;
	for (uint8_t p = 0; providers[p] != NULL; p++)
		n = (uint16_t)(n + providers[p]->unread());
	return n;
}

// Walk the merged order to row, or to provider want's message want_index (want not NULL): which provider and index
// it is, and its merged row. Each inbox is newest first, so the newest of the providers' next messages comes next; a
// tie goes to the earlier provider. A message is read only to compare ages, while two inboxes or more have some left:
// a DMR message's read looks its sender up in the contacts, a scan of the codeplug, so one inbox alone (the usual
// case) costs no read at all. One message at a time in scratch, which the caller owns: a copy per provider at once
// would put the list on the UI task's deepest chain. NULL: past the end
static const ui_msg_provider_t *walk(uint16_t row, const ui_msg_provider_t *want, uint16_t want_index,
				     ui_msg_t *scratch, uint16_t *index, uint16_t *at)
{
	uint16_t next[N_PROVIDERS + 1u] = { 0 }, n[N_PROVIDERS + 1u];
	uint8_t left = 0;	// the inboxes with messages left
	for (uint8_t p = 0; providers[p] != NULL; p++) {
		n[p] = providers[p]->count();
		left += n[p] != 0;
	}
	for (uint16_t r = 0; r < UI_MSG_INDEX_MAX; r++) {
		uint8_t best = N_PROVIDERS;	// none yet
		uint32_t best_age = 0;
		for (uint8_t p = 0; providers[p] != NULL; p++) {
			if (next[p] >= n[p] || (left > 1 && !providers[p]->get(next[p], scratch)))
				continue;
			uint32_t age = left > 1 ? scratch->age_ms : 0;	// one inbox left: its next comes next
			if (best == N_PROVIDERS || age < best_age) {
				best = p;
				best_age = age;
			}
		}
		if (best == N_PROVIDERS)
			return NULL;	// every inbox ends here
		if (want != NULL ? providers[best] == want && next[best] == want_index : r == row) {
			*index = next[best];
			*at = r;
			return providers[best];
		}
		if (++next[best] == n[best])
			left--;
	}
	return NULL;
}

// Row i's provider and its index there (NULL: no such row)
static const ui_msg_provider_t *at(uint16_t row, uint16_t *index)
{
	ui_msg_t m;
	uint16_t r;
	return walk(row, NULL, 0, &m, index, &r);
}

bool ui_svc_msg_get(uint16_t i, ui_msg_t *out)
{
	uint16_t index, r;
	const ui_msg_provider_t *p = walk(i, NULL, 0, out, &index, &r);
	return p != NULL && p->get(index, out);
}

void ui_svc_msg_read(uint16_t i)
{
	uint16_t index;
	const ui_msg_provider_t *p = at(i, &index);
	if (p != NULL)
		p->read(index);
}

bool ui_svc_msg_delete(uint16_t i)
{
	uint16_t index;
	const ui_msg_provider_t *p = at(i, &index);
	return p != NULL && p->del(index);
}

bool ui_svc_msg_send(const ui_msg_to_t *to, const char *text)
{
	const ui_msg_provider_t *p = ui_svc_msg_net(to->to.net);
	return p != NULL && p->send != NULL && p->send(to, text);
}

bool ui_svc_msg_reply(uint16_t i, const char *text)
{
	uint16_t index;
	const ui_msg_provider_t *p = at(i, &index);
	return p != NULL && p->reply != NULL && p->reply(index, text);
}

const ui_msg_provider_t *ui_svc_msg_of(uint16_t i)
{
	uint16_t index;
	return at(i, &index);
}

bool ui_svc_msg_where(uint16_t i, uint8_t *net, uint16_t *index)
{
	const ui_msg_provider_t *p = at(i, index);
	if (p == NULL)
		return false;
	*net = p->net;
	return true;
}

uint16_t ui_svc_msg_row(uint8_t net, uint16_t i)
{
	ui_msg_t m;
	uint16_t index, row;
	const ui_msg_provider_t *p = ui_svc_msg_net(net);
	return p != NULL && walk(0, p, i, &m, &index, &row) != NULL ? row : UI_MSG_NO_ROW;
}
