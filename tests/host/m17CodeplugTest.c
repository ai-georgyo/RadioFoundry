// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 in the codeplug (core/m17/m17_codeplug.c, m17_addr.c; docs/spec/m17-codeplug.md) on a simulated NOR flash: the
// shared vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), a channel's set-up from
// its tag and its system, a channel and the radio-wide record written from the radio, our address, traffic keys, the
// signing key and station keys. And the storage journal's copy of the settings area: no key bytes left in it after a
// write of an area that holds a secret, while other writes keep theirs (docs/spec/settings-store.md 4). Built twice:
// with both ciphers, and without them (a block's encryption the build lacks reads as none)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "m17/m17_codeplug.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

// The board's air interface (core/radio/radio_m17.c in the firmware): whether the radio runs kind 6 here
static bool fits;
bool radio_m17_fits(void) { return fits; }

#define FLASH_SIZE	0x200000u
#define JOURNAL		0x19000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = JOURNAL,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 900000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static int unhex(const char *s, uint8_t *out, int max)
{
	int n = 0;

	if (strcmp(s, "-") == 0)
		return 0;
	while (s[0] != '\0' && s[1] != '\0' && n < max) {
		char b[3] = { s[0], s[1], 0 };
		out[n++] = (uint8_t)strtoul(b, NULL, 16);
		s += 2;
	}
	return n;
}

static uint64_t h64(const char *s) { return strtoull(s, NULL, 16); }
static uint32_t h(const char *s) { return (uint32_t)strtoul(s, NULL, 16); }

static channel_t make(const char *name, uint32_t rx, uint32_t tx)
{
	channel_t c;

	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = tx;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

// The fields of a vector (w[at] on): squelch, 1600, CAN, TX CAN, destination, encryption, key ID
static m17_conv_t conv_of(char **w)
{
	m17_conv_t c = { (uint8_t)h(w[0]), h(w[1]) != 0, (uint8_t)h(w[2]), (uint8_t)h(w[3]), h64(w[4]), (uint8_t)h(w[5]),
			 (uint16_t)h(w[6]) };
	return c;
}

// A block decoded by this build: an encryption it lacks reads as none (m17_codeplug.h)
static m17_conv_t as_built(m17_conv_t c)
{
	if (!((c.tx_enc == M17_ENC_SCRAMBLER && CONFIG_M17_SCRAMBLER) || (c.tx_enc == M17_ENC_AES && CONFIG_M17_AES)))
		c.tx_enc = M17_ENC_NONE;
	return c;
}

static bool conv_eq(const m17_conv_t *a, const m17_conv_t *b)
{
	return a->squelch == b->squelch && a->mode1600 == b->mode1600 && a->can == b->can && a->tx_can == b->tx_can &&
	       a->dst == b->dst && a->tx_enc == b->tx_enc && a->key_id == b->key_id;
}

// address, sign, send position, station type, text (hex)
static m17_wide_t wide_of(char **w)
{
	m17_wide_t r;

	memset(&r, 0, sizeof r);
	r.src = h64(w[0]);
	r.sign = h(w[1]) != 0;
	r.gnss = h(w[2]) != 0;
	r.station = (uint8_t)h(w[3]);
	r.text_len = (uint8_t)unhex(w[4], r.text, sizeof r.text);
	return r;
}

static bool wide_eq(const m17_wide_t *a, const m17_wide_t *b)
{
	return a->src == b->src && a->sign == b->sign && a->gnss == b->gnss && a->station == b->station &&
	       a->text_len == b->text_len && memcmp(a->text, b->text, a->text_len) == 0;
}

// The text between the line's bars, spaces kept
static bool barred(const char *line, char *out, size_t cap)
{
	const char *a = strchr(line, '|'), *b = a != NULL ? strrchr(line, '|') : NULL;

	if (a == NULL || b == a || (size_t)(b - a - 1) >= cap)
		return false;
	memcpy(out, a + 1, (size_t)(b - a - 1));
	out[b - a - 1] = '\0';
	return true;
}

// A channel tag of kind 6 at number 1 holding block, then the M17 set-up read back from the flash
static channel_t ch1;
static void put_block(const uint8_t *b, int len)
{
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_M17, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };

	ch1 = make("M17 vec", 433475000, 433475000);
	CHECK(codeplug_set_channel(1, &ch1) && codeplug_tag_write(1, ch1.name, &t, b, (uint16_t)len));
	CHECK(codeplug_channel(1, &ch1) && ch1.kind == CODEPLUG_KIND_M17);
}

