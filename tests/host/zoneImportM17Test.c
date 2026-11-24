// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone sharing with M17 (docs/spec/zone-share.md 4.2 and 5.5, docs/spec/m17-codeplug.md 7) on a simulated flash: M17
// channels go with their parameter blocks, their system at its sharing level (whole: the system and its station
// keys, class 128 entries; identity: only that, and an M17 system has no identity to match), and the system's secret
// (the signing key) only where a record carries it at the whole level, which a radio never sends. A receiver keeps
// neither the station keys nor the secret: an over-the-air share never makes a key trusted or signs the user's calls.
// The receiver reads what came in through the M17 codec (core/m17/m17_codeplug.c)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_import.h"
#include "codeplug/zone_edit.h"
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
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 5, .flash = &sim_flash };

static settings_zs_import_t imports[SETTINGS_ZS_IMPORTS];
static uint8_t recbuf[ZS_RECORD_MAX], sent[ZS_RECORD_MAX];

// The M17 test key (p256.md 12): d, then Q compressed
static const uint8_t secret65[M17_CP_SECRET_LEN] = {
	0x3F, 0x9B, 0xC0, 0x00, 0xFA, 0xB8, 0x3A, 0xED, 0x45, 0x51, 0xEC, 0x40, 0xA5, 0xF6, 0xA7, 0xC1,
	0x78, 0xB2, 0x55, 0xBA, 0x65, 0x4C, 0x06, 0x84, 0xD2, 0x42, 0xBC, 0x8F, 0x59, 0xF8, 0xDA, 0x18,
	0x02, 0xB9, 0x19, 0x31, 0xF4, 0xDC, 0x5E, 0xFC, 0x54, 0xC2, 0x3C, 0x0B, 0x8C, 0x87, 0x70, 0x4C,
	0x45, 0x58, 0x5B, 0xD7, 0x91, 0x04, 0x6F, 0xEF, 0x05, 0xBF, 0xBF, 0xD5, 0xA0, 0xCE, 0x9B, 0x53, 0xB3,
};

#define N0CALL	0x4B13D106u
#define W1AW	0x1680B7u
#define AB1CD	0x9FDD51u

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	memset(imports, 0, sizeof(imports));
}

static channel_t make(const char *name, uint32_t rx)
{
	channel_t c;

	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = rx;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;	// the stand-in a channel of a kind is stored as
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

// An M17 system n: no identity, an empty block (m17-codeplug.md 4)
static void put_system(uint8_t n, uint8_t flags, uint8_t level, const char *name)
{
	uint8_t v[CODEPLUG_SYS_HDR];

	memset(v, 0, sizeof(v));
	v[0] = CODEPLUG_SYSTYPE_M17;
	v[1] = flags;
	v[2] = level;
	v[4] = 0x44;	// an own ID: never shared
	strncpy((char *)&v[12], name, CODEPLUG_NAME_LEN);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYSTEM + n), v, sizeof(v)));
}

// Entry n: system's station key for addr, its key bytes k after the prefix
static void put_station(uint16_t n, uint8_t system, uint64_t addr, uint8_t k)
{
	uint8_t v[CODEPLUG_SYS_ENTRY_HDR + M17_CP_STATION_LEN];

	memset(v, k, sizeof(v));
	v[0] = system;
	v[1] = M17_CP_ENTRY_STATION;
	v[2] = CODEPLUG_GROUP_NONE;
	v[3] = 0;
	m17_addr_put(&v[CODEPLUG_SYS_ENTRY_HDR], addr);
	v[CODEPLUG_SYS_ENTRY_HDR + M17_ADDR_BYTES] = 2;
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), v, sizeof(v)));
}

static void put_m17(uint16_t number, const char *name, uint32_t rx, uint8_t system, const m17_conv_t *c)
{
	channel_t ch = make(name, rx);
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_M17, .system = system, .flags = CODEPLUG_TAG_TX };
	uint8_t blk[M17_CP_CONV_LEN];

	m17_conv_encode(c, blk);
	CHECK(codeplug_set_channel(number, &ch) && codeplug_tag_write(number, name, &t, blk, sizeof(blk)));
}

