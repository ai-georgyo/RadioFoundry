// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CRCs of up to 16 bits, MSB first, no reflection, no final XOR: the register starts at init and the data's bits shift
// in from the most significant of the first byte. What the air-framed modes use: NXDN's (any width, init all ones,
// TS 1-A 4.5.5) and M17's (width 16, poly 0x5935, init 0xFFFF, docs/spec/m17.md 2.8). Bit-serial: no table, a few
// hundred bytes a frame at most. Reentrant. Built with the air interface (core/fec).
#ifndef FEC_CRC_H
#define FEC_CRC_H

#include <stddef.h>
#include <stdint.h>

// n bits of a packed bit string (bit 0 = bit 7 of byte 0), width 1..16, poly without its top term
uint16_t fec_crc_bits(const uint8_t *bits, uint16_t n, uint8_t width, uint16_t poly, uint16_t init);
// Whole bytes, width 16, continuing from crc (start with the mode's init): a CRC over data in pieces
uint16_t fec_crc16(uint16_t crc, uint16_t poly, const uint8_t *p, size_t n);

#endif