static void test_vectors(void)
{
	FILE *f = fopen("../vectors/channel-modes.txt", "r");
	char line[1024], text[64];
	int counted = 0, conv = 0, wide = 0, call = 0, secret = 0;

	CHECK(f != NULL);
	if (f == NULL)
		return;
	fresh();
	while (fgets(line, sizeof line, f) != NULL) {
		char copy[1024];
		char *w[16] = { 0 };
		char *rest = NULL;
		uint8_t b[160], e[160];
		int n = 0;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0' || strncmp(line, "m17", 3) != 0)
			continue;
		strcpy(copy, line);
		for (char *p = strtok_r(copy, " ", &rest); p != NULL && n < 16; p = strtok_r(NULL, " ", &rest))
			w[n++] = p;
		if (strcmp(w[0], "m17conv") == 0 && strcmp(w[1], "dec") == 0 && n == 10) {
			m17_conv_t got, want = as_built(conv_of(&w[3]));
			m17_conv_decode(b, unhex(w[2], b, sizeof b), &got);
			CHECK(conv_eq(&got, &want));
			if (!conv_eq(&got, &want))
				printf("  %s\n", line);
			conv++;
		} else if (strcmp(w[0], "m17conv") == 0 && strcmp(w[1], "enc") == 0 && n == 10) {
			m17_conv_t c = conv_of(&w[2]);
			m17_conv_encode(&c, e);
			CHECK(unhex(w[9], b, sizeof b) == M17_CP_CONV_LEN && memcmp(e, b, M17_CP_CONV_LEN) == 0);
			conv++;
		} else if (strcmp(w[0], "m17conv") == 0 && strcmp(w[1], "keep") == 0 && n == 11) {
			// Through the radio's write (m17_conv_save over the stored tag), read back from the flash
			m17_conv_t c = conv_of(&w[3]);
			int want = unhex(w[10], b, sizeof b);
			put_block(e, unhex(w[2], e, sizeof e));
			CHECK(m17_conv_save(1, &ch1, &c) && codeplug_channel(1, &ch1));
			CHECK(codeplug_tag_params(1, &ch1, e, sizeof e - CODEPLUG_TAG_HEAD_MAX) == want && memcmp(e, b, (size_t)want) == 0);
			conv++;
		} else if (strcmp(w[0], "m17wide") == 0 && strcmp(w[1], "dec") == 0 && n == 8) {
			m17_wide_t got, want = wide_of(&w[3]);
			int len = unhex(w[2], b, sizeof b);
			m17_wide_decode(b, len, &got);
			CHECK(wide_eq(&got, &want));
			// The same from the flash: absent (-1) and stored alike
			if (len == 0)
				CHECK(codeplug_kv_remove(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17));
			else
				CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, b, (uint16_t)len));
			m17_wide_load(&got);
			CHECK(wide_eq(&got, &want));
			wide++;
		} else if (strcmp(w[0], "m17wide") == 0 && strcmp(w[1], "enc") == 0 && n == 8) {
			m17_wide_t r = wide_of(&w[2]);
			m17_wide_encode(&r, e);
			CHECK(unhex(w[7], b, sizeof b) == M17_CP_WIDE_LEN && memcmp(e, b, M17_CP_WIDE_LEN) == 0);
			wide++;
		} else if (strcmp(w[0], "m17wide") == 0 && strcmp(w[1], "keep") == 0 && n == 9) {
			m17_wide_t r = wide_of(&w[3]);
			int want = unhex(w[8], b, sizeof b);
			CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, e, (uint16_t)unhex(w[2], e, sizeof e)));
			CHECK(m17_wide_save(&r));
			CHECK(codeplug_kv_get(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, e, sizeof e) == want &&
			      memcmp(e, b, (size_t)want) == 0);
			wide++;
		} else if (strcmp(w[0], "m17call") == 0 && strcmp(w[1], "text") == 0 && barred(line, text, sizeof text)) {
			uint64_t a = 1;
			bool ok = m17_addr_from_text(text, &a);
			CHECK(strcmp(w[2], "-") == 0 ? !ok && a == 0 : ok && a == h64(w[2]));
			if (!(strcmp(w[2], "-") == 0 ? !ok : ok && a == h64(w[2])))
				printf("  %s\n", line);
			call++;
		} else if (strcmp(w[0], "m17call") == 0 && strcmp(w[1], "addr") == 0 && barred(line, text, sizeof text)) {
			char out[M17_ADDR_TEXT];
			CHECK(m17_addr_text(h64(w[2]), out) == h(w[3]) && strcmp(out, text) == 0);
			call++;
		} else if (strcmp(w[0], "m17secret") == 0 && strcmp(w[1], "rec") == 0 && n == 5) {
			// System 3, an M17 one, with this secret
			uint8_t sys[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17, CODEPLUG_SYS_TX };
			uint8_t d[32], q[33];
			bool has_q = true;
			int len = unhex(w[2], b, sizeof b);
			CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 3, sys, sizeof sys));
			CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 3, b, (uint16_t)len));
			memset(d, 0xEE, sizeof d);
			memset(q, 0xEE, sizeof q);
			bool ok = m17_system_signing_key(3, d, q, &has_q);
			CHECK(ok == (h(w[3]) != 0) && has_q == (h(w[4]) != 0));
			if (ok)
				CHECK(memcmp(d, b, 32) == 0);
			if (has_q)
				CHECK(memcmp(q, &b[32], 33) == 0);
			secret++;
		} else if (strcmp(w[0], "m17secret") == 0 && strcmp(w[1], "pub") == 0 && n == 5) {
			// d with Q (the editor's record): valid, Q as stored; d alone: valid, Q to derive
			uint8_t d[32], q[33];
			bool has_q;
			int len = unhex(w[2], b, 32);
			len += unhex(w[3], &b[32], 33);
			CHECK(len == 65 && codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 3, b, 65));
			CHECK(m17_system_signing_key(3, d, q, &has_q) && has_q && memcmp(q, &b[32], 33) == 0);
			CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 3, b, 32));
			CHECK(m17_system_signing_key(3, d, NULL, &has_q) && !has_q && memcmp(d, b, 32) == 0);
			secret++;
		} else {
			printf("FAIL unknown vector: %s\n", line);
			failures++;
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(conv >= 30 && wide >= 15 && call >= 30 && secret >= 12 && counted == conv + wide + call + secret);
	printf("m17CodeplugTest: %d vectors\n", counted);
}

