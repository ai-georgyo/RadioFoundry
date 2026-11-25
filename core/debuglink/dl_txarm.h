// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RADIO op 44, the owner's transmit arm (docs/spec/debuglink.md 3.8, radio-features.md 6.3): its parsing and replies
// over hal_radio_t.tx_arm_info and tx_arm, the portable part (host tested). The checks are the board's.
#ifndef DL_TXARM_H
#define DL_TXARM_H

#include <stddef.h>
#include <stdint.h>
#include "hal_radio.h"

#define DL_TX_ARM_INFO_LEN	16u	// the info reply: status, stage, max, addr u32, len u32, crc u32, ok
#define DL_TX_ARM_REPLY_LEN	2u	// an arm's reply: status, reason (HAL_TX_ARM_*)

// args and len: the request after the op byte. No argument: the info (DL_ST_OK, then stage u8, max u8, the range's
// address u32 and length u32, the CRC-32 u32 of the calibration in use, ok u8). stage u8 and id u32: the arm
// (DL_ST_OK and reason 0, or DL_ST_REFUSED and the reason). Any other length: DL_ST_BAD_ARG. A radio without both
// hooks: DL_ST_UNAVAILABLE. out holds at least DL_TX_ARM_INFO_LEN bytes; returns the reply's length
size_t dl_tx_arm_op(const hal_radio_t *r, const uint8_t *args, uint16_t len, uint8_t *out);

#endif
