// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Candidate files of the Codec 2 encoder (core/codec2) for the reference harness's compare.py (kept outside the
// repository, docs/spec/codec2.md 1, 16): for every PCM key <name>.<mode> of the harness, <key>.bit (this encoder's
// bits for inputs/<name>.raw) and <key>.enc.jsonl (its stage values: per 10 ms f0, Wo, L, voicing; per LPC analysis E
// and the LSPs; no amplitudes, which this encoder never forms, so "A" is empty). Not run by CI: it needs the harness.
//
//   ./c2EncCand HARNESS_DIR OUT_DIR
//   python3 -I HARNESS_DIR/compare.py HARNESS_DIR OUT_DIR --gate fixed
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2/c2_enc_int.h"

static int16_t *read_pcm(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	int16_t *p = malloc(len > 0 ? (size_t)len : 2);
	*n = p != NULL && len > 0 ? fread(p, 2, (size_t)len / 2, f) : 0;
	fclose(f);
	return p;
}

static int key_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void one_key(const char *dir, const char *out, const char *key)
{
	char path[1024], name[256];
	c2_mode_t m = strstr(key, ".1600") != NULL ? C2_MODE_1600 : C2_MODE_3200;

	snprintf(name, sizeof name, "%s", key);
	*strrchr(name, '.') = 0;
	snprintf(path, sizeof path, "%s/inputs/%s.raw", dir, name);
	size_t n;
	int16_t *pcm = read_pcm(path, &n);
	if (pcm == NULL) {
		printf("%s: no input %s\n", key, path);
		return;
	}
	snprintf(path, sizeof path, "%s/%s.bit", out, key);
	FILE *fb = fopen(path, "wb");
	snprintf(path, sizeof path, "%s/%s.enc.jsonl", out, key);
	FILE *fj = fopen(path, "w");
	if (fb == NULL || fj == NULL) {
		printf("%s: cannot write %s\n", key, path);
		exit(1);
	}
	static c2_enc_state_t st;
	static c2_scratch_t scr;
	c2_enc_init(&st, m);
	// The encoder's buffer as it stands after each block (1.0 at creation, spec 5.1), for the LSPs of the analyses that
	// do not send them (1600's frame 2): the dump has them, so the candidate's too
	int16_t buf[C2E_M], x[2 * C2E_WIN_HALF];
	for (int j = 0; j < C2E_M; j++)
		buf[j] = 1;
	unsigned frames = 0, blocks = 0;
	for (size_t i = 0; i + C2_BLOCK_SAMPLES <= n; i += C2_BLOCK_SAMPLES, blocks++) {
		uint8_t bits[C2_FRAME_BYTES];
		bool done = c2_enc_block(&st, &scr, pcm + i, bits);
		c2e_trace_t t;
		c2_enc_trace(&st, &t);
		memmove(buf, buf + C2_BLOCK_SAMPLES, sizeof(buf[0]) * (C2E_M - C2_BLOCK_SAMPLES));
		memcpy(buf + C2E_M - C2_BLOCK_SAMPLES, pcm + i, sizeof(buf[0]) * C2_BLOCK_SAMPLES);
		if (!t.lsps)
			c2e_lpc(buf, x, true, &t.lpc);
		for (int k = 0; k < 2; k++) {
			unsigned sub = m == C2_MODE_1600 ? 2 * (blocks & 1) + (unsigned)k : (unsigned)k;
			fprintf(fj, "{\"type\":\"analysis\",\"frame\":%u,\"sub\":%u,\"f0\":%.9g,\"Wo\":%.9g,\"L\":%u,"
				"\"voiced\":%u,\"A\":[]}\n", m == C2_MODE_1600 ? blocks / 2 : blocks, sub, t.f[k].k_c * 3.125,
				t.f[k].wo / 268435456.0, t.f[k].L, t.f[k].voiced);
		}
		double E = t.lpc.e_pos ? exp2(t.lpc.e_log2 / 2048.0) : 0.0;
		fprintf(fj, "{\"type\":\"lpc\",\"E\":%.9g,\"lsp\":[", E);
		for (int j = 0; j < C2_ORDER; j++)
			fprintf(fj, "%s%.9g", j ? "," : "", t.lpc.lsp_hz[j] / 65536.0 * 3.141592654 / 4000.0);
		fprintf(fj, "]}\n");
		if (done) {
			fwrite(bits, 1, C2_FRAME_BYTES, fb);
			frames++;
		}
	}
	fclose(fb);
	fclose(fj);
	free(pcm);
	printf("%s: %u frames\n", key, frames);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: c2EncCand HARNESS_DIR OUT_DIR\n");
		return 2;
	}
	char path[1024];
	snprintf(path, sizeof path, "%s/ref", argv[1]);
	DIR *d = opendir(path);
	if (d == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	char *keys[128];
	unsigned n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < 128) {
		size_t l = strlen(de->d_name);
		if (l > 10 && strcmp(de->d_name + l - 10, ".enc.jsonl") == 0) {
			keys[n] = strdup(de->d_name);
			keys[n][l - 10] = 0;
			n++;
		}
	}
	closedir(d);
	qsort(keys, n, sizeof keys[0], key_cmp);
	for (unsigned i = 0; i < n; i++) {
		one_key(argv[1], argv[2], keys[i]);
		free(keys[i]);
	}
	printf("c2EncCand: %u keys written to %s\n", n, argv[2]);
	return 0;
}