// Every character of the alphabet both ways, and addresses back to the text they came from
static void test_addr(void)
{
	char t[M17_ADDR_TEXT], back[M17_ADDR_TEXT];
	uint64_t a;

	for (unsigned v = 0; v < 40; v++) {
		char c = m17_addr_char((uint8_t)v);
		CHECK(m17_addr_value(c) == v || (v == 0 && c == ' '));
		if (c >= 'A' && c <= 'Z')
			CHECK(m17_addr_value((char)(c - 'A' + 'a')) == v);
	}
	for (int c = 1; c < 256; c++) {
		bool in = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '/' ||
			  c == '.';
		CHECK((m17_addr_value((char)c) != 0) == in);
		char s[3] = { 'A', (char)c, 0 };
		CHECK(m17_addr_from_text(s, &a) == (in || c == ' '));
	}
	// Standard addresses spread over the range: text and back
	uint64_t x = 0x9E3779B97F4A7C15ull;
	for (int i = 0; i < 20000; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		uint64_t v = (x % M17_ADDR_STD_MAX) + 1u;
		CHECK(m17_addr_text(v, t) == M17_ADDR_KIND_STD && strlen(t) <= M17_CALL_MAX);
		CHECK(m17_addr_from_text(t, &a) && a == v);
		CHECK(m17_addr_text(a, back) == M17_ADDR_KIND_STD && strcmp(t, back) == 0);
	}
	CHECK(m17_addr_text(M17_ADDR_STD_MAX + 1u, t) == M17_ADDR_KIND_EXTENDED);
	CHECK(m17_addr_text(M17_ADDR_STD_MAX, t) == M17_ADDR_KIND_STD && strcmp(t, ".........") == 0);
	CHECK(m17_addr_text(0xFFFF000000000001ull, t) == M17_ADDR_KIND_STD && strcmp(t, "A") == 0);	// 48 bits only
}

// A channel's set-up: the block, our address, the system and whether it allows transmit
static void test_conv_load(void)
{
	m17_conv_t c = { .squelch = 2 /* addressed */, .mode1600 = true, .can = 9, .tx_can = 3, .dst = 0x9FDD51u,
			 .tx_enc = M17_ENC_AES, .key_id = 7 };
	uint8_t blk[M17_CP_CONV_LEN];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_M17, .system = 2, .flags = CODEPLUG_TAG_TX };
	uint8_t sys[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17, CODEPLUG_SYS_TX };
	m17_conv_cfg_t cfg;
	channel_t ch;

	fresh();
	CHECK(codeplug_set_radio_name("pd0abc"));
	ch = make("M17 Ops", 433475000, 433475000);
	CHECK(codeplug_set_channel(10, &ch));
	m17_conv_encode(&c, blk);
	CHECK(codeplug_tag_write(10, ch.name, &t, blk, sizeof blk));
	CHECK(codeplug_channel(10, &ch) && ch.kind == CODEPLUG_KIND_M17 && ch.system == 2 && !ch.rx_only);
	// Run where the board's air interface carries M17 (CODEPLUG_RUNS_M17), else parked; named as the registry says
	fits = false;
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_M17) && strcmp(codeplug_kind_name(CODEPLUG_KIND_M17), "M17") == 0 &&
	      !codeplug_systype_built(CODEPLUG_SYSTYPE_M17) && strcmp(codeplug_systype_name(CODEPLUG_SYSTYPE_M17), "M17") == 0);
	fits = true;
	CHECK(codeplug_kind_built(CODEPLUG_KIND_M17) && codeplug_systype_built(CODEPLUG_SYSTYPE_M17));
	// The system it names isn't here: no transmit, no system
	uint64_t own;
	CHECK(m17_addr_from_text("PD0ABC", &own));
	m17_conv_t c_built = as_built(c);
	CHECK(m17_conv_load(10, &ch, &cfg) && conv_eq(&cfg.conv, &c_built) && cfg.src == own && cfg.system == CODEPLUG_SYSTEM_NONE &&
	      !cfg.tx_allowed);
	// An M17 system that allows transmit; then one listen only; then another type's
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 2, sys, sizeof sys));
	CHECK(m17_conv_load(10, &ch, &cfg) && cfg.system == 2 && cfg.tx_allowed);
	sys[1] = 0;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 2, sys, sizeof sys));
	CHECK(m17_conv_load(10, &ch, &cfg) && cfg.system == 2 && !cfg.tx_allowed);
	sys[0] = CODEPLUG_SYSTYPE_NXDN;
	sys[1] = CODEPLUG_SYS_TX;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 2, sys, sizeof sys));
	CHECK(m17_conv_load(10, &ch, &cfg) && cfg.system == CODEPLUG_SYSTEM_NONE && !cfg.tx_allowed);
	// No system: transmit allowed (the channel's own flag still rules)
	t.system = CODEPLUG_SYSTEM_NONE;
	CHECK(codeplug_tag_write(10, ch.name, &t, blk, sizeof blk) && codeplug_channel(10, &ch));
	CHECK(m17_conv_load(10, &ch, &cfg) && cfg.system == CODEPLUG_SYSTEM_NONE && cfg.tx_allowed);
	// A tag with no block: the defaults (CAN 0, broadcast, clear)
	CHECK(codeplug_tag_write(10, ch.name, &t, NULL, 0) && codeplug_channel(10, &ch));
	CHECK(m17_conv_load(10, &ch, &cfg) && cfg.conv.can == 0 && cfg.conv.tx_can == 0 && cfg.conv.squelch == 0 &&
	      !cfg.conv.mode1600 && cfg.conv.dst == M17_ADDR_BROADCAST && cfg.conv.tx_enc == M17_ENC_NONE);
	// A plain channel, another kind's, a stale tag: no set-up
	channel_t plain = make("Plain", 433500000, 433500000);
	CHECK(codeplug_set_channel(11, &plain) && codeplug_channel(11, &plain) && !m17_conv_load(11, &plain, &cfg));
	t.kind = CODEPLUG_KIND_NXDN;
	plain = make("NXDN", 433600000, 433600000);
	CHECK(codeplug_set_channel(12, &plain) && codeplug_tag_write(12, plain.name, &t, NULL, 0));
	CHECK(codeplug_channel(12, &plain) && plain.kind == CODEPLUG_KIND_NXDN && !m17_conv_load(12, &plain, &cfg));
	channel_t stale = ch;
	strcpy(stale.name, "Renamed");
	CHECK(!m17_conv_load(10, &stale, &cfg));
}