static const m17_conv_t conv_a = { .squelch = 2, .mode1600 = false, .can = 5, .tx_can = 5, .dst = AB1CD,
				   .tx_enc = M17_ENC_AES, .key_id = 3 };
static const m17_conv_t conv_b = { .squelch = 1, .mode1600 = true, .can = 0, .tx_can = 9, .dst = M17_ADDR_BROADCAST };

static bool conv_eq(const m17_conv_t *a, const m17_conv_t *b)
{
	return a->squelch == b->squelch && a->mode1600 == b->mode1600 && a->can == b->can && a->tx_can == b->tx_can &&
	       a->dst == b->dst && a->tx_enc == b->tx_enc && a->key_id == b->key_id;
}

// The sender: system 2 shared whole (transmit allowed, its signing key, two station keys, a traffic key), system 4 by
// identity; channels: one on each, one on no system, a plain FM one. The zone lists them in that order
static void sender(void)
{
	static const uint8_t traffic[] = { 2, M17_CP_ALG_AES, 3, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	channel_t fm = make("FM", 145500000);
	zone_t z;

	fresh();
	put_system(2, CODEPLUG_SYS_TX, CODEPLUG_SHARE_WHOLE, "Net");
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 2, secret65, sizeof(secret65)));
	CHECK(codeplug_kv_set(CODEPLUG_KEY_TRAFFIC, traffic, sizeof(traffic)));
	put_station(10, 2, N0CALL, 0x11);
	put_station(3, 2, W1AW, 0x22);
	put_system(4, CODEPLUG_SYS_TX, CODEPLUG_SHARE_IDENTITY, "Other");
	put_station(5, 4, AB1CD, 0x33);	// an identity-only system's entries never go
	put_m17(1, "M17 Net", 433475000, 2, &conv_a);
	put_m17(2, "M17 Other", 433500000, 4, &conv_a);
	put_m17(3, "M17 Simplex", 433525000, CODEPLUG_SYSTEM_NONE, &conv_b);
	fm.mode = RADIO_MODE_FM;
	fm.rx_only = false;
	CHECK(codeplug_set_channel(4, &fm));
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "M17");
	z.count = 4;
	for (uint16_t i = 0; i < 4; i++) {
		z.channels[i] = (uint16_t)(i + 1);
	}
	CHECK(codeplug_set_zone(0, &z));
}

// A radio with a zone of its own
static void receiver(void)
{
	channel_t ch = make("Home", 146520000);
	zone_t z;

	fresh();
	ch.mode = RADIO_MODE_FM;
	ch.rx_only = false;
	CHECK(codeplug_set_channel(1, &ch));
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Home");
	z.count = 1;
	z.channels[0] = 1;
	CHECK(codeplug_set_zone(0, &z));
}

