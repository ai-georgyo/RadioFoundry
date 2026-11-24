// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's chip UID from the data flash's unique ID (platform/hrc7000/chip_uid.h): an ID of one repeated byte,
// whichever byte, is no ID (the entropy pool then runs without one, crypto.md 8.3, and the debug link's token isn't
// the 0 every such radio would share); one differing byte anywhere in the 16 makes it an ID; the fold's words.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../platform/hrc7000/chip_uid.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static const uint32_t untouched[3] = { 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u };

int main(void)
{
	uint8_t id[20];
	uint32_t uid[3];

	// One repeated byte, every value of it, at the drivers' 16 bytes and at other lengths: no ID, uid untouched
	unsigned refused = 0;
	for (unsigned v = 0; v < 256; v++) {
		memset(id, (int)v, sizeof id);
		for (uint32_t len = 0; len <= sizeof id; len++) {
			memcpy(uid, untouched, sizeof uid);
			bool ok = hrc_chip_uid_fold(id, len, uid);
			refused += !ok && memcmp(uid, untouched, sizeof uid) == 0;
		}
	}
	CHECK(refused == 256u * (sizeof id + 1u));
	memcpy(uid, untouched, sizeof uid);
	CHECK(!hrc_chip_uid_fold(NULL, 16, uid) && memcmp(uid, untouched, sizeof uid) == 0);

	// A byte past the 16 doesn't count: 16 equal bytes then another one is still no ID
	memset(id, 0xFF, sizeof id);
	id[16] = 0x12;
	CHECK(!hrc_chip_uid_fold(id, 17, uid));

	// One byte that differs, at each of the 16 places and against both all-0x00 and all-0xFF: an ID, and a different
	// one for each place but the 4 pairs the fold shares (byte k and byte 12 + k both land in uid[0]'s byte k)
	for (unsigned base = 0; base < 2; base++) {
		uint32_t got[16][3];
		for (unsigned i = 0; i < 16; i++) {
			memset(id, base ? 0xFF : 0x00, sizeof id);
			id[i] ^= 0x5A;
			CHECK(hrc_chip_uid_fold(id, 16, got[i]));
		}
		unsigned same = 0;
		for (unsigned i = 0; i < 16; i++) {
			for (unsigned j = i + 1; j < 16; j++) {
				same += memcmp(got[i], got[j], sizeof got[i]) == 0;
			}
		}
		CHECK(same == 4);
	}

	// The words: little endian groups of 4; uid[0] is bytes 0-3 XOR 12-15, so it moves with either end of the ID
	for (unsigned i = 0; i < 16; i++) {
		id[i] = (uint8_t)(0x10u + i);
	}
	CHECK(hrc_chip_uid_fold(id, 16, uid));
	CHECK(uid[0] == (0x13121110u ^ 0x1F1E1D1Cu) && uid[1] == 0x17161514u && uid[2] == 0x1B1A1918u);
	uint32_t t0 = uid[0];
	id[0] ^= 1;
	CHECK(hrc_chip_uid_fold(id, 16, uid) && uid[0] != t0);
	id[0] ^= 1;
	id[15] ^= 0x80;
	CHECK(hrc_chip_uid_fold(id, 16, uid) && uid[0] != t0);
	// A short ID: the missing bytes read 0
	CHECK(hrc_chip_uid_fold(id, 6, uid) && uid[0] == 0x13121110u && uid[1] == 0x1514u && uid[2] == 0);

	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