// A channel written from the radio: the stand-in record, then a new tag (transmit allowed as the channel says); an
// edit after keeps the tag's system, fallback, other flags and what follows the block
static void test_conv_save(void)
{
	m17_conv_t c = { .squelch = 1, .can = 4, .tx_can = 4, .dst = M17_ADDR_BROADCAST };
	m17_conv_cfg_t back;
	codeplug_tag_t rt;
	channel_t ch, r;

	fresh();
	ch = make("Save", 433475000, 433475000);
	ch.mode = RADIO_MODE_DMR;
	ch.rx_only = false;
	CHECK(m17_conv_save(20, &ch, &c));
	CHECK(codeplug_channel(20, &r) && r.kind == CODEPLUG_KIND_M17 && !r.rx_only && r.mode == RADIO_MODE_DMR &&
	      r.system == CODEPLUG_SYSTEM_NONE);
	CHECK(codeplug_tag_read(20, &rt) && rt.flags == CODEPLUG_TAG_TX && rt.len == CODEPLUG_TAG_HDR + M17_CP_CONV_LEN);
	CHECK(m17_conv_load(20, &r, &back) && conv_eq(&back.conv, &c));
	codeplug_tag_remove(20);
	CHECK(codeplug_channel(20, &r) && r.kind == CODEPLUG_KIND_NONE && r.rx_only && r.mode == RADIO_MODE_DMR);
	// Receive only from the radio: the tag says so
	ch.rx_only = true;
	CHECK(m17_conv_save(21, &ch, &c) && codeplug_channel(21, &r) && r.kind == CODEPLUG_KIND_M17 && r.rx_only);

	// Over a tag of the kind with a system, a fallback, the kind's flags, reserved bits and bytes after the block
	uint8_t longer[M17_CP_CONV_LEN + 2];
	m17_conv_t old = { .can = 1, .tx_can = 1, .dst = 0x4B13D106u };
	m17_conv_encode(&old, longer);
	longer[0] |= 0xA8u;
	longer[M17_CP_CONV_LEN] = 0x5A;
	longer[M17_CP_CONV_LEN + 1] = 0xA5;
	codeplug_tag_t kt = { .kind = CODEPLUG_KIND_M17, .system = 1, .fb_chan = 3, .fb_check = 0x4321,
			      .flags = CODEPLUG_TAG_FALLBACK | 0x30u };
	channel_t base = make("Keep", 433475000, 433475000);
	CHECK(codeplug_set_channel(22, &base) && codeplug_tag_write(22, "Keep", &kt, longer, sizeof longer));
	CHECK(codeplug_channel(22, &r) && r.kind == CODEPLUG_KIND_M17 && r.system == 1 && r.rx_only);
	strcpy(r.name, "Kept");
	r.rx_only = false;
	m17_conv_t ed = { .squelch = 2, .mode1600 = true, .can = 15, .tx_can = 0, .dst = 0x1680B7u, .tx_enc = M17_ENC_SCRAMBLER,
			  .key_id = 0x0102 };
	CHECK(m17_conv_save(22, &r, &ed));
	CHECK(codeplug_channel(22, &r) && r.kind == CODEPLUG_KIND_M17 && r.system == 1 && !r.rx_only &&
	      strcmp(r.name, "Kept") == 0);
	CHECK(codeplug_tag_read(22, &rt) && rt.flags == (CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_TX | 0x30u) && rt.fb_chan == 3 &&
	      rt.fb_check == 0x4321 && rt.len == CODEPLUG_TAG_HEAD_MAX + sizeof longer);
	uint8_t raw[sizeof longer + CODEPLUG_TAG_HEAD_MAX];
	CHECK(codeplug_tag_params(22, &r, raw, sizeof longer) == (int)sizeof longer && (raw[0] & 0xF8u) == 0xA8u &&
	      raw[M17_CP_CONV_LEN] == 0x5A && raw[M17_CP_CONV_LEN + 1] == 0xA5);
	m17_conv_t ed_built = as_built(ed);
	CHECK(m17_conv_load(22, &r, &back) && conv_eq(&back.conv, &ed_built));
	// The tag of another kind at a number: replaced, the new block's reserved bits 0
	channel_t other = make("Other", 433475000, 433475000);
	kt.kind = CODEPLUG_KIND_NXDN;
	kt.flags = 0;
	CHECK(codeplug_set_channel(23, &other) && codeplug_tag_write(23, "Other", &kt, longer, sizeof longer));
	other.rx_only = false;
	CHECK(m17_conv_save(23, &other, &ed) && codeplug_channel(23, &r) && r.kind == CODEPLUG_KIND_M17 &&
	      r.system == CODEPLUG_SYSTEM_NONE);
	CHECK(codeplug_tag_params(23, &r, raw, sizeof longer) == (int)M17_CP_CONV_LEN && (raw[0] & 0xF8u) == 0);
	// A copy at another number takes the tag
	CHECK(codeplug_channel(22, &r) && codeplug_set_channel(24, &r) && codeplug_tag_copy(22, 24, &r));
	CHECK(codeplug_channel(24, &r) && r.kind == CODEPLUG_KIND_M17 && m17_conv_load(24, &r, &back) &&
	      conv_eq(&back.conv, &ed_built));
}

