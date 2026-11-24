// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The test voice set in Codec 2 (tests/data/voice/README.md): the speech file through the firmware's own encoder
// (core/codec2, as m17_voice.c runs it, one 20 ms block a call) to 8-byte frames in both modes, and those frames
// through the decoder to a WAV of what an M17 radio plays. Fixed point: the output is the same on every host.
//
//   c2VoiceGen IN.pcm OUTDIR	writes OUTDIR/<name>.c2_3200, <name>.c2_1600 and <name>_c2_3200_decoded.wav,
//				<name>_c2_1600_decoded.wav (<name>: IN without its directory and .pcm)
//
// make -C tests/host c2-voice-data runs it on every set (into tests/host/out/voice). The files are generated, not
// committed. A set of an odd number of blocks ends its 1600 frames with a block of silence.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/codec2/codec2.h"

#define MAX_BLOCKS	1000

static int16_t pcm[(MAX_BLOCKS + 1) * C2_BLOCK_SAMPLES];
static uint8_t bits[MAX_BLOCKS][C2_FRAME_BYTES];
static c2_enc_state_t enc;
static c2_dec_state_t dec;
static c2_scratch_t scr;

static size_t read_pcm(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t b[2];
	size_t n = 0;

	if (!f) {
		fprintf(stderr, "c2VoiceGen: can't read %s\n", path);
		exit(2);
	}
	// s16le, whatever the host's byte order
	while (n < MAX_BLOCKS * C2_BLOCK_SAMPLES && fread(b, 1, 2, f) == 2)
		pcm[n++] = (int16_t)(b[0] | b[1] << 8);
	if (!feof(f) && fgetc(f) != EOF) {
		fprintf(stderr, "c2VoiceGen: %s holds more than %d blocks\n", path, MAX_BLOCKS);
		exit(2);
	}
	fclose(f);
	if (n == 0 || n % C2_BLOCK_SAMPLES) {
		fprintf(stderr, "c2VoiceGen: %s: %zu samples, not whole 20 ms blocks\n", path, n);
		exit(2);
	}
	return n / C2_BLOCK_SAMPLES;
}

// The frames of mode for blocks of speech (pcm[] is zero past them: a 1600 frame's missing half is silence)
static size_t encode(c2_mode_t mode, size_t blocks)
{
	size_t nf = 0;

	c2_enc_init(&enc, mode);
	if (mode == C2_MODE_1600 && (blocks & 1))
		blocks++;
	for (size_t i = 0; i < blocks; i++)
		if (c2_enc_block(&enc, &scr, pcm + i * C2_BLOCK_SAMPLES, bits[nf]))
			nf++;
	return nf;
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static FILE *open_out(const char *dir, const char *name, const char *suffix)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s%s", dir, name, suffix);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "c2VoiceGen: can't write %s\n", path);
		exit(2);
	}
	return f;
}

// The frames decoded as the voice path plays them: each frame once, then its blocks
static int write_wav(FILE *f, c2_mode_t mode, size_t nf)
{
	size_t blocks = nf * (mode == C2_MODE_1600 ? 2u : 1u);
	uint32_t data = (uint32_t)(blocks * C2_BLOCK_SAMPLES * 2);
	uint8_t h[45] = "RIFF....WAVEfmt \x10\0\0\0\1\0\1\0........\2\0\x10\0data....";
	int peak = 0;	// int: -32768 has no int16_t magnitude

	put32(h + 4, 36 + data);
	put32(h + 24, 8000);
	put32(h + 28, 16000);
	put32(h + 40, data);
	fwrite(h, 1, 44, f);
	c2_dec_init(&dec, mode);
	for (size_t i = 0; i < nf; i++) {
		c2_dec_frame(&dec, bits[i], false);
		for (int b = 0; b < (mode == C2_MODE_1600 ? 2 : 1); b++) {
			int16_t out[C2_BLOCK_SAMPLES];

			c2_dec_block(&dec, &scr, out);
			for (int k = 0; k < C2_BLOCK_SAMPLES; k++) {
				int a = out[k] < 0 ? -out[k] : out[k];
				if (a > peak)
					peak = a;
				fputc(out[k] & 0xFF, f);
				fputc((out[k] >> 8) & 0xFF, f);
			}
		}
	}
	return peak;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: c2VoiceGen IN.pcm OUTDIR\n");
		return 2;
	}
	const char *base = strrchr(argv[1], '/');
	char name[256];

	snprintf(name, sizeof name, "%s", base ? base + 1 : argv[1]);
	char *dot = strrchr(name, '.');
	if (dot && !strcmp(dot, ".pcm"))
		*dot = 0;
	size_t blocks = read_pcm(argv[1]);
	for (c2_mode_t mode = C2_MODE_3200; mode <= C2_MODE_1600; mode++) {
		const char *mn = mode == C2_MODE_3200 ? "3200" : "1600";
		char suffix[32];
		size_t nf = encode(mode, blocks);
		FILE *f;

		snprintf(suffix, sizeof suffix, ".c2_%s", mn);
		f = open_out(argv[2], name, suffix);
		if (fwrite(bits, C2_FRAME_BYTES, nf, f) != nf) {
			fprintf(stderr, "c2VoiceGen: write error\n");
			return 2;
		}
		fclose(f);
		snprintf(suffix, sizeof suffix, "_c2_%s_decoded.wav", mn);
		f = open_out(argv[2], name, suffix);
		int peak = write_wav(f, mode, nf);
		fclose(f);
		// Speech that decodes to near silence means the chain is broken
		if (peak < 1000) {
			printf("FAIL: %s %s decodes to near silence (peak %d)\n", name, mn, peak);
			return 1;
		}
		printf("# c2VoiceGen: %s/%s.c2_%s: %zu frames of %d bytes, decoded peak %d\n", argv[2], name, mn, nf,
		       C2_FRAME_BYTES, peak);
	}
	return 0;
}
