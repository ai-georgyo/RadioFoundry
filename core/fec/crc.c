// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CRCs of up to 16 bits, MSB first (crc.h). One bit a step: a frame's CRC covers at most a few hundred bytes, so a
// table's flash buys nothing the modes need.
#include "crc.h"

uint16_t fec_crc_bits(const uint8_t *bits, uint16_t n, uint8_t width, uint16_t poly, uint16_t init)
{
	uint16_t mask = (uint16_t)((1u << width) - 1u), top = (uint16_t)(1u << (width - 1u)), r = (uint16_t)(init & mask);

	for (uint16_t i = 0; i < n; i++) {
		// The data bit enters at the top: the feedback is it XOR the bit leaving the register
		uint8_t fb = (uint8_t)(((bits[i >> 3] >> (7 - (i & 7))) & 1u) ^ ((r & top) ? 1u : 0u));

		r = (uint16_t)((r << 1) & mask);
		if (fb)
			r = (uint16_t)(r ^ (poly & mask));
	}
	return r;
}

uint16_t fec_crc16(uint16_t crc, uint16_t poly, const uint8_t *p, size_t n)
{
	while (n--) {
		crc = (uint16_t)(crc ^ (uint16_t)(*p++ << 8));
		for (uint8_t k = 0; k < 8; k++)
			crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ poly : crc << 1);
	}
	return crc;
}