// Our address: the record's when it is a standard one, else the radio name where it encodes, else 0
static void test_own_addr(void)
{
	m17_wide_t w;
	bool bad = true;
	uint64_t a;

	fresh();
	CHECK(m17_own_addr(&bad) == 0 && !bad);			// no record, no radio name
	CHECK(codeplug_set_radio_name("n0call"));
	CHECK(m17_addr_from_text("N0CALL", &a) && m17_own_addr(&bad) == a && !bad && a == 0x4B13D106u);
	CHECK(codeplug_set_radio_name("N0_CALL"));		// not a callsign
	CHECK(m17_own_addr(&bad) == 0 && !bad);
	CHECK(codeplug_set_radio_name("W1AW"));
	memset(&w, 0, sizeof w);
	w.src = 0x9FDD51u;
	w.sign = true;
	memcpy(w.text, "Hi", 2);
	w.text_len = 2;
	CHECK(m17_wide_save(&w));
	CHECK(m17_own_addr(&bad) == 0x9FDD51u && !bad);
	CHECK(m17_own_addr(NULL) == 0x9FDD51u);
	m17_wide_t r;
	m17_wide_load(&r);
	CHECK(wide_eq(&r, &w));
	// Not a standard address: none, and said so (never the radio name instead)
	static const uint64_t not_std[] = { M17_ADDR_STD_MAX + 1u, 0xFFFFFFFFFFFEull, M17_ADDR_BROADCAST };
	for (unsigned i = 0; i < sizeof not_std / sizeof not_std[0]; i++) {
		w.src = not_std[i];
		CHECK(m17_wide_save(&w));
		bad = false;
		CHECK(m17_own_addr(&bad) == 0 && bad);
		CHECK(m17_own_addr(NULL) == 0);
	}
	// 0 is none: the radio name stands in
	w.src = 0;
	CHECK(m17_wide_save(&w) && m17_own_addr(&bad) == 0x1680B7u && !bad);
	// A record cut short (a CPS that wrote 3 bytes): no address, the radio name
	uint8_t three[3] = { 0, 0, 1 };
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, three, sizeof three));
	CHECK(m17_own_addr(&bad) == 0x1680B7u && !bad);
}

static bool zero(const uint8_t *p, int n)
{
	for (int i = 0; i < n; i++)
		if (p[i] != 0)
			return false;
	return true;
}

// Traffic key record n: system, algorithm, key ID, flags, the key
static void put_traffic(uint16_t n, uint8_t system, uint8_t alg, uint16_t id, const uint8_t *key, int len)
{
	uint8_t v[CODEPLUG_TRAFFIC_HDR + 40];

	v[0] = system;
	v[1] = alg;
	v[2] = (uint8_t)id;
	v[3] = (uint8_t)(id >> 8);
	v[4] = 0;
	memcpy(&v[CODEPLUG_TRAFFIC_HDR], key, (size_t)len);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_TRAFFIC + n), v, (uint16_t)(CODEPLUG_TRAFFIC_HDR + len)));
}

