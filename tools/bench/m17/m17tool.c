// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on the bench, through the firmware's own code (core/m17, core/codec2, core/fec; docs/spec/m17.md): writes the
// symbols of a transmission as the radio's transmit path lays it out, and reads symbols off the air back into frames.
// tools/bench/m17tool.py runs it (and builds it: make -C tools/bench/m17).
//
//   m17tool voice OUT --src CALL --dst CALL [--can N] [--frames N] [--1600] [--tone HZ] [--lead N]
//                     [--scrambler SEED | --aes KEY]
//   m17tool bert OUT [--frames N] [--lead N]
//   m17tool sms OUT --src CALL --dst CALL --text TEXT [--can N] [--lead N]
//   m17tool decode IN [--unpacked]
//
// OUT and IN hold dibits packed four a byte, the first in bits 7-6 (hal_air.h: 01 +3, 00 +1, 10 -1, 11 -3); IN with
// --unpacked one a byte. A transmission is the preamble, the LSF (not for BERT), the frames, the end of transmission
// (m17.md 1.1); --lead puts that many more preamble symbols before it. Voice frames are Codec 2 of a tone (--tone, 0
// silence) at -6 dBFS. Addresses: a callsign, ALL (broadcast) or 12 hex digits. --scrambler (1-3 bytes) or --aes
// (16, 24 or 32 bytes), in hex: the voice frames encrypted as the radio's transmit path does (m17.md 6; core/m17's
// m17_crypt), the subtype from the length, an AES nonce of timestamp 0 and fresh random bytes in META.
//
// decode finds the frames by their syncs (one symbol in error at most), every 192 symbols once one is found, and prints
// one JSON line per frame, then a summary: the LSF's fields, stream frames and their numbers, the decoded voice's peak,
// packets (an SMS's text), BERT bits and errors, whether the end came.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "air/air_syms.h"
#include "codec2/codec2.h"
#include "m17/m17.h"
#include "m17/m17_bert.h"
#include "m17/m17_crypt.h"
#include "m17/m17_frame.h"
#include "m17/m17_packet.h"

#define MAX_SYMS	(1u << 22)

static uint8_t *out;
static uint32_t out_syms;

static void die(const char *what)
{
	fprintf(stderr, "m17tool: %s\n", what);
	exit(2);
}

static void put_sym(uint8_t d)
{
	if (out_syms >= MAX_SYMS) {
		die("too long");
	}
	out[out_syms >> 2] = (uint8_t)(out[out_syms >> 2] | d << (6 - 2 * (out_syms & 3)));	// out starts zeroed
	out_syms++;
}

static void put_syms(const uint8_t *packed, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		put_sym(air_sym_get(packed, i));
	}
}

static uint64_t addr(const char *t)
{
	uint64_t a = 0;
	if (strcmp(t, "ALL") == 0 || strcmp(t, "all") == 0) {
		return M17_ADDR_BROADCAST;
	}
	if (strlen(t) == 12 && strspn(t, "0123456789abcdefABCDEF") == 12) {
		return strtoull(t, NULL, 16);
	}
	if (!m17_addr_from_text(t, &a)) {
		die("not an M17 address");
	}
	return a;
}

static void lead(unsigned n, bool bert)
{
	uint8_t p[M17_FRAME_BYTES];
	m17_preamble(bert, p);
	for (unsigned i = 0; i < n; i++) {
		put_sym(air_sym_get(p, (uint16_t)(i % M17_FRAME_SYMS)));	// a longer preamble (m17.md 13: accepted)
	}
	put_syms(p, M17_FRAME_SYMS);
}

static void frame(const uint8_t f[M17_FRAME_BYTES])
{
	put_syms(f, M17_FRAME_SYMS);
}

static void eot(void)
{
	uint8_t f[M17_FRAME_BYTES];
	m17_eot(f);
	frame(f);
}

static void write_out(const char *path, const char *kind, unsigned frames)
{
	FILE *fp = fopen(path, "wb");
	if (fp == NULL || fwrite(out, 1, (out_syms + 3) / 4, fp) != (out_syms + 3) / 4 || fclose(fp) != 0) {
		die("can't write the output");
	}
	printf("{\"ev\": \"gen\", \"kind\": \"%s\", \"syms\": %u, \"frames\": %u}\n", kind, (unsigned)out_syms, frames);
}

