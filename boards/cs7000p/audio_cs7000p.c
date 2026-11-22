// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS PCM audio: the hal_audio_t on the HR_C6000's I2S port (platform/stm32/hrc/i2s_pcm.c, the MCU a
// slave on SPI3) for beeps and DMR audio (docs/spec/hardware-cs7000p.md 6.3, 8). Nothing here has run on a radio.
//
// The whole file is DMR code, as the stream is: a build without DMR has no PCM path, plays FM through the modem's
// line out and makes no beeps.
//
// The stream does not start until the platform has proven which of the two data pads the I2S block drives
// (platform/stm32h7/hrc/i2s_pcm_soc.h): until then start() fails, there are no beeps, and board.i2s says why. That
// first start probes with the modem's I2S clock running; a set-up of the modem by the radio task in those 9 ms can
// leave the probe without a verdict, and the next start probes again.
//
// Everything played leaves by the modem's line out, whose amplifier is on the receive rail: beeps are silent while
// the radio transmits or its receiver sleeps. No codec register is written here: the modem's set-ups own them, the
// microphone's input bits above all (c6000_cs7000p.c).
#include "config.h"

#if CONFIG_DMR
#include "cs7000p.h"
#include "speaker_cs7000p.h"
#include "hrc/i2s_pcm.h"

// The clock edges as the modem's set-up leaves them (data changes on the falling edge and is read on the rising
// one): unverified. The prompt FIFO carries a beep while the modem is in FM receive, where I2S does not reach its
// line out
static const stm32_i2s_pcm_cfg_t i2s_cfg = { .ckpol = false, .master_if_free = false, .prompt_fifo = true };

static bool audio_start(audio_block_cb_t cb, void *ctx)
{
	return stm32_i2s_pcm_start(&i2s_cfg, cb, ctx);
}

static void audio_stop(void)
{
	stm32_i2s_pcm_stop();
}

// The stream's own route first, then the amplifier and the FM audio's feed-through (radio_cs7000p.c)
static void speaker_pcm(bool on)
{
	stm32_i2s_pcm_speaker(on);
	cs7000p_speaker_pcm(on);
}

const hal_audio_t cs7000p_audio = {
	.start = audio_start,
	.stop = audio_stop,
	.speaker_pcm = speaker_pcm,
	.stream_stats = stm32_i2s_pcm_stream_stats,
};
#endif