static void test_keys(void)
{
	uint8_t sys[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17, CODEPLUG_SYS_TX };
	uint8_t key[40], got[32];

	fresh();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 1, sys, sizeof sys));
	for (int i = 0; i < 40; i++)
		key[i] = (uint8_t)(0xC0 + i);
	// Every length, as each algorithm: only 1-3 (scrambler) and 16, 24, 32 (AES) are keys
	for (int len = 0; len <= 33; len++) {
		put_traffic(0, 1, M17_CP_ALG_SCRAMBLER, 100, key, len);
		put_traffic(1, 1, M17_CP_ALG_AES, 200, key, len);
		memset(got, 0x55, sizeof got);
		uint8_t n = m17_system_key(1, M17_ENC_SCRAMBLER, 100, got);
		CHECK(n == (len >= 1 && len <= 3 ? len : 0));
		CHECK(n ? memcmp(got, key, n) == 0 : zero(got, 32));
		memset(got, 0x55, sizeof got);
		n = m17_system_key(1, M17_ENC_AES, 200, got);
		CHECK(n == (len == 16 || len == 24 || len == 32 ? len : 0));
		CHECK(n ? memcmp(got, key, n) == 0 : zero(got, 32));	// a key read but refused is wiped
	}
	put_traffic(0, 1, M17_CP_ALG_SCRAMBLER, 100, key, 2);
	put_traffic(1, 1, M17_CP_ALG_AES, 200, key, 32);
	// The algorithm and the ID pick the key; another encryption, ID or system has none
	CHECK(m17_system_key(1, M17_ENC_SCRAMBLER, 200, got) == 0 && m17_system_key(1, M17_ENC_AES, 100, got) == 0);
	CHECK(m17_system_key(1, M17_ENC_NONE, 100, got) == 0 && m17_system_key(1, M17_ENC_OTHER, 100, got) == 0);
	CHECK(m17_system_key(2, M17_ENC_SCRAMBLER, 100, got) == 0 && m17_system_key(CODEPLUG_SYSTEM_NONE, M17_ENC_AES, 200, got) == 0);
	// An all-zero scrambler seed would send in the clear: missing
	static const uint8_t z3[3];
	put_traffic(2, 1, M17_CP_ALG_SCRAMBLER, 300, z3, 3);
	CHECK(m17_system_key(1, M17_ENC_SCRAMBLER, 300, got) == 0);
	// The system of another type: its keys aren't M17's
	sys[0] = CODEPLUG_SYSTYPE_NXDN;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 1, sys, sizeof sys));
	CHECK(m17_system_key(1, M17_ENC_AES, 200, got) == 0);
	sys[0] = CODEPLUG_SYSTYPE_M17;
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 1, sys, sizeof sys));
	CHECK(m17_system_key(1, M17_ENC_AES, 200, got) == 32);
	// No signing key on a system of another type, nor without a secret
	uint8_t d[32];
	bool has_q;
	CHECK(!m17_system_signing_key(1, d, NULL, &has_q) && !has_q);
	CHECK(!m17_system_signing_key(CODEPLUG_SYSTEM_NONE, d, NULL, &has_q));
}

// The signing system: the channel's when it is an M17 system with a secret, else the first that has one
static void test_signing_system(void)
{
	uint8_t m17[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17, CODEPLUG_SYS_TX };
	uint8_t nxdn[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_NXDN, CODEPLUG_SYS_TX };
	uint8_t d[32];

	fresh();
	memset(d, 0x11, sizeof d);
	CHECK(m17_signing_system(CODEPLUG_SYSTEM_NONE) == CODEPLUG_SYSTEM_NONE);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 0, nxdn, sizeof nxdn));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 0, d, sizeof d));		// another type's secret
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 2, m17, sizeof m17));		// no secret
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 5, m17, sizeof m17));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 5, d, sizeof d));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 7, m17, sizeof m17));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 7, d, sizeof d));
	uint8_t shorty[3] = { CODEPLUG_SYSTYPE_M17, 0, 0 };			// too short to be a system
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 1, shorty, sizeof shorty));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 1, d, sizeof d));
	CHECK(m17_signing_system(CODEPLUG_SYSTEM_NONE) == 5);
	CHECK(m17_signing_system(7) == 7);
	CHECK(m17_signing_system(2) == 5);		// an M17 system without one: the first that has one
	CHECK(m17_signing_system(0) == 5);		// not an M17 system
	CHECK(m17_signing_system(1) == 5);
	CHECK(m17_signing_system(200) == 5);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYS_KEY + 5) && m17_signing_system(CODEPLUG_SYSTEM_NONE) == 7);
	CHECK(m17_signing_system(250) == 7);
}

// Entry n: a station key of system for addr, key byte k (or a malformed one)
static void put_station(uint16_t n, uint8_t system, uint8_t cls, uint64_t addr, uint8_t prefix, uint8_t k, int len)
{
	uint8_t v[CODEPLUG_SYS_ENTRY_HDR + M17_CP_STATION_LEN + 4];

	memset(v, k, sizeof v);
	v[0] = system;
	v[1] = cls;
	v[2] = CODEPLUG_GROUP_NONE;
	v[3] = 0;
	m17_addr_put(&v[CODEPLUG_SYS_ENTRY_HDR], addr);
	v[CODEPLUG_SYS_ENTRY_HDR + M17_ADDR_BYTES] = prefix;
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), v, (uint16_t)len));
}