// TYPE (m17.md 3.1): stream bit 0, data type 1-2, encryption 3-4 (none), its subtype 5-6 (META text), CAN 7-10
static uint16_t type_of(bool stream, uint8_t data, uint8_t can)
{
	return (uint16_t)((stream ? M17_TYPE_STREAM : 0u) | (unsigned)data << 1 | (unsigned)(can & 15u) << 7);
}

// The voice's encryption (--scrambler, --aes): the key's bytes, M17_ENC_* (NONE: in the clear)
typedef struct {
	uint8_t kind, len, key[AES_KEY_MAX];
} crypt_opt_t;

static void hex_key(const char *t, crypt_opt_t *c, uint8_t kind)
{
	size_t n = strlen(t);
	if (n == 0 || n % 2 != 0 || n / 2 > sizeof c->key || strspn(t, "0123456789abcdefABCDEF") != n) {
		die("a key in hex: --scrambler 1-3 bytes, --aes 16, 24 or 32");
	}
	for (size_t i = 0; i < n / 2; i++) {
		char b[3] = { t[2 * i], t[2 * i + 1], '\0' };
		c->key[i] = (uint8_t)strtoul(b, NULL, 16);
	}
	c->len = (uint8_t)(n / 2);
	c->kind = kind;
}

static void gen_voice(uint64_t src, uint64_t dst, uint8_t can, unsigned frames, bool m1600, unsigned tone,
		      const crypt_opt_t *cr)
{
	static c2_enc_state_t enc;
	static c2_scratch_t scr;
	static m17_scr_t sc;
	static aes_ctx_t aes;
	uint8_t lsf[M17_LSF_BYTES], meta[M17_META_BYTES] = { 0 }, f[M17_FRAME_BYTES], lich[M17_LICH_BYTES];
	int16_t pcm[C2_BLOCK_SAMPLES];
	double ph = 0;
	uint16_t type = type_of(true, m1600 ? M17_DATA_VOICE_DATA : M17_DATA_VOICE, can);

	// TYPE bits 3-4 the encryption, 5-6 its subtype (m17.md 3.1); AES's nonce is META (6.2)
	if (cr->kind == M17_ENC_SCRAMBLER) {
		if (!m17_scr_init(&sc, cr->key, cr->len)) {
			die("--scrambler: 1 to 3 bytes, not all zero");
		}
		type = (uint16_t)(type | M17_ENC_SCRAMBLER << 3 | (unsigned)m17_scr_sub(&sc) << 5);
	} else if (cr->kind == M17_ENC_AES) {
		uint8_t rnd[M17_AES_NONCE_RANDOM];
		FILE *r = fopen("/dev/urandom", "rb");
		if (!aes_init(&aes, cr->key, cr->len)) {
			die("--aes: 16, 24 or 32 bytes");
		}
		if (r == NULL || fread(rnd, 1, sizeof rnd, r) != sizeof rnd) {
			die("no random bytes for the nonce");
		}
		fclose(r);
		m17_aes_nonce(0, rnd, meta);
		type = (uint16_t)(type | M17_ENC_AES << 3 | (unsigned)m17_aes_sub(cr->len) << 5);
	}
	m17_lsf_make(lsf, dst, src, type, meta);
	m17_frame_lsf(lsf, f);
	frame(f);
	c2_enc_init(&enc, m1600 ? C2_MODE_1600 : C2_MODE_3200);
	for (unsigned n = 0; n < frames; n++) {
		uint8_t s[M17_STREAM_BYTES] = { 0 }, bits[C2_FRAME_BYTES];
		// Two 20 ms blocks a frame: 3200 a codec frame each, 1600 one for both (and 8 bytes of no data)
		for (unsigned b = 0; b < 2; b++) {
			for (unsigned i = 0; i < C2_BLOCK_SAMPLES; i++) {
				pcm[i] = (int16_t)(tone ? 16384.0 * sin(ph) : 0.0);
				ph += 2.0 * M_PI * tone / 8000.0;
			}
			if (c2_enc_block(&enc, &scr, pcm, bits)) {
				memcpy(s + (m1600 ? 0 : b * C2_FRAME_BYTES), bits, C2_FRAME_BYTES);
			}
		}
		uint16_t fn = (uint16_t)(n | (n + 1 == frames ? M17_FN_END : 0u));
		if (cr->kind == M17_ENC_SCRAMBLER) {
			m17_scr_frame(&sc, s);
		} else if (cr->kind == M17_ENC_AES) {
			m17_aes_frame(&aes, meta, fn, s);
		}
		m17_lich_make(lsf, (uint8_t)(n % 6u), lich);
		m17_frame_stream(lich, fn, s, f);
		frame(f);
	}
	aes_wipe(&aes);
	eot();
}

