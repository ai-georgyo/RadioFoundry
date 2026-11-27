// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the host link leaves hotspot mode (see hostlink.h). DTR is only an edge: a clear DTR from the start (MMDVMHost
// on Windows) never ends the session; a set DTR that drops (a POSIX host closing the port) does
// (docs/spec/mmdvm-protocol.md 5.1, test T-USB-2). And when the host port gets a reattach after a transmission.
#include "hostlink/hostlink.h"

void hostlink_session_start(hostlink_session_t *s, bool connected)
{
	s->was_connected = connected;
}

hostlink_leave_t hostlink_session_check(hostlink_session_t *s, bool connected, bool present, bool exit_requested,
				       uint32_t idle_ms)
{
	bool dropped = s->was_connected && !connected;

	s->was_connected = connected;
	if (exit_requested) {
		return HOSTLINK_LEAVE_USER;
	}
	if (!present) {
		return HOSTLINK_LEAVE_GONE;
	}
	if (dropped) {
		return HOSTLINK_LEAVE_CLOSED;
	}
	if (idle_ms > HOSTLINK_HOTSPOT_IDLE_MS) {
		return HOSTLINK_LEAVE_IDLE;
	}
	return HOSTLINK_STAY;
}

bool hostlink_trial_failed(bool open, bool dropped, bool junk, uint32_t since_entry_ms)
{
	return !open && (dropped || junk || (since_entry_ms > HOSTLINK_HOTSPOT_OPEN_MS));
}

bool hostlink_reattach_check(hostlink_reattach_t *r, bool transmitting, hal_stream_host_t host, uint32_t now_ms)
{
	uint8_t before = r->was;

	r->was = (uint8_t)host;
	if (transmitting) {
		// The port may go at the key-up itself, before a pass sees the radio on the air: the pass before counts
		r->keyed = true;
		if (before > r->owed) {
			r->owed = before;
		}
		if ((uint8_t)host > r->owed) {
			r->owed = (uint8_t)host;
		}
		return false;
	}
	if (r->keyed) {
		r->keyed = false;
		r->end_ms = now_ms;
	}
	if (r->owed == HAL_STREAM_HOST_NONE) {
		return false;
	}
	if (host == HAL_STREAM_HOST_UP) {
		r->owed = HAL_STREAM_HOST_NONE;	// a host has it again by itself
		return false;
	}
	if ((uint32_t)(now_ms - r->end_ms) < HOSTLINK_REATTACH_WAIT_MS) {
		return false;
	}
	// A port a host had awake is lost unless it is awake again. One that slept through the key-up and is still
	// configured is as it was (a host that suspended it, a pulled cable): only one that lost its configuration since
	// (a host reset it and got no answer) is lost
	bool lost = (r->owed == HAL_STREAM_HOST_UP) || (host == HAL_STREAM_HOST_NONE);

	r->owed = HAL_STREAM_HOST_NONE;	// once: the next one needs a host to have had the port again
	return lost;
}

bool hostlink_port_after_tx(hostlink_reattach_t *r, const hal_stream_t *port, bool transmitting, uint32_t now_ms)
{
	if ((port->reattach == NULL) || (port->host == NULL)) {
		return false;
	}
	if (port->transmitting != NULL) {
		port->transmitting(transmitting);	// no try of a reattach resets the controller on the air
	}
	if (!hostlink_reattach_check(r, transmitting, port->host(), now_ms)) {
		return false;
	}
	port->reattach();
	return true;
}