static int export_zone(void)
{
	uint8_t included = 0;
	zsRecord_t rec;
	zsSystem_t s;
	zsEntry_t e;
	zsTag_t t;

	sender();
	int len = zone_export_zone(0, recbuf, sizeof(recbuf), &included);
	CHECK(len > 0 && included == 4 && zsParse(recbuf, len, &rec) && (rec.flags & ZS_FLAG_MODES));
	CHECK(rec.systems == 2 && rec.tags == 3);
	// The whole system: no block, no identity, its station keys, and never its key from a radio
	CHECK(zsSystemAt(&rec, 0, &s) && s.type == CODEPLUG_SYSTYPE_M17 && s.level == ZS_LEVEL_WHOLE && s.idLen == 0 &&
	      s.blockLen == 0 && s.entries == 2 && s.secretLen == 0 && s.flags == CODEPLUG_SYS_TX && strcmp(s.name, "Net") == 0);
	// In the settings area's order, as every type's entries go (the area keeps them in the order they were written)
	CHECK(zsEntryAt(&rec, &s, 0, &e) && e.cls == M17_CP_ENTRY_STATION && e.len == M17_CP_STATION_LEN &&
	      m17_addr_get(e.body) == N0CALL && e.body[M17_ADDR_BYTES] == 2 && e.body[M17_ADDR_BYTES + 1] == 0x11);
	CHECK(zsEntryAt(&rec, &s, 1, &e) && m17_addr_get(e.body) == W1AW && e.body[M17_ADDR_BYTES + 1] == 0x22);
	CHECK(zsSystemAt(&rec, 1, &s) && s.level == ZS_LEVEL_IDENTITY && s.entries == 0 && s.blockLen == 0);
	uint8_t blk[M17_CP_CONV_LEN];
	m17_conv_encode(&conv_a, blk);
	CHECK(zsTagAt(&rec, 0, &t) && t.channel == 1 && t.kind == CODEPLUG_KIND_M17 && t.system == 1 &&
	      t.flags == CODEPLUG_TAG_TX && t.len == M17_CP_CONV_LEN && memcmp(t.params, blk, sizeof(blk)) == 0);
	CHECK(zsTagAt(&rec, 1, &t) && t.channel == 2 && t.system == 2);
	CHECK(zsTagAt(&rec, 2, &t) && t.channel == 3 && t.system == 0);
	// Nothing of the secrets is anywhere in the record
	for (int i = 0; i + 8 <= len; i++) {
		CHECK(memcmp(&recbuf[i], secret65, 8) != 0 && memcmp(&recbuf[i], &secret65[24], 8) != 0);
	}
	memcpy(sent, recbuf, (size_t)len);
	return len;
}