static void test_station_keys(void)
{
	uint8_t m17[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17 };
	uint8_t nxdn[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_NXDN };
	const uint64_t A = 0x4B13D106u, B = 0x1680B7u;
	const int full = CODEPLUG_SYS_ENTRY_HDR + M17_CP_STATION_LEN;
	uint8_t key[33];
	uint64_t addr;

	fresh();
	CHECK(!m17_station_known(A) && !m17_station_key_at(0, &addr, key));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 1, m17, sizeof m17));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 3, m17, sizeof m17));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 4, nxdn, sizeof nxdn));
	// Written out of entry number order, so the area's order isn't the key order
	put_station(9, 3, M17_CP_ENTRY_STATION, A, 2, 0x39, full);	// system 3's A
	put_station(2, 1, M17_CP_ENTRY_STATION, A, 3, 0x12, full + 4);	// system 1's A (longer: a later layout)
	put_station(7, 1, M17_CP_ENTRY_STATION, B, 2, 0x17, full);	// system 1's B
	put_station(4, 3, M17_CP_ENTRY_STATION, A, 3, 0x34, full);	// system 3's other A
	put_station(1, 1, M17_CP_ENTRY_STATION, A, 2, 0x11, full);	// system 1's other A
	put_station(5, 4, M17_CP_ENTRY_STATION, A, 2, 0x45, full);	// an NXDN system's: not a station key
	put_station(6, 1, 0, A, 2, 0x16, full);				// class 0
	put_station(8, 1, M17_CP_ENTRY_STATION, A, 4, 0x18, full);	// not a compressed key
	put_station(10, 1, M17_CP_ENTRY_STATION, A, 2, 0x1A, full - 1);	// cut short
	put_station(11, 9, M17_CP_ENTRY_STATION, A, 2, 0x1B, full);	// a system that isn't there

	// System 1's own first (entry order), then the others'
	static const uint8_t from1[] = { 0x11, 0x12, 0x34, 0x39 }, from3[] = { 0x34, 0x39, 0x11, 0x12 };
	for (uint8_t i = 0; i < 4; i++) {
		CHECK(m17_station_key(1, A, i, key) && key[0] == (from1[i] == 0x11 || from1[i] == 0x39 ? 2 : 3) &&
		      key[1] == from1[i] && key[32] == from1[i]);
		CHECK(m17_station_key(3, A, i, key) && key[1] == from3[i]);
		CHECK(m17_station_key(CODEPLUG_SYSTEM_NONE, A, i, key) && key[1] == from1[i]);	// in entry order
		CHECK(m17_station_key(4, A, i, key) && key[1] == from1[i]);	// not an M17 system: all are others
	}
	CHECK(!m17_station_key(1, A, 4, key) && !m17_station_key(3, A, 4, key) && !m17_station_key(CODEPLUG_SYSTEM_NONE, A, 4, key));
	CHECK(m17_station_key(3, B, 0, key) && key[1] == 0x17 && !m17_station_key(3, B, 1, key));
	CHECK(m17_station_known(A) && m17_station_known(B) && !m17_station_known(0x9FDD51u) && !m17_station_known(0));
	// Every one in entry order
	static const struct { uint64_t a; uint8_t k; } all[] = { { A, 0x11 }, { A, 0x12 }, { A, 0x34 }, { B, 0x17 }, { A, 0x39 } };
	for (uint16_t i = 0; i < 5; i++)
		CHECK(m17_station_key_at(i, &addr, key) && addr == all[i].a && key[1] == all[i].k);
	CHECK(!m17_station_key_at(5, &addr, key));
	// The system made another type: its keys go with it
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 3, nxdn, sizeof nxdn));
	CHECK(m17_station_key(3, A, 0, key) && key[1] == 0x11 && !m17_station_key(3, A, 2, key));
	CHECK(m17_station_key_at(2, &addr, key) && addr == B && !m17_station_key_at(3, &addr, key));
}

// --- The journal's copy (settings-store.md 4) ---

// Whether the 8-byte pattern p is anywhere in the journal's two sectors
static bool in_journal(const uint8_t *p)
{
	for (uint32_t a = JOURNAL; a + 8 <= JOURNAL + 2u * STORAGE_SECTOR; a++)
		if (memcmp(&sim_flash_data[a], p, 8) == 0)
			return true;
	return false;
}

static bool j1_blank(void)
{
	for (uint32_t i = 0; i < STORAGE_SECTOR; i++)
		if (sim_flash_data[JOURNAL + STORAGE_SECTOR + i] != 0xFF)
			return false;
	return true;
}