static unsigned gen_bert(unsigned frames)
{
	m17_prbs9_t g;
	uint8_t bits[M17_BERT_BYTES], f[M17_FRAME_BYTES];
	m17_prbs9_init(&g);
	for (unsigned n = 0; n < frames; n++) {
		m17_bert_next(&g, bits);
		m17_frame_bert(bits, f);
		frame(f);
	}
	eot();
	return frames;
}

static unsigned gen_sms(uint64_t src, uint64_t dst, uint8_t can, const char *text)
{
	static uint8_t data[M17_PKT_DATA_MAX];
	uint8_t lsf[M17_LSF_BYTES], meta[M17_META_BYTES] = { 0 }, f[M17_FRAME_BYTES], chunk[M17_PACKET_FRAME_BYTES];
	size_t n = strlen(text);
	unsigned frames = 1;
	m17_pkt_tx_t t;

	if (n + 2 > sizeof data) {
		die("text too long");
	}
	data[0] = M17_PKT_SMS;
	memcpy(data + 1, text, n + 1);		// and its zero byte (m17.md 8)
	m17_lsf_make(lsf, dst, src, type_of(false, 0, can), meta);
	m17_frame_lsf(lsf, f);
	frame(f);
	m17_pkt_tx_start(&t, data, (uint16_t)(n + 2));
	while (m17_pkt_tx_frame(&t, chunk)) {
		m17_frame_packet(chunk, f);
		frame(f);
		frames++;
	}
	eot();
	return frames;
}

// --- decode ---
static uint8_t *syms;	// one a byte
static uint32_t nsyms;

static unsigned sync_errs(uint32_t at, uint16_t word)
{
	unsigned e = 0;
	for (unsigned i = 0; i < M17_SYNC_SYMS; i++) {
		e += syms[at + i] != ((word >> (14 - 2 * i)) & 3u);
	}
	return e;
}

static void packed_at(uint32_t at, uint8_t f[M17_FRAME_BYTES])
{
	memset(f, 0, M17_FRAME_BYTES);
	for (uint16_t i = 0; i < M17_FRAME_SYMS; i++) {
		air_sym_put(f, i, syms[at + i]);
	}
}

static void addr_json(const char *key, const uint8_t *a)
{
	char s[M17_ADDR_TEXT];
	uint64_t v = m17_addr_get(a);
	uint8_t k = m17_addr_text(v, s);
	printf("\"%s\": \"%s\"", key, k == M17_ADDR_KIND_BROADCAST ? "ALL" : s);
}

static void json_text(const char *s)
{
	putchar('"');
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			printf("\\%c", c);
		} else if (c < 0x20) {
			printf("\\u%04x", c);
		} else {
			putchar(c);
		}
	}
	putchar('"');
}

// What a frame decodes to, as one kind: the decoder's count, or -1 (m17_frame.h)
typedef struct {
	uint8_t lsf[M17_LSF_BYTES];
	uint16_t fn;
	uint8_t s[M17_STREAM_BYTES];
	uint8_t c[M17_PACKET_FRAME_BYTES];
	uint8_t b[M17_BERT_BYTES];
} frame_t;