static void test_export_import(void)
{
	int len = export_zone();
	zone_import_result_t res;
	zsRecord_t rec;
	zone_t z;
	channel_t ch;
	m17_conv_cfg_t cfg;
	uint8_t key[33], d[32];
	uint64_t addr;
	bool has_q;

	// A radio with no M17 system: the whole one is added without its station keys, the identity-only one is missing
	receiver();
	CHECK(zsParse(sent, len, &rec) && !zone_import_unavailable(&rec, 0) && zone_import_unavailable(&rec, 1) &&
	      !zone_import_unavailable(&rec, 2) && !zone_import_unavailable(&rec, 3));
	CHECK(zone_import_zone(sent, len, 77, imports, &res) == ZI_OK && res.channels == 4 && res.systems_added == 1 &&
	      res.unavailable == 1);
	codeplug_system_t sys;
	uint8_t block[64];
	CHECK(codeplug_system(0, &sys, block, sizeof(block)) == 0 && sys.type == CODEPLUG_SYSTYPE_M17 &&
	      sys.flags == CODEPLUG_SYS_TX && sys.sharing == CODEPLUG_SHARE_WHOLE && sys.own_id == 0 &&
	      strcmp(sys.name, "Net") == 0);
	CHECK(codeplug_system(1, &sys, block, sizeof(block)) < 0);
	CHECK(codeplug_system_secret(0, block, sizeof(block)) < 0);		// no key came
	CHECK(codeplug_traffic_key(0, M17_CP_ALG_AES, 3, block, sizeof(block)) < 0);	// traffic keys never go
	CHECK(!m17_system_signing_key(0, d, NULL, &has_q) && m17_signing_system(CODEPLUG_SYSTEM_NONE) == CODEPLUG_SYSTEM_NONE);
	// The station keys that came stay out: none vouches for W1AW or N0CALL here
	CHECK(!m17_station_key(0, W1AW, 0, key) && !m17_station_key(CODEPLUG_SYSTEM_NONE, W1AW, 0, key));
	CHECK(!m17_station_known(N0CALL) && !m17_station_known(W1AW) && !m17_station_known(AB1CD));
	CHECK(!m17_station_key_at(0, &addr, key));
	for (uint16_t k = 0; k < 0x100u; k++) {
		CHECK(codeplug_kv_get((uint16_t)(CODEPLUG_KEY_SYS_CHAN + k), block, sizeof(block)) < 0);
	}

	CHECK(codeplug_zone(res.zone, &z) && z.count == 4);
	// On the system added: transmit as the sender had it, the system allows it
	CHECK(codeplug_channel(z.channels[0], &ch) && ch.kind == CODEPLUG_KIND_M17 && ch.system == 0 && !ch.rx_only);
	CHECK(m17_conv_load(z.channels[0], &ch, &cfg) && conv_eq(&cfg.conv, &conv_a) && cfg.system == 0 && cfg.tx_allowed);
	// On the missing system: no system, receive only
	CHECK(codeplug_channel(z.channels[1], &ch) && ch.kind == CODEPLUG_KIND_M17 && ch.system == CODEPLUG_SYSTEM_NONE &&
	      ch.rx_only);
	CHECK(m17_conv_load(z.channels[1], &ch, &cfg) && conv_eq(&cfg.conv, &conv_a));
	// On no system: receive only until the user allows transmit, its block byte for byte
	CHECK(codeplug_channel(z.channels[2], &ch) && ch.kind == CODEPLUG_KIND_M17 && ch.system == CODEPLUG_SYSTEM_NONE &&
	      ch.rx_only && ch.mode == RADIO_MODE_DMR && ch.rx_hz == 433525000);
	CHECK(m17_conv_load(z.channels[2], &ch, &cfg) && conv_eq(&cfg.conv, &conv_b) && cfg.tx_allowed);
	CHECK(codeplug_channel(z.channels[3], &ch) && ch.kind == CODEPLUG_KIND_NONE && ch.mode == RADIO_MODE_FM && ch.rx_only);

	// The same record again into a radio that has a whole M17 system of its own, with its own key for W1AW and its
	// signing key: no identity to match it by, so the sender's comes in as a system of its own, and the radio's is
	// left as it is. On the imported system's channel W1AW's key is still the radio's own, the only one, and the
	// radio still signs with its own key
	receiver();
	put_system(0, 0, CODEPLUG_SHARE_OFF, "Mine");
	put_station(0, 0, AB1CD, 0x44);
	put_station(1, 0, W1AW, 0x55);
	CHECK(codeplug_kv_set(CODEPLUG_KEY_SYS_KEY + 0, secret65, sizeof(secret65)));
	CHECK(zone_import_zone(sent, len, 77, imports, &res) == ZI_OK && res.systems_added == 1 && res.unavailable == 1);
	CHECK(codeplug_system(0, &sys, block, sizeof(block)) == 0 && sys.flags == 0 && strcmp(sys.name, "Mine") == 0);
	CHECK(codeplug_system(1, &sys, block, sizeof(block)) == 0 && strcmp(sys.name, "Net") == 0);
	CHECK(codeplug_zone(res.zone, &z) && codeplug_channel(z.channels[0], &ch) && ch.system == 1 && !ch.rx_only);
	CHECK(m17_station_key(1, W1AW, 0, key) && key[1] == 0x55 && !m17_station_key(1, W1AW, 1, key));
	CHECK(m17_station_key(1, AB1CD, 0, key) && key[1] == 0x44 && !m17_station_known(N0CALL));
	CHECK(m17_station_key_at(0, &addr, key) && m17_station_key_at(1, &addr, key) && !m17_station_key_at(2, &addr, key));
	CHECK(m17_signing_system(1) == 0 && m17_signing_system(CODEPLUG_SYSTEM_NONE) == 0);
}