static void test_journal(void)
{
	static const uint8_t d[32] = { 0x3F, 0x9B, 0xC0, 0x00, 0xFA, 0xB8, 0x3A, 0xED, 0x45, 0x51, 0xEC, 0x40, 0xA5, 0xF6, 0xA7, 0xC1,
				       0x78, 0xB2, 0x55, 0xBA, 0x65, 0x4C, 0x06, 0x84, 0xD2, 0x42, 0xBC, 0x8F, 0x59, 0xF8, 0xDA, 0x18 };
	static const uint8_t aes[16] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF };
	uint8_t sys[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_M17, CODEPLUG_SYS_TX };
	static const uint8_t plain[8] = { 0x70, 0x6C, 0x61, 0x69, 0x6E, 0x21, 0x21, 0x21 };
	uint8_t got[32];
	bool has_q;

	fresh();
	// An ordinary write to an area with no secret: the journal keeps its copy (a replay could still use it)
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYSTEM + 0, sys, sizeof sys));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, plain, sizeof plain));
	CHECK(in_journal(plain) && !j1_blank());
	uint32_t erases = sim_flash_erases(JOURNAL + STORAGE_SECTOR);
	// The signing key written: the area is in the flash, the journal holds none of it
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 0, d, sizeof d));
	CHECK(m17_system_signing_key(0, got, NULL, &has_q) && memcmp(got, d, 32) == 0);
	CHECK(!in_journal(d) && !in_journal(&d[24]) && j1_blank());
	CHECK(sim_flash_erases(JOURNAL + STORAGE_SECTOR) == erases + 2);	// the write's, and the wipe's
	// Any write of an area that holds a key copies every key into the journal: wiped as well
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, d, 4));
	CHECK(!in_journal(d) && !in_journal(&d[24]) && j1_blank());
	// A traffic key, the same
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYS_KEY + 0));
	put_traffic(0, 0, M17_CP_ALG_AES, 1, aes, sizeof aes);
	CHECK(m17_system_key(0, M17_ENC_AES, 1, got) == 16 && !in_journal(aes) && !in_journal(&aes[8]) && j1_blank());
	// Its removal leaves an area without keys: the journal keeps the copy, which holds none
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC));
	CHECK(!j1_blank() && !in_journal(aes));
	// A channel record (a patch through the journal) keeps its journal
	channel_t ch = make("Journal ch", 433475000, 433475000);
	CHECK(codeplug_set_channel(5, &ch) && !j1_blank());

	// A CPS commit of the area's sector (codeplug_kv_commit), holding a key: wiped
	put_traffic(0, 0, M17_CP_ALG_AES, 1, aes, sizeof aes);
	static uint8_t sector[STORAGE_SECTOR];
	memcpy(sector, &sim_flash_data[CODEPLUG_KV_ADDR], STORAGE_SECTOR);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC) && !j1_blank());
	CHECK(codeplug_kv_commit(CODEPLUG_KV_ADDR, sector) && j1_blank() && !in_journal(aes));
	CHECK(m17_system_key(0, M17_ENC_AES, 1, got) == 16);
	// Another sector committed the same way keeps its journal
	static uint8_t other[STORAGE_SECTOR];
	memset(other, 0x42, sizeof other);
	CHECK(codeplug_kv_commit(0x3000u, other) && !j1_blank());

	// A copy left by a firmware without the rule (a raw write of the area's sector, as before), or by a cut between
	// the write and the erase: erased at boot
	put_traffic(1, 0, M17_CP_ALG_AES, 2, aes, sizeof aes);
	CHECK(j1_blank());
	memcpy(sector, &sim_flash_data[CODEPLUG_KV_ADDR], STORAGE_SECTOR);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC + 1) && codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC));
	CHECK(storage_sector_write(CODEPLUG_KV_ADDR, sector) && in_journal(aes));
	CHECK(storage_init());
	codeplug_init();
	CHECK(!in_journal(aes) && j1_blank());
	CHECK(m17_system_key(0, M17_ENC_AES, 2, got) == 16 && m17_system_key(0, M17_ENC_AES, 1, got) == 16);
	// After the wipe a reboot finds the region ours and nothing to replay
	storage_stats_t st;
	CHECK(storage_init() && storage_region() == STORAGE_REGION_OURS);
	storage_get_stats(&st);
	CHECK(st.replays == 0 && st.foreign_slots == 0);
	codeplug_init();
	CHECK(m17_system_key(0, M17_ENC_AES, 2, got) == 16);
	// Nothing needs the copy after the boot's replay: it goes at boot whatever it held, and the next write makes one
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC) && codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC + 1) && !j1_blank());
	CHECK(storage_init());
	codeplug_init();
	CHECK(j1_blank());
	CHECK(codeplug_kv_set(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17, plain, sizeof plain) && in_journal(plain));

	// A key write cut by a power loss: the boot replays a committed rewrite from the copy before it erases the copy.
	// Whole or not at all, and no key in the journal after the boot, also from a cut before the commit
	uint8_t v[CODEPLUG_TRAFFIC_HDR + sizeof aes] = { 0, M17_CP_ALG_AES, 1, 0, 0 };
	memcpy(&v[CODEPLUG_TRAFFIC_HDR], aes, sizeof aes);
	uint32_t units = sim_flash_units();
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC, v, sizeof v));
	uint32_t cost = sim_flash_units() - units;
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC));
	bool cut_seen = false;
	for (uint32_t at = 1; at < cost; at += cost / 40 + 1) {
		sim_flash_cut_after(at);
		bool ok = codeplug_kv_set(CODEPLUG_KEY_TRAFFIC, v, sizeof v);
		sim_flash_cut_after(0);
		sim_flash_power_on();
		const uint8_t *h0 = &sim_flash_data[JOURNAL];
		// "OJNL" with the done byte still 0xFF: the rewrite is committed and not finished
		bool committed = !ok && memcmp(h0, "OJNL", 4) == 0 && h0[16] == 0xFF;
		cut_seen = cut_seen || committed;
		CHECK(storage_init());
		codeplug_init();
		int n = m17_system_key(0, M17_ENC_AES, 1, got);
		CHECK(n == 0 || (n == 16 && memcmp(got, aes, 16) == 0));
		CHECK(!committed || n == 16);	// the replay had the copy
		CHECK(!in_journal(aes) && !in_journal(&aes[8]));
		CHECK(codeplug_kv_remove(CODEPLUG_KEY_TRAFFIC));
	}
	CHECK(cut_seen);
}

int main(void)
{
	test_vectors();
	test_addr();
	test_conv_load();
	test_conv_save();
	test_own_addr();
	test_keys();
	test_signing_system();
	test_station_keys();
	test_journal();
	sim_flash_free();
	printf("m17CodeplugTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
