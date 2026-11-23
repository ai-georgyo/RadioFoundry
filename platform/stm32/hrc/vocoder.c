// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock AMBE+2 codec run in place from flash (vocoder.h; the calls: docs/spec/codec-dm1701.md, "Codec state init
// and calls"). The code reaches its tables through absolute addresses, which the merge keeps; where it moves the code it
// rewrites what points into it. Without a valid descriptor and codec, present() is false and the radio is FM only.
//
// Not reentrant: one task (the audio task, through the audio block callback) makes all calls. The codec needs about
// 4 KB of the caller's stack (3.8 to 4.0 KB measured on the builds in use).
#include "hrc/vocoder.h"

#if CONFIG_DMR	// the whole file: DMR only
#include <string.h>
#include "platform.h"
#include "codec_call.h"
#include "codec_cpu.h"
#include "vocoder/ambe2_flags.h"

// codec_cpu.h has the trap bits by number, for the host tests: the numbers are the core header's
_Static_assert(CODEC_CCR_TRAPS == (SCB_CCR_UNALIGN_TRP_Msk | SCB_CCR_DIV_0_TRP_Msk), "CCR trap bits");

#define CODEC_SIGNATURE		0x4FF0E92Du	// push.w {r4-r11, lr}

// State sizes found by running the stock code with each block ending at a fault boundary: the codec touches
// D[0, 0x7EC), E[0, 0x16E0), its tables, its arguments and the stack, nothing else. The same in every build
#define DEC_STATE_BYTES		0x7ECu
#define ENC_STATE_BYTES		0x16E0u
#define FEC_STATE_BYTES		0x44u
#define STATE_BYTES		(ENC_STATE_BYTES > DEC_STATE_BYTES ? ENC_STATE_BYTES : DEC_STATE_BYTES)

#define HALF			(VOCODER_FRAME_SAMPLES / 2)