static int decode_as(int kind, const uint8_t *f, m17_fec_scratch_t *fs, frame_t *x)
{
	switch (kind) {
	case 0: return m17_frame_lsf_get(f, NULL, fs, x->lsf);
	case 1: return m17_frame_stream_get(f, NULL, fs, &x->fn, x->s);
	case 2: return m17_frame_packet_get(f, NULL, fs, x->c);
	case 3: return m17_frame_bert_get(f, NULL, fs, x->b);
	default: return -1;
	}
}

static int decode(void)
{
	static m17_fec_scratch_t fs;
	static c2_dec_state_t dec;
	static c2_scratch_t scr;
	static uint8_t pkt[M17_PKT_DATA_MAX + 1];
	static const uint16_t words[] = { M17_SYNC_LSF, M17_SYNC_STREAM, M17_SYNC_PACKET, M17_SYNC_BERT };
	uint8_t f[M17_FRAME_BYTES], lsf[M17_LSF_BYTES];
	frame_t x;
	unsigned lsfs = 0, stream = 0, packets = 0, bert = 0, eots = 0, lost = 0, fn_gaps = 0, fec = 0;
	int fn_first = -1, fn_last = -1, peak = 0, have_lsf = 0;
	bool end = false, dec_on = false, pkt_mode = false;
	// In a transmission (an LSF decoded, or a BERT run's preamble seen): its frames come every 192 symbols, and a frame
	// that doesn't decode is counted lost there. Outside one only the LSF and BERT's start begin one: a stream or BERT
	// frame has no CRC, and their 8-symbol syncs turn up in noise
	unsigned tx = 0, misses = 0;
	m17_pkt_rx_t pr;
	m17_bert_rx_t br;
	uint32_t i = 0;

	memset(lsf, 0, sizeof lsf);
	m17_bert_rx_init(&br);
	m17_pkt_rx_start(&pr, pkt, M17_PKT_DATA_MAX);
	while (i + M17_FRAME_SYMS <= nsyms) {
		// The kinds whose sync is here (one symbol off at most). The LSF and packet words are two symbols apart, as
		// are stream and BERT (m17.md 1.2): one symbol off between them matches both. So, as the firmware does (m17.md
		// 14.2), the kind the transmission in hand expects is tried first (after an LSF its TYPE's stream or packet; in
		// a BERT run BERT), then the others in the words' order
		int expect = tx == 1 ? (pkt_mode ? 2 : 1) : tx == 2 ? 3 : -1, order[5], n = 0, kind = -1, e = -1;
		bool any = false, bert_pre = false;

		if (expect >= 0) {
			order[n++] = expect;
		}
		for (int k = 0; k < 4; k++) {
			if (k != expect) {
				order[n++] = k;
			}
		}
		packed_at(i, f);
		for (int j = 0; j < n && e < 0; j++) {
			int k = order[j];

			if (sync_errs(i, words[k]) > 1) {
				continue;
			}
			any = true;
			// A stream or packet frame only inside an LSF's transmission; BERT inside a run, or after its preamble
			if ((k == 1 || k == 2) && tx != 1) {
				continue;
			}
			if (k == 3 && tx != 2) {
				// The BERT preamble's last 8 symbols before the first frame (m17.md 9)
				unsigned pe = 0;
				for (unsigned q = 0; q < 8 && i >= 8; q++) {
					pe += syms[i - 8 + q] != ((M17_PREAMBLE_BERT >> (6 - 2 * (q & 3))) & 3u);
				}
				if (i < 8 || pe > 1) {
					continue;
				}
				bert_pre = true;
			}
			e = decode_as(k, f, &fs, &x);
			kind = k;
		}
		if (!any && m17_eot_errs(f) <= 2) {
			printf("{\"ev\": \"eot\", \"at\": %u}\n", (unsigned)i);
			eots++;
			tx = 0;
			m17_pkt_rx_start(&pr, pkt, M17_PKT_DATA_MAX);	// a packet cut short is over
			i += M17_FRAME_SYMS;
			continue;
		}
		if (bert_pre) {
			tx = 2;		// a BERT run starts at its preamble, its first frame decoded or not (then lost on the grid)
		}
		if (e >= 0 && kind == 0) {
			uint16_t t;

			memcpy(lsf, x.lsf, sizeof lsf);
			t = m17_lsf_type(lsf);
			printf("{\"ev\": \"lsf\", \"at\": %u, \"errs\": %d, ", (unsigned)i, e);
			addr_json("src", lsf + M17_LSF_SRC);
			printf(", ");
			addr_json("dst", lsf + M17_LSF_DST);
			printf(", \"type\": %u, \"can\": %u, \"stream\": %u, \"data\": %u}\n", t, M17_TYPE_CAN(t),
			       t & M17_TYPE_STREAM, M17_TYPE_DATA(t));
			lsfs++;
			have_lsf = 1;
			tx = 1;
			pkt_mode = (t & M17_TYPE_STREAM) == 0;
			// A new transmission never inherits a packet half received
			m17_pkt_rx_start(&pr, pkt, M17_PKT_DATA_MAX);
			if (t & M17_TYPE_STREAM) {
				c2_dec_init(&dec, M17_TYPE_DATA(t) == M17_DATA_VOICE_DATA ? C2_MODE_1600 : C2_MODE_3200);
				dec_on = M17_TYPE_DATA(t) >= M17_DATA_VOICE;
			}
		} else if (e >= 0 && kind == 1) {
			uint16_t fn = x.fn;
			int fnn = fn & M17_FN_MASK;
			if (fn_last >= 0 && fnn != fn_last + 1) {
				fn_gaps++;
			}
			fn_first = fn_first < 0 ? fnn : fn_first;
			fn_last = fnn;
			end = end || (fn & M17_FN_END) != 0;
			stream++;
			fec += (unsigned)e;
			if (dec_on) {
				// 3200: two codec frames a stream frame; 1600: one, two blocks long
				for (unsigned h = 0; h < (M17_TYPE_DATA(m17_lsf_type(lsf)) == M17_DATA_VOICE ? 2u : 1u); h++) {
					int16_t pcm[C2_BLOCK_SAMPLES];
					c2_dec_frame(&dec, x.s + h * C2_FRAME_BYTES, false);
					while (c2_dec_block(&dec, &scr, pcm)) {
						for (unsigned k = 0; k < C2_BLOCK_SAMPLES; k++) {
							peak = abs(pcm[k]) > peak ? abs(pcm[k]) : peak;
						}
					}
				}
			}
			printf("{\"ev\": \"stream\", \"at\": %u, \"errs\": %d, \"fn\": %u}\n", (unsigned)i, e, fn);
		} else if (e >= 0 && kind == 2) {
			uint8_t r = m17_pkt_rx_frame(&pr, x.c);
			printf("{\"ev\": \"packet_frame\", \"at\": %u, \"errs\": %d, \"last\": %u}\n", (unsigned)i, e,
			       (x.c[M17_PACKET_CHUNK] & M17_PKT_EOF) != 0);
			if (r != M17_PKT_MORE) {
				uint8_t tl = 0;
				uint32_t type = m17_pkt_type_get(pkt, pr.kept, &tl);
				pkt[pr.kept < M17_PKT_DATA_MAX ? pr.kept : M17_PKT_DATA_MAX] = '\0';
				printf("{\"ev\": \"packet\", \"ok\": %u, \"type\": %u, \"len\": %u, \"text\": ", r == M17_PKT_DONE,
				       (unsigned)type, pr.kept);
				json_text(r == M17_PKT_DONE && type == M17_PKT_SMS ? (const char *)pkt + tl : "");
				printf("}\n");
				packets += r == M17_PKT_DONE;
				m17_pkt_rx_start(&pr, pkt, M17_PKT_DATA_MAX);
			}
		} else if (e >= 0 && kind == 3) {
			m17_bert_rx_frame(&br, x.b);
			bert++;
		}
		if (e < 0 && tx != 0) {
			// On the grid: a frame lost; four in a row end the transmission (search again from here)
			lost++;
			if (++misses < 4) {
				i += M17_FRAME_SYMS;
			} else {
				tx = 0;
				misses = 0;
				m17_pkt_rx_start(&pr, pkt, M17_PKT_DATA_MAX);
				i++;
			}
			continue;
		}
		if (e < 0) {
			i++;
			continue;
		}
		misses = 0;
		i += M17_FRAME_SYMS;
	}
	printf("{\"ev\": \"summary\", \"lsf\": %u, \"stream\": %u, \"fn_first\": %d, \"fn_last\": %d, \"fn_gaps\": %u, "
	       "\"end\": %u, \"fec_bits\": %u, \"voice_peak\": %d, \"packets\": %u, \"bert_frames\": %u, \"bert_bits\": %u, "
	       "\"bert_errs\": %u, \"eot\": %u, \"bad\": %u}\n", lsfs, stream, fn_first, fn_last, fn_gaps, end, fec, peak,
	       packets, bert, (unsigned)br.bits, (unsigned)br.errs, eots, lost);
	return have_lsf || bert ? 0 : 1;
}

