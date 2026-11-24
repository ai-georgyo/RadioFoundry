// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The built air-framed modes' rows, and every mode's names (radio_air_modes.h). Built with the radio core in every
// configuration: the names serve FM and DMR builds too.
#include <stddef.h>
#include "radio/radio_air_modes.h"

#if CONFIG_AIR
// A host test's fake mode (tests/host/radioCoreTest.c), never in firmware: -DRADIO_AIR_TEST_ROW=<its row>
#ifdef RADIO_AIR_TEST_ROW
extern const radio_air_mode_t RADIO_AIR_TEST_ROW;
#endif

// In the order the radio's feature string lists the modes (build_modes_now). NULL ends it: a build may have no row
const radio_air_mode_t *const radio_air_modes[] = {
#if CONFIG_P25
	&radio_p25_mode,
#endif
#if CONFIG_NXDN
	&radio_nxdn_mode,
#endif
#if CONFIG_M17
	&radio_m17_mode,
#endif
#ifdef RADIO_AIR_TEST_ROW
	&RADIO_AIR_TEST_ROW,
#endif
	NULL
};
const uint8_t radio_air_mode_count = (uint8_t)(sizeof(radio_air_modes) / sizeof(radio_air_modes[0]) - 1u);

const radio_air_mode_t *radio_air_mode_for(uint8_t mode)
{
	for (const radio_air_mode_t *const *m = radio_air_modes; *m != NULL; m++) {
		if ((*m)->mode == mode)
			return *m;
	}
	return NULL;
}
#endif

// FM's and DMR's words, else the row's: the key (name false) or the display name
static const char *mode_word(uint8_t mode, bool name)
{
	static const char *const fm_dmr[2][2] = { { "fm", "FM" }, { "dmr", "DMR" } };

	if (mode <= RADIO_MODE_DMR)
		return fm_dmr[mode][name];
#if CONFIG_AIR
	const radio_air_mode_t *m = radio_air_mode_for(mode);
	if (m != NULL)
		return name ? m->name : m->key;
#endif
	return "?";
}

const char *radio_mode_key(uint8_t mode)
{
	return mode_word(mode, false);
}

const char *radio_mode_name(uint8_t mode)
{
	return mode_word(mode, true);
}