// CPU-only RAM (common.ld's .ccmram), all the codec needs; not zeroed by the start-up code (enc_start(), dec_start()).
// One block holds the encoder's state or the decoder's, never both: DMR and NXDN voice are simplex, so a call only
// encodes or only decodes (core/dmr/ambe/ambeVoice.c decodes nothing while a transmission runs). Each init touches only
// its own block (codec-dm1701.md, "State sizes"), so the first encode after decoding, or the first decode after
// encoding, builds its half fresh, as reset() did at the start of every call. Saves the 2 KB a second block took
static uint32_t state[STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t fec_state[FEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static int16_t bitbuf[72] __attribute__((section(".ccmram"), aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((section(".ccmram"), aligned(4)));

static int8_t presence = -1;			// -1 not checked yet
static const stm32_codec_profile_t *prof;	// the build found
static uint32_t base;				// the code's start, from the descriptor
enum { HELD_NONE, HELD_ENC, HELD_DEC };
static uint8_t held = HELD_NONE;		// what the state block holds now; NONE after reset()
static uint32_t enc_flags = AMBE2_ENC_DEFAULT;	// set_options (VOCODER_OPT_DEFAULT until then)

static uint32_t call(uint16_t off, uint32_t nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0,
		     uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t args[CODEC_CALL_MAX_ARGS] = { a0, a1, a2, a3, s0, s1, s2, s3 };
	return stm32_codec_call(base + off, args, nargs);
}

#if STM32_FLASH_ECC
#define FLASH_WORD		32u	// the unit the error correction covers
#define BUS_ERRORS		(SCB_CFSR_PRECISERR_Msk | SCB_CFSR_IMPRECISERR_Msk | SCB_CFSR_BFARVALID_Msk)

// Whether every flash word of [lo, hi) reads without a bus error. A word that a power cut left half erased or half
// programmed reads as a double ECC error, a bus fault, which the boot check's plain loads would turn into a crash
// record and a restart at every start, FM lost with DMR voice (a third-party DfuSe tool cut short can leave one; the
// installers write the vector table last, so they don't). Each word is read at priority -1 (FAULTMASK) with data bus
// faults ignored (CCR.BFHFNMIGN), as sysboot.c reg() reads a register, so such a word is a refusal here: the radio
// starts without the codec. Interrupts are held off for one word at a time. The flash's own ECC flags are left as
// they are: nothing reads them. Untested on a radio: no torn word has been read here
static bool flash_reads(uint32_t lo, uint32_t hi)
{
	for (uint32_t a = lo & ~(FLASH_WORD - 1u); a < hi; a += FLASH_WORD) {
		const uint32_t fm = __get_FAULTMASK();
		__disable_fault_irq();
		const uint32_t before = SCB->CFSR & BUS_ERRORS;
		SCB->CCR |= SCB_CCR_BFHFNMIGN_Msk;
		__DSB();
		__ISB();
		for (uint32_t i = 0; i < FLASH_WORD; i += 4u) {
			(void)*(const volatile uint32_t *)(uintptr_t)(a + i);
		}
		__DSB();
		SCB->CCR &= ~SCB_CCR_BFHFNMIGN_Msk;
		const uint32_t err = SCB->CFSR & BUS_ERRORS & ~before;
		if (err != 0) {
			SCB->CFSR = err;	// write 1 to clear: only the flags these loads set
		}
		__DSB();
		__ISB();
		__set_FAULTMASK(fm);
		if (err != 0) {
			return false;
		}
	}
	return true;
}

// The flash every profile's check reads: from the lowest flash the boards leave to the codec to the descriptor's end
static bool codec_flash_reads(void)
{
	uint32_t lo = CODEC_DESC_ADDR;
	for (uint8_t i = 0; i < stm32_codec_profiles.count; i++) {
		if (stm32_codec_profiles.list[i]->flash_lo < lo) {
			lo = stm32_codec_profiles.list[i]->flash_lo;
		}
	}
	return flash_reads(lo, CODEC_DESC_ADDR + (uint32_t)sizeof(codec_desc_t));
}
#endif

// One build's checks: the descriptor (its own checks, the build's tag, its placement and CRCs where the board pins
// them), then the code's signature word (an erased or zero region fails fast), then the CRC of every window
static bool matches(const codec_desc_t *d, const stm32_codec_profile_t *p)
{
	if (!codec_desc_check(d, p->tag, p->flash_lo, CODEC_DESC_ADDR, p->code_len) || d->code.len != p->code_len) {
		return false;
	}
	if ((p->code != 0 && d->code.addr != p->code) || (p->code_crc != 0 && d->code.crc != p->code_crc)) {
		return false;
	}
	if (p->tables != NULL && (d->windows != p->windows ||
				  memcmp(d->table, p->tables, p->windows * sizeof(codec_desc_window_t)) != 0)) {
		return false;
	}
	return *(const volatile uint32_t *)d->code.addr == CODEC_SIGNATURE &&
	       codec_desc_check_bytes(d, (const uint8_t *)p->flash_lo, p->flash_lo);
}

// Once (the window CRCs read about 100-140 KB of flash)
static bool present(void)
{
	if (presence < 0) {
		// Once, before the first codec call: encode and decode both come through here first
		if (codec_cpu_traps_off(&SCB->CCR)) {
			__DSB();
			__ISB();
		}
		const codec_desc_t *d = (const codec_desc_t *)CODEC_DESC_ADDR;
		prof = NULL;
#if STM32_FLASH_ECC
		// before any plain load of it: a torn word costs DMR voice, not the start-up
		const bool readable = codec_flash_reads();
#else
		const bool readable = true;
#endif
		for (uint8_t i = 0; readable && i < stm32_codec_profiles.count && prof == NULL; i++) {
			if (matches(d, stm32_codec_profiles.list[i])) {
				prof = stm32_codec_profiles.list[i];
			}
		}
		base = prof != NULL ? d->code.addr : 0;
		presence = prof != NULL ? 1 : 0;
	}
	return presence == 1;
}

// The init sequence (codec-dm1701.md, "Init") in its two halves, each on the shared block: the encoder's steps, or the
// decoder's (with its first decode). The codec touches no fixed RAM address, so the halves don't depend on each other
static void enc_start(void)
{
	const stm32_codec_entries_t *at = &prof->at;
	const uint32_t E = (uint32_t)state;
	memset(state, 0, sizeof state);		// the codec expects a zeroed block
	memset(fec_state, 0, sizeof fec_state);
	memset(bitbuf, 0, sizeof bitbuf);
	call(at->enc_init, 3, E, 1, 1, 0, 0, 0, 0, 0);
	call(at->fec_init, 2, (uint32_t)fec_state, 1, 0, 0, 0, 0, 0, 0);	// the encoder side's FEC state
	{
		const uint32_t args[CODEC_CALL_MAX_ARGS] = {
			E, (uint32_t)-22, 20, (uint32_t)-10, (uint32_t)-40, 0x4000, 3276, 3276, 1310, 10, 20,
		};
		stm32_codec_call(base + at->enc_params, args, 11);
	}
	call(at->enc_set, 3, E, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	held = HELD_ENC;
}

static void dec_start(void)
{
	const stm32_codec_entries_t *at = &prof->at;
	const uint32_t D = (uint32_t)state;
	memset(state, 0, sizeof state);
	memset(bitbuf, 0, sizeof bitbuf);
	call(at->dec_init, 2, D, 1, 0, 0, 0, 0, 0, 0);
	call(at->dec_set, 3, D, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	// Last: one decode of an all-zero frame with flags 8
	call(at->decode, 7, (uint32_t)pcmbuf, HALF, (uint32_t)bitbuf, 0, 8, 0, D, 0);
	held = HELD_DEC;
}

// A fresh state for the next call: the next encode or decode builds its half (enc_start, dec_start)
static void reset(void)
{
	held = HELD_NONE;
}

// Two calls per 20 ms frame, 80 samples each, half index 0 then 1. Both calls pass the same flags word, read once
// per frame (set_options; 0x1840 by default). r1 = 0 and the 0x2000 argument are what stock passes from its wrapper
// variables. The 49 parameter bits come back one per halfword.
static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	if (!present()) {
		memset(bits, 0, VOCODER_BITS);
		return;
	}
	if (held != HELD_ENC) {
		enc_start();
	}
	memcpy(pcmbuf, pcm, sizeof pcmbuf);
	const uint32_t flags = enc_flags;
	for (uint32_t h = 0; h < 2; h++) {
		call(prof->at.encode, 8, (uint32_t)bitbuf, 0, (uint32_t)&pcmbuf[h * HALF], HALF, flags, h, 0x2000,
		     (uint32_t)state);
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bits[i] = (uint8_t)(bitbuf[i] & 1);
	}
}

// Two calls per frame on the same bit buffer (the codec rewrites it during the first call, as with stock). The flags
// word is the codec's frame quality input: 0 for a good frame; stock passes 4 when its FEC finds C0 uncorrectable
// (the codec then repeats the previous frame). hal_vocoder_t has no quality input, so the caller conceals bad frames
// (core/dmr/ambe/ambeVoice.c) and this always passes 0.
static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	if (!present()) {
		memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
		return;
	}
	if (held != HELD_DEC) {
		dec_start();
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bitbuf[i] = bits[i] & 1;
	}
	for (uint32_t h = 0; h < 2; h++) {
		call(prof->at.decode, 7, (uint32_t)&pcmbuf[h * HALF], HALF, (uint32_t)bitbuf, 0, 0, h,
		     (uint32_t)state, 0);
	}
	memcpy(pcm, pcmbuf, sizeof pcmbuf);
}

// The id tools/rfoundry and the release manifests use for the build found
static const char *profile(void)
{
	return present() ? prof->id : NULL;
}

// Encode options (hal_vocoder_t): one flags word for every encode call, kept across reset(). The same library in
// every build these profiles accept, so the same bits
static uint32_t options(void)
{
	return AMBE2_OPTIONS;
}

static void set_options(uint32_t opts)
{
	enc_flags = ambe2_enc_flags(opts);
}

static uint32_t encode_flags(void)
{
	return enc_flags;
}

const hal_vocoder_t stm32_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
	.options = options, .set_options = set_options, .encode_flags = encode_flags,
};

#endif	// CONFIG_DMR
