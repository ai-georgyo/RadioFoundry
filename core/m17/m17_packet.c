// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 packet mode (m17_packet.h; docs/spec/m17.md 8): the data type, the CRC run over the chunks as they come and go,
// and the frames' byte 25.
#include "config.h"

#if CONFIG_M17_PACKET

#include <string.h>

#include "m17/m17_packet.h"
#include "fec/crc.h"

#define CRC_POLY	0x5935u		// m17_crc's (m17.md 2.8), run here over the chunks one at a time
#define TYPE_MAX	0x1FFFFFu	// 2^21 - 1: what four UTF-8 bytes hold
#define FN_DONE		0xFFu		// rx: the packet ended (done or bad); a frame now is out of place

// UTF-8's form (m17.md 8.1, 13): the shortest one only, so each value has one encoding. Values D800-DFFF are numbers
// here, not characters, and are taken
uint32_t m17_pkt_type_get(const uint8_t *p, size_t n, uint8_t *len)
{
	static const uint32_t min[4] = { 0, 0x80u, 0x800u, 0x10000u };
	uint8_t k;
	uint32_t v;

	*len = 0;
	if (n == 0)
		return 0;
	if (p[0] < 0x80u)
		k = 1, v = p[0];
	else if ((p[0] & 0xE0u) == 0xC0u)
		k = 2, v = p[0] & 0x1Fu;
	else if ((p[0] & 0xF0u) == 0xE0u)
		k = 3, v = p[0] & 0x0Fu;
	else if ((p[0] & 0xF8u) == 0xF0u)
		k = 4, v = p[0] & 0x07u;
	else
		return 0;
	if (n < k)
		return 0;
	for (uint8_t i = 1; i < k; i++) {
		if ((p[i] & 0xC0u) != 0x80u)
			return 0;
		v = v << 6 | (p[i] & 0x3Fu);
	}
	if (v < min[k - 1])
		return 0;
	*len = k;
	return v;
}

// A type over 2^21 - 1 has no encoding: 0 bytes written
uint8_t m17_pkt_type_put(uint32_t type, uint8_t out[4])
{
	if (type < 0x80u) {
		out[0] = (uint8_t)type;
		return 1;
	}
	if (type < 0x800u) {
		out[0] = (uint8_t)(0xC0u | type >> 6);
		out[1] = (uint8_t)(0x80u | (type & 0x3Fu));
		return 2;
	}
	if (type < 0x10000u) {
		out[0] = (uint8_t)(0xE0u | type >> 12);
		out[1] = (uint8_t)(0x80u | ((type >> 6) & 0x3Fu));
		out[2] = (uint8_t)(0x80u | (type & 0x3Fu));
		return 3;
	}
	if (type <= TYPE_MAX) {
		out[0] = (uint8_t)(0xF0u | type >> 18);
		out[1] = (uint8_t)(0x80u | ((type >> 12) & 0x3Fu));
		out[2] = (uint8_t)(0x80u | ((type >> 6) & 0x3Fu));
		out[3] = (uint8_t)(0x80u | (type & 0x3Fu));
		return 4;
	}
	return 0;
}

// --- Receive ---
void m17_pkt_rx_start(m17_pkt_rx_t *r, uint8_t *buf, uint16_t cap)
{
	memset(r, 0, sizeof *r);
	r->buf = buf;
	r->cap = cap;
	r->crc = 0xFFFFu;
}

static uint8_t rx_end(m17_pkt_rx_t *r, uint8_t res)
{
	r->next = FN_DONE;
	return res;
}

// Every byte goes through the CRC, the first cap of them into buf too; which of them are the CRC is known only at the
// last frame, so kept is cut back then. Frames 0..31 carry a counter, the 33rd can only be the last (m17.md 8.2)
uint8_t m17_pkt_rx_frame(m17_pkt_rx_t *r, const uint8_t chunk[M17_PACKET_FRAME_BYTES])
{
	uint8_t b = chunk[M17_PACKET_CHUNK], cnt = (uint8_t)M17_PKT_COUNT(b), use;

	if (r->next == FN_DONE)
		return M17_PKT_BAD;
	if (b & M17_PKT_EOF) {
		if (cnt == 0 || cnt > M17_PACKET_CHUNK)
			return rx_end(r, M17_PKT_BAD);
		use = cnt;
	} else {
		if (cnt != r->next || r->next >= M17_PKT_FRAMES_MAX - 1u)
			return rx_end(r, M17_PKT_BAD);
		use = M17_PACKET_CHUNK;
		r->next++;
	}
	r->crc = fec_crc16(r->crc, CRC_POLY, chunk, use);
	if (r->kept < r->cap) {
		uint16_t n = (uint16_t)(r->cap - r->kept < use ? r->cap - r->kept : use);

		memcpy(r->buf + r->kept, chunk, n);
		r->kept = (uint16_t)(r->kept + n);
	}
	r->len = (uint16_t)(r->len + use);
	if (!(b & M17_PKT_EOF))
		return M17_PKT_MORE;
	// At least a one-byte data type before the CRC, and the CRC over all of it 0
	if (r->len < 3u || r->crc != 0)
		return rx_end(r, M17_PKT_BAD);
	if (r->kept > r->len - 2u)
		r->kept = (uint16_t)(r->len - 2u);
	r->cut = r->len - 2u > r->cap;
	return rx_end(r, M17_PKT_DONE);
}

// --- Transmit ---
void m17_pkt_tx_start(m17_pkt_tx_t *t, const uint8_t *data, uint16_t len)
{
	if (len > M17_PKT_DATA_MAX)
		len = M17_PKT_DATA_MAX;
	t->data = data;
	t->len = len;
	t->at = 0;
	t->crc = m17_crc(data, len);
	t->fn = 0;
}

// Data and CRC cut into 25-byte chunks; the last padded with zeros, its byte 25 the EOF bit and the bytes in use
bool m17_pkt_tx_frame(m17_pkt_tx_t *t, uint8_t chunk[M17_PACKET_FRAME_BYTES])
{
	uint16_t total = (uint16_t)(t->len + 2u), n;

	if (t->at >= total)
		return false;
	n = (uint16_t)(total - t->at);
	if (n > M17_PACKET_CHUNK)
		n = M17_PACKET_CHUNK;
	memset(chunk, 0, M17_PACKET_FRAME_BYTES);
	for (uint16_t k = 0; k < n; k++) {
		uint16_t i = (uint16_t)(t->at + k);

		if (i < t->len)
			chunk[k] = t->data[i];
		else
			chunk[k] = (uint8_t)(i == t->len ? t->crc >> 8 : t->crc);
	}
	t->at = (uint16_t)(t->at + n);
	if (t->at >= total) {
		chunk[M17_PACKET_CHUNK] = (uint8_t)(M17_PKT_EOF | n << 2);
	} else {
		chunk[M17_PACKET_CHUNK] = (uint8_t)((t->fn & 0x1Fu) << 2);
		t->fn++;
	}
	return true;
}

#endif
