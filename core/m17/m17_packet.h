// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 packet mode (m17.md 8): a packet's data type (written as UTF-8 writes a code point), payload and CRC cut into
// 25-byte chunks, one a frame, and put together again. The receiver checks the CRC as the chunks come (a packet may be
// 825 bytes; the radio keeps only what it shows), so its buffer can be shorter than the packet. SMS (type 5) is UTF-8
// text with a terminating zero. Pure functions on the caller's memory. Built with CONFIG_M17_PACKET.
#ifndef RFOUNDRY_M17_PACKET_H
#define RFOUNDRY_M17_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "m17_frame.h"

#define M17_PKT_DATA_MAX	823u	// data type and payload
#define M17_PKT_MAX		825u	// ... and the CRC
#define M17_PKT_FRAMES_MAX	33u
#define M17_PKT_EOF		0x80u	// byte 25: the last frame
#define M17_PKT_COUNT(b)	(((b) >> 2) & 0x1Fu)	// byte 25: frame counter, or in the last frame the bytes in use

// Data types (m17.md 8.1)
enum { M17_PKT_RAW = 0, M17_PKT_AX25 = 1, M17_PKT_APRS = 2, M17_PKT_6LOWPAN = 3, M17_PKT_IPV4 = 4, M17_PKT_SMS = 5,
       M17_PKT_WINLINK = 6 };

// The data type at the start of a packet: its value, and how many bytes it took (0: malformed)
uint32_t m17_pkt_type_get(const uint8_t *p, size_t n, uint8_t *len);
// Write type as 1 to 4 bytes; returns how many
uint8_t m17_pkt_type_put(uint32_t type, uint8_t out[4]);

// --- Receive ---
enum { M17_PKT_MORE, M17_PKT_DONE, M17_PKT_BAD };
typedef struct {
	uint8_t *buf;		// the caller's: the first cap bytes of type and payload are kept here
	uint16_t cap;
	uint16_t kept;		// bytes in buf
	uint16_t len;		// bytes of type, payload and CRC received
	uint16_t crc;		// running over everything received
	uint8_t next;		// the frame counter due
	bool cut;		// more came than cap: buf holds the start only
} m17_pkt_rx_t;

void m17_pkt_rx_start(m17_pkt_rx_t *r, uint8_t *buf, uint16_t cap);
// One frame's 26 bytes (m17_frame_packet_get). M17_PKT_DONE: the last frame came and the CRC holds (kept bytes are
// the data type and payload, without the CRC); M17_PKT_BAD: a counter out of order, too many frames, a bad byte count
// or a CRC failure (start again at the next packet)
uint8_t m17_pkt_rx_frame(m17_pkt_rx_t *r, const uint8_t chunk[M17_PACKET_FRAME_BYTES]);

// --- Transmit ---
typedef struct {
	const uint8_t *data;	// data type and payload, the caller's until the last frame is out
	uint16_t len;		// at most M17_PKT_DATA_MAX
	uint16_t at;		// bytes of data and CRC sent
	uint16_t crc;
	uint8_t fn;
} m17_pkt_tx_t;

void m17_pkt_tx_start(m17_pkt_tx_t *t, const uint8_t *data, uint16_t len);
// The next frame's 26 bytes; false when the packet is all out
bool m17_pkt_tx_frame(m17_pkt_tx_t *t, uint8_t chunk[M17_PACKET_FRAME_BYTES]);
// Frames a packet of len data bytes takes
static inline uint8_t m17_pkt_frames(uint16_t len)
{
	return (uint8_t)((len + 2u + M17_PACKET_CHUNK - 1u) / M17_PACKET_CHUNK);
}

#endif
