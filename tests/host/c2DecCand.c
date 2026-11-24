// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codec 2 decoder candidate writer (out of CI): decodes every bit stream of the reference harness (HARNESS.md: the
// directory holding ref/ and inputs/, kept outside the repository) and writes what compare.py reads, per key:
// <key>.dec.raw (the PCM) and <key>.dec.jsonl (each 10 ms frame's voicing, ω0, L, energy, LSPs and random draws).
// Also prints the host time per block (mean and largest).
//
//   c2DecCand <harness dir> <output dir>
//   python3 -I compare.py <harness dir> <output dir> --gate fixed
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec2/codec2.h"
#include "codec2/c2_int.h"

static int key_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static c2_dec_state_t dec;
static c2_scratch_t scr;

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: c2DecCand <harness dir> <output dir>\n");
		return 2;
	}
	const char *dir = argv[1], *out = argv[2];
	char path[1024];
	snprintf(path, sizeof path, "%s/ref", dir);
	DIR *dd = opendir(path);
	if (dd == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	char *keys[128];
	unsigned n = 0;
	struct dirent *de;
	while ((de = readdir(dd)) != NULL && n < 128) {
		size_t l = strlen(de->d_name);
		if (l > 10 && strcmp(de->d_name + l - 10, ".dec.jsonl") == 0) {
			keys[n] = strdup(de->d_name);
			keys[n][l - 10] = 0;
			n++;
		}
	}
	closedir(dd);
	qsort(keys, n, sizeof keys[0], key_cmp);

	double total = 0, worst = 0;
	unsigned blocks = 0, frames = 0;
	for (unsigned i = 0; i < n; i++) {
		const char *key = keys[i];
		c2_mode_t m = strstr(key, ".1600") != NULL ? C2_MODE_1600 : C2_MODE_3200;
		char name[256];
		snprintf(name, sizeof name, "%s", key);
		*strrchr(name, '.') = 0;
		snprintf(path, sizeof path, "%s/ref/%s.bit", dir, key);
		FILE *fb = fopen(path, "rb");
		if (fb == NULL) {
			snprintf(path, sizeof path, "%s/inputs/%s.bit", dir, name);
			fb = fopen(path, "rb");
		}
		if (fb == NULL) {
			fprintf(stderr, "%s: no bit stream\n", key);
			return 1;
		}
		snprintf(path, sizeof path, "%s/%s.dec.raw", out, key);
		FILE *fr = fopen(path, "wb");
		snprintf(path, sizeof path, "%s/%s.dec.jsonl", out, key);
		FILE *fj = fopen(path, "w");
		if (fr == NULL || fj == NULL) {
			fprintf(stderr, "cannot write %s\n", path);
			return 1;
		}
		c2_dec_init(&dec, m);
		uint8_t bits[C2_FRAME_BYTES];
		while (fread(bits, 1, sizeof bits, fb) == sizeof bits) {
			c2_dec_frame(&dec, bits, false);
			frames++;
			for (int b = 0; b < (m == C2_MODE_1600 ? 2 : 1); b++) {
				int16_t pcm[C2_BLOCK_SAMPLES];
				double t0 = now_us();
				c2_dec_block(&dec, &scr, pcm);
				double t = now_us() - t0;
				total += t;
				worst = t > worst ? t : worst;
				blocks++;
				fwrite(pcm, sizeof pcm[0], C2_BLOCK_SAMPLES, fr);
				for (uint8_t k = 0; k < 2; k++) {
					c2_params_t p;
					uint16_t draws;
					c2_dec_params(&dec, k, &p, &draws);
					fprintf(fj, "{\"voiced\":%d,\"Wo\":%.9g,\"L\":%u,\"e\":%.9g,\"lsp\":[", p.voiced ? 1 : 0,
						p.wo / 268435456.0, p.L, p.e / 262144.0);
					for (int j = 0; j < C2_ORDER; j++)
						fprintf(fj, "%s%.9g", j ? "," : "", p.lsp[j] / 268435456.0);
					fprintf(fj, "],\"rand_calls\":%u}\n", draws);
				}
			}
		}
		fclose(fb);
		fclose(fr);
		fclose(fj);
		free(keys[i]);
	}
	printf("c2DecCand: %u keys, %u frames, %u blocks: host %.1f us per block mean, %.1f us largest\n", n, frames,
	       blocks, blocks ? total / blocks : 0.0, worst);
	return 0;
}
