// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32
// radios"; the codec: docs/spec/codec-cs7000p.md, "Entry points and calling convention" and "Hosting").
//
// The merge on the user's machine (rfoundry, tools/codec_extract.py, the web flasher) takes the codec from the maker's
// DMR program file V9.00.96 and puts all of it in sector 7, the last sector of flash bank 1:
// - the three code ranges as one window at their stock distances, from 0x080E0004 (0x15C98 bytes);
// - the two code islands and the twenty table windows right after it, each at its stock address modulo 16, joined
//   into one block (0x080F5CA0, 0x3E83 bytes, 0xFF between two windows): the descriptor's one table window;
// - the descriptor at the end of the sector (platform/stm32/codec_desc.h).
// The merge rewrites every address in the codec that points into a moved window (the codec map). It then writes 0xFF
// over every span of a window that the page calls not the codec's: the maker's own code between the code ranges, and
// the maker's small pieces inside the ranges and inside three table windows. Both CRC-32s below are of the bytes after
// that, so a codec with any of those spans left in place is refused.
// None of it is part of this repository or a release. The calls and the checks are the shared ones
// (platform/stm32/hrc/vocoder.c); this file says which build and where. Without a valid descriptor and codec,
// present() is false and DMR voice is off ("No codec: voice off"); DMR data, the hotspot and FM don't need the codec.
// On one radio the boot check passes and decode runs; encode has not run on a radio (docs/testing/cs7000p-bringup.md).
//
// Nothing of the board is included here: the host test compiles this file as it is
// (boards/cs7000p/tests/cs7000pCodecTest.c).
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_TAG		0x00090096u	// the descriptor's tag for the maker's V9.00.96
#define CODEC_FLASH_LO		0x080E0000u	// sector 7, the last of bank 1: the flash memory.ld leaves to the codec

// The V9.00.96 codec as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of
// the code window and of the block after the rewrites and the blanking. The descriptor must carry exactly these.
// Both are from the merge tool's own run on the maker's file, after the three CRC-32s that follow from the page
// alone had matched (tools/codec_extract.py --placed)
#define CODEC_CODE		0x080E0004u
#define CODEC_CODE_LEN		0x15C98u
#define CODEC_CRC		0xA6BAF942u
#define CODEC_WINDOWS		1u

static const codec_desc_window_t tables[CODEC_WINDOWS] = {
	{ 0x080F5CA0u, 0x3E83u, 0x1DD4537Cu },	// island 1, island 2 and the 20 table windows as one block
};

// The id tools/rfoundry and the release manifests use for a codec placed as this profile says (the descriptor and
// both CRCs matched; stockfw.py CS7000P_CODEC)
#define CODEC_ID		"cs7000p-v90096-s7"

static const stm32_codec_profile_t v90096 = {
	.tag = CODEC_TAG, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = CODEC_CODE, .code_len = CODEC_CODE_LEN, .code_crc = CODEC_CRC, .tables = tables, .windows = CODEC_WINDOWS,
	// Not used: encode ECC +0x514, FEC decode +0xAE0. Our air frame FEC (core/dmr/ambe) matches the codec's
	.at = { .encode = 0x14C, .decode = 0x604, .enc_init = 0x294, .fec_init = 0x4FC, .dec_init = 0x6F0,
		.enc_params = 0x854, .enc_set = 0x1E0, .dec_set = 0x6AC },
};

static const stm32_codec_profile_t *const profiles[] = { &v90096 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 1 };

#endif	// CONFIG_DMR