static const char *opt(int argc, char **argv, const char *name, const char *dflt)
{
	for (int i = 2; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return dflt;
}

static bool flag(int argc, char **argv, const char *name)
{
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return true;
		}
	}
	return false;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: m17tool voice|bert|sms OUT [options] | decode IN [--unpacked]\n");
		return 2;
	}
	const char *cmd = argv[1], *path = argv[2];
	if (strcmp(cmd, "decode") == 0) {
		FILE *fp = fopen(path, "rb");
		static uint8_t buf[MAX_SYMS];
		size_t n = fp != NULL ? fread(buf, 1, sizeof buf, fp) : 0;
		if (fp == NULL) {
			die("can't read the input");
		}
		fclose(fp);
		bool unpacked = flag(argc, argv, "--unpacked");
		nsyms = (uint32_t)(unpacked ? n : (n * 4 > MAX_SYMS ? MAX_SYMS : n * 4));
		syms = malloc(nsyms + 1);
		for (uint32_t i = 0; i < nsyms; i++) {
			syms[i] = unpacked ? (uint8_t)(buf[i] & 3u) : (uint8_t)((buf[i >> 2] >> (6 - 2 * (i & 3))) & 3u);
		}
		int r = decode();
		free(syms);
		return r;
	}
	out = calloc(MAX_SYMS / 4, 1);
	unsigned frames = (unsigned)strtoul(opt(argc, argv, "--frames", "25"), NULL, 0);
	uint8_t can = (uint8_t)strtoul(opt(argc, argv, "--can", "0"), NULL, 0);
	unsigned ld = (unsigned)strtoul(opt(argc, argv, "--lead", "0"), NULL, 0);
	if (can > 15 || frames == 0 || frames > 30000) {
		die("--can 0..15, --frames 1..30000");
	}
	if (strcmp(cmd, "voice") == 0) {
		crypt_opt_t cr = { M17_ENC_NONE, 0, { 0 } };
		const char *scr = opt(argc, argv, "--scrambler", NULL), *aes = opt(argc, argv, "--aes", NULL);
		if (scr != NULL && aes != NULL) {
			die("--scrambler or --aes, not both");
		}
		if (scr != NULL) {
			hex_key(scr, &cr, M17_ENC_SCRAMBLER);
		} else if (aes != NULL) {
			hex_key(aes, &cr, M17_ENC_AES);
		}
		lead(ld, false);
		gen_voice(addr(opt(argc, argv, "--src", "N0CALL")), addr(opt(argc, argv, "--dst", "ALL")), can, frames,
			  flag(argc, argv, "--1600"), (unsigned)strtoul(opt(argc, argv, "--tone", "440"), NULL, 0), &cr);
	} else if (strcmp(cmd, "bert") == 0) {
		lead(ld, true);
		frames = gen_bert(frames);
	} else if (strcmp(cmd, "sms") == 0) {
		lead(ld, false);
		frames = gen_sms(addr(opt(argc, argv, "--src", "N0CALL")), addr(opt(argc, argv, "--dst", "ALL")), can,
				 opt(argc, argv, "--text", "test"));
	} else {
		die("unknown command");
	}
	write_out(path, cmd, frames);
	free(out);
	return 0;
}