// A crafted record that brings four station keys for W1AW (more than the verification tries) and a signing key, into a
// radio with its own W1AW key: none of them comes in, so W1AW's real key is still the first one tried, and the radio
// signs with no key it didn't make
static void test_crafted_keys(void)
{
	zsWriter_t w;
	zsChannel_t zc;
	zsSystem_t s;
	zsTag_t t;
	zone_import_result_t res;
	zone_t z;
	channel_t ch;
	uint8_t blk[M17_CP_CONV_LEN], body[4][M17_CP_STATION_LEN], key[33], d[32];
	zsEntry_t ent[4];
	bool has_q;

	m17_conv_encode(&conv_a, blk);
	zsWriterBegin(&w, recbuf, ZS_RECORD_MAX, 0x4321, 0, "Crafted");
	memset(&zc, 0, sizeof(zc));
	strcpy(zc.name, "M17 Bait");
	zc.rxHz = 433475000;
	zc.dmr = true;
	zc.colorCode = 1;
	zc.timeslot = 1;
	zc.rxOnly = true;
	CHECK(zsWriterChannel(&w, &zc));
	zsWriterModes(&w, 1);
	memset(&s, 0, sizeof(s));
	s.type = CODEPLUG_SYSTYPE_M17;
	s.flags = CODEPLUG_SYS_TX;
	s.level = ZS_LEVEL_WHOLE;
	strcpy(s.name, "Bait");
	s.secret = secret65;
	s.secretLen = 32;
	for (uint8_t k = 0; k < 4; k++) {
		memset(body[k], 0x60 + k, sizeof(body[k]));
		m17_addr_put(body[k], W1AW);
		body[k][M17_ADDR_BYTES] = 3;
		memset(&ent[k], 0, sizeof(ent[k]));
		ent[k].cls = M17_CP_ENTRY_STATION;
		ent[k].group = CODEPLUG_GROUP_NONE;
		ent[k].len = M17_CP_STATION_LEN;
		ent[k].body = body[k];
	}
	s.entries = 4;
	CHECK(zsWriterSystem(&w, &s));
	for (uint8_t k = 0; k < 4; k++) {
		CHECK(zsWriterEntry(&w, &ent[k]));
	}
	CHECK(zsWriterTags(&w, 1));
	memset(&t, 0, sizeof(t));
	t.channel = 1;
	t.kind = CODEPLUG_KIND_M17;
	t.system = 1;
	t.flags = CODEPLUG_TAG_TX;
	t.len = sizeof(blk);
	t.params = blk;
	CHECK(zsWriterTag(&w, &t));
	int len = zsWriterEnd(&w);
	CHECK(len > 0);

	receiver();
	put_system(5, 0, CODEPLUG_SHARE_OFF, "Mine");
	put_station(40, 5, W1AW, 0x77);
	CHECK(zone_import_zone(recbuf, len, 99, imports, &res) == ZI_OK && res.channels == 1 && res.systems_added == 1);
	CHECK(codeplug_zone(res.zone, &z) && codeplug_channel(z.channels[0], &ch) && ch.kind == CODEPLUG_KIND_M17 &&
	      ch.system == 0);
	CHECK(m17_station_key(ch.system, W1AW, 0, key) && key[0] == 2 && key[1] == 0x77 && !m17_station_key(ch.system, W1AW, 1, key));
	CHECK(codeplug_system_secret(0, d, sizeof(d)) < 0 && !m17_system_signing_key(0, d, NULL, &has_q));
	CHECK(m17_signing_system(ch.system) == CODEPLUG_SYSTEM_NONE && m17_signing_system(CODEPLUG_SYSTEM_NONE) == CODEPLUG_SYSTEM_NONE);
}

