// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A channel's sub-audio selection as the software detector and generator take it (drivers/rf/subaudio/subaudio_sel.h):
// the kind for every tone type, and when two selections are the same one, so the detector is kept
#include <stdio.h>
#include "rf/subaudio/subaudio_sel.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	const subtone_t none = { TONE_NONE, 0 };
	const subtone_t ctcss = { TONE_CTCSS, 1000 };
	const subtone_t dcs = { TONE_DCS, 19 };
	const subtone_t dcs_inv = { TONE_DCS_INV, 19 };

	// The kind for each type; a type nobody knows and no selection at all are no tone
	CHECK(subaudio_kind(&none) == SUBAUDIO_NONE);
	CHECK(subaudio_kind(&ctcss) == SUBAUDIO_CTCSS);
	CHECK(subaudio_kind(&dcs) == SUBAUDIO_DCS);
	CHECK(subaudio_kind(&dcs_inv) == SUBAUDIO_DCS_INV);
	CHECK(subaudio_kind(NULL) == SUBAUDIO_NONE);
	for (unsigned type = TONE_DCS_INV + 1u; type < 256u; type++) {
		const subtone_t odd = { (uint8_t)type, 1000 };
		CHECK(subaudio_kind(&odd) == SUBAUDIO_NONE);
	}
	// The value plays no part in the kind
	CHECK(subaudio_kind(&(subtone_t){ TONE_CTCSS, 0 }) == SUBAUDIO_CTCSS);
	CHECK(subaudio_kind(&(subtone_t){ TONE_NONE, 1000 }) == SUBAUDIO_NONE);

	// The same selection: kind and value
	CHECK(subaudio_same(&ctcss, &ctcss));
	CHECK(subaudio_same(&ctcss, &(subtone_t){ TONE_CTCSS, 1000 }));
	CHECK(subaudio_same(&dcs, &(subtone_t){ TONE_DCS, 19 }));
	CHECK(subaudio_same(&dcs_inv, &(subtone_t){ TONE_DCS_INV, 19 }));
	// Another tone or code
	CHECK(!subaudio_same(&ctcss, &(subtone_t){ TONE_CTCSS, 1035 }));
	CHECK(!subaudio_same(&dcs, &(subtone_t){ TONE_DCS, 21 }));
	// Another kind with the same number, the two DCS polarities among them
	CHECK(!subaudio_same(&dcs, &dcs_inv));
	CHECK(!subaudio_same(&dcs_inv, &dcs));
	CHECK(!subaudio_same(&ctcss, &(subtone_t){ TONE_DCS, 1000 }));
	CHECK(!subaudio_same(&ctcss, &none));
	CHECK(!subaudio_same(&none, &dcs));

	// No tone is no tone: whatever value is left in it, a type nobody knows, or no selection
	CHECK(subaudio_same(&none, &none));
	CHECK(subaudio_same(&none, &(subtone_t){ TONE_NONE, 885 }));
	CHECK(subaudio_same(&none, &(subtone_t){ 0x7F, 1000 }));
	CHECK(subaudio_same(NULL, NULL));
	CHECK(subaudio_same(NULL, &none));
	CHECK(subaudio_same(&none, NULL));
	CHECK(!subaudio_same(NULL, &ctcss));
	CHECK(!subaudio_same(&dcs, NULL));

	// Either order gives the same answer
	const subtone_t *all[] = { NULL, &none, &ctcss, &dcs, &dcs_inv };
	for (unsigned i = 0; i < 5; i++) {
		for (unsigned k = 0; k < 5; k++) {
			CHECK(subaudio_same(all[i], all[k]) == subaudio_same(all[k], all[i]));
			CHECK(subaudio_same(all[i], all[k]) == (i == k || (i < 2 && k < 2)));
		}
	}

	printf("subaudioSelTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
