// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RADIO op 44, the owner's transmit arm (dl_txarm.h)
#include "debuglink/dl_txarm.h"
#include "debuglink/dl_frame.h"

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

size_t dl_tx_arm_op(const hal_radio_t *r, const uint8_t *args, uint16_t len, uint8_t *out)
{
	if (r == NULL || r->tx_arm_info == NULL || r->tx_arm == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (len == 0) {
		hal_tx_arm_info_t info = { 0 };
		r->tx_arm_info(&info);
		out[0] = DL_ST_OK;
		out[1] = info.stage;
		out[2] = info.max;
		put32(out + 3, info.addr);
		put32(out + 7, info.len);
		put32(out + 11, info.crc);
		out[15] = info.cal_ok ? 1u : 0u;
		return DL_TX_ARM_INFO_LEN;
	}
	if (len != 5) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	// The id as the host sent it: the board compares it with the calibration in use, nothing here does
	const uint32_t id = (uint32_t)args[1] | (uint32_t)args[2] << 8 | (uint32_t)args[3] << 16 | (uint32_t)args[4] << 24;
	const uint8_t why = r->tx_arm(args[0], id);
	out[0] = why == HAL_TX_ARM_OK ? DL_ST_OK : DL_ST_REFUSED;
	out[1] = why;
	return DL_TX_ARM_REPLY_LEN;
}