// A record from a sender that adds the signing key (the "whole system with key" level with keys on: no radio does,
// the format allows it; d alone fits the record's limit): the system comes in, its secret does not
static void test_secret(void)
{
	zsWriter_t w;
	zsChannel_t zc;
	zsSystem_t s;
	zsTag_t t;
	zone_import_result_t res;
	zone_t z;
	channel_t ch;
	uint8_t blk[M17_CP_CONV_LEN], d[32], q[33];
	bool has_q = true;

	m17_conv_encode(&conv_a, blk);
	for (int level = ZS_LEVEL_IDENTITY; level <= ZS_LEVEL_WHOLE; level++) {
		zsWriterBegin(&w, recbuf, ZS_RECORD_MAX, 0x1234, 0, "Keyed");
		memset(&zc, 0, sizeof(zc));
		strcpy(zc.name, "M17 Keyed");
		zc.rxHz = 433475000;
		zc.dmr = true;
		zc.colorCode = 1;
		zc.timeslot = 1;
		zc.rxOnly = true;
		CHECK(zsWriterChannel(&w, &zc));
		zsWriterModes(&w, 1);
		memset(&s, 0, sizeof(s));
		s.type = CODEPLUG_SYSTYPE_M17;
		s.flags = CODEPLUG_SYS_TX;
		s.level = (uint8_t)level;
		strcpy(s.name, "Keyed net");
		// A 65-byte secret is past the record's limit (64): a sender would send d alone
		s.secret = secret65;
		s.secretLen = sizeof(secret65);
		if (level == ZS_LEVEL_WHOLE) {
			zsWriter_t probe = w;
			uint8_t keep[ZS_RECORD_MAX];
			memcpy(keep, recbuf, sizeof(keep));
			CHECK(!zsWriterSystem(&probe, &s));
			memcpy(recbuf, keep, sizeof(keep));
			s.secretLen = 32;
		} else {
			s.secretLen = 0;	// an identity carries no secret
		}
		CHECK(zsWriterSystem(&w, &s));
		CHECK(zsWriterTags(&w, 1));
		memset(&t, 0, sizeof(t));
		t.channel = 1;
		t.kind = CODEPLUG_KIND_M17;
		t.system = 1;
		t.flags = CODEPLUG_TAG_TX;
		t.len = sizeof(blk);
		t.params = blk;
		CHECK(zsWriterTag(&w, &t));
		int len = zsWriterEnd(&w);
		CHECK(len > 0);

		receiver();
		CHECK(zone_import_zone(recbuf, len, 99, imports, &res) == ZI_OK && res.channels == 1);
		CHECK(codeplug_zone(res.zone, &z) && codeplug_channel(z.channels[0], &ch) && ch.kind == CODEPLUG_KIND_M17);
		if (level == ZS_LEVEL_IDENTITY) {
			// Not here, and an identity can't add it: no system, no key, receive only
			CHECK(res.systems_added == 0 && res.unavailable == 1 && ch.system == CODEPLUG_SYSTEM_NONE && ch.rx_only);
			CHECK(m17_signing_system(CODEPLUG_SYSTEM_NONE) == CODEPLUG_SYSTEM_NONE);
			continue;
		}
		// On a system the radio now has: transmit as the tag says, the system's flag rules (zone-share.md 5.5). The
		// secret stays out: a key the user never made never signs their calls (m17-codeplug.md 7)
		CHECK(res.systems_added == 1 && res.unavailable == 0 && ch.system == 0 && !ch.rx_only);
		CHECK(codeplug_system_secret(0, d, sizeof(d)) < 0 && !m17_system_signing_key(0, d, q, &has_q));
		CHECK(m17_signing_system(ch.system) == CODEPLUG_SYSTEM_NONE);
	}
}

// A failed import takes the system, its station keys and its secret with it: no room for the last channel
static void test_undo(void)
{
	int len = export_zone();
	zone_import_result_t res;
	codeplug_system_t sys;
	uint8_t block[64];
	uint64_t addr;
	uint8_t key[33];

	receiver();
	for (uint16_t n = 2; n <= CODEPLUG_CHANNELS_MAX - 3; n++) {
		channel_t fill = make("Fill", 146000000);
		fill.mode = RADIO_MODE_FM;
		CHECK(codeplug_set_channel(n, &fill));
	}
	CHECK(zone_import_zone(sent, len, 77, imports, &res) == ZI_NO_CHANNELS);
	CHECK(codeplug_system(0, &sys, block, sizeof(block)) < 0 && !m17_station_key_at(0, &addr, key));
}

int main(void)
{
	test_export_import();
	test_secret();
	test_crafted_keys();
	test_undo();
	sim_flash_free();
	printf("zoneImportM17Test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
