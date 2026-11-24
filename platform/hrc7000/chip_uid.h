// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's chip UID (system.c): the board's data flash's unique ID folded into hal_chip_t.uid's three words.
// Plain code, so the host test runs it (tests/host/hrc7000ChipUidTest.c).
#ifndef PLATFORM_HRC7000_CHIP_UID_H
#define PLATFORM_HRC7000_CHIP_UID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HRC_CHIP_UID_BYTES	16u	// the ID bytes that count; any past them are ignored

// The first 16 bytes of id into uid: all 16 count, uid[0] (the debug link's REBOOT and CLAIM token) being bytes 0-3
// XOR 12-15, so it differs whichever end of the ID varies. False, uid untouched, for no ID or one of a single repeated
// byte (all 0x00, all 0xFF: a bus that read nothing, a part without one). The radio then has no chip ID: the entropy
// pool doesn't take a constant every such radio shares (docs/spec/crypto.md 8.3), and the token isn't 0
static inline bool hrc_chip_uid_fold(const uint8_t *id, uint32_t len, uint32_t uid[3])
{
	uint32_t w[4] = { 0 };
	bool varies = false;
	if (id == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < len && i < HRC_CHIP_UID_BYTES; i++) {
		w[i / 4] |= (uint32_t)id[i] << (8u * (i % 4));
		varies = varies || id[i] != id[0];
	}
	if (!varies) {
		return false;
	}
	uid[0] = w[0] ^ w[3];
	uid[1] = w[1];
	uid[2] = w[2];
	return true;
}

#endif
