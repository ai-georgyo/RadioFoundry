// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 reflector client (core/net/m17_net.c; docs/spec/m17-ip.md) against a fake IP gateway and a fake reflector:
// the packet codecs against packets built here by hand from the tables of docs/spec/m17-ip.md 3 and 4 (an address
// encoder and a CRC of this file's own, from m17.md's rules), the link's state machine (CONN and its retries, ACKN,
// NACK, PING and PONG, the 30 s loss, DISC and its retries, the reflector's DISC, the host link going down), stream
// packets and SMS both ways through a fake reflector that sends ours back as a reflector would, the MAGICs that are
// ignored, the radio task's rings, and the debug link's m17net section. No network: every datagram is the fake's
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "net/m17_net.h"
#include "m17/m17.h"
#include "m17/m17_frame.h"
#include "dmr/ipGateway.h"
#include "debuglink/dl_fmt.h"
#include "debuglink/dl_frame.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- the IP gateway ----
static ipGatewayUdpHandler bound;
static uint16_t bound_port;
static bool bind_fails;
static uint8_t frame_buf[IPGW_LOCAL_MAX];
#define SENT_MAX 512
typedef struct {
	uint32_t ip;
	uint16_t sport, dport;
	int len;
	uint8_t d[IPGW_UDP_PAYLOAD_MAX];
} sent_t;
static sent_t sent[SENT_MAX];
static int nsent;
static bool usb_busy;

uint8_t *ipGatewayLocalPayload(void) { return frame_buf; }
bool ipGatewayUdpBind(uint16_t port, ipGatewayUdpHandler handler)
{
	if (bind_fails)
		return false;
	bound = handler;
	bound_port = port;
	return true;
}
bool ipGatewayUdpSend(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	if (usb_busy)
		return false;
	if (nsent < SENT_MAX) {
		sent[nsent].ip = dstIp;
		sent[nsent].sport = srcPort;
		sent[nsent].dport = dstPort;
		sent[nsent].len = length;
		memcpy(sent[nsent].d, payload, (size_t)length);
		nsent++;
	}
	return true;
}

#define REFL_IP		0xC0000201u	// 192.0.2.1 (documentation range)
#define OTHER_IP	0xC0000202u
#define RADIO_IP	0x0C002715u

static uint32_t now = 1000u;
static bool up = true;

static void poll(void)
{
	m17_net_poll(now, up);
}

// The clock runs ms in 5 ms steps, the link polled at each
static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		poll();
	}
}

static void from(uint32_t ip, uint16_t port, const void *p, int n)
{
	bound(false, ip, port, RADIO_IP, M17_NET_PORT, (const uint8_t *)p, n);
}

static void from_refl(const void *p, int n)
{
	from(REFL_IP, M17_NET_PORT, p, n);
}

// The datagrams sent since mark with this MAGIC (and length, -1: any)
static int count_sent(int mark, const char *magic, int len)
{
	int n = 0;
	for (int i = mark; i < nsent; i++)
		if (memcmp(sent[i].d, magic, 4) == 0 && (len < 0 || sent[i].len == len))
			n++;
	return n;
}

static const sent_t *last_sent(const char *magic)
{
	for (int i = nsent; i-- > 0;)
		if (memcmp(sent[i].d, magic, 4) == 0)
			return &sent[i];
	return NULL;
}

// ---- references of this file's own, from the rules (m17.md 10, 2.8) ----
static uint8_t b40_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return (uint8_t)(c - 'A' + 1);
	if (c >= '0' && c <= '9')
		return (uint8_t)(c - '0' + 27);
	return c == '-' ? 37 : c == '/' ? 38 : c == '.' ? 39 : 0;
}

// The first character is the least significant base-40 digit
static uint64_t b40(const char *s)
{
	uint64_t a = 0;
	for (size_t i = strlen(s); i-- > 0;)
		a = a * 40u + b40_value(s[i]);
	return a;
}

static void put48(uint8_t *p, uint64_t a)
{
	for (int i = 5; i >= 0; i--) {
		p[i] = (uint8_t)a;
		a >>= 8;
	}
}

static uint64_t get48(const uint8_t *p)
{
	uint64_t a = 0;
	for (int i = 0; i < 6; i++)
		a = a << 8 | p[i];
	return a;
}

// Polynomial 0x5935, initial value 0xFFFF, no reflection, no final XOR, MSB first
static uint16_t ref_crc(const uint8_t *p, size_t n)
{
	uint16_t c = 0xFFFFu;
	for (size_t i = 0; i < n; i++) {
		for (int b = 7; b >= 0; b--) {
			bool in = ((p[i] >> b) & 1u) != 0;
			bool top = (c & 0x8000u) != 0;
			c = (uint16_t)(c << 1);
			if (in != top)
				c ^= 0x5935u;
		}
	}
	return c;
}

// A stream packet by hand: MAGIC 0..3, SID 4..5, LSD 6..33 (DST, SRC, TYPE, META), FN 34..35, payload 36..51, CRC
// 52..53 over 0..51 (crc_from 0) or 4..51 (the other reading of GAP 1)
static void hand_stream(uint8_t *p, uint16_t sid, uint64_t dst, uint64_t src, uint16_t type, uint16_t fn, uint8_t fill,
			unsigned crc_from)
{
	memcpy(p, "M17 ", 4);
	p[4] = (uint8_t)(sid >> 8);
	p[5] = (uint8_t)sid;
	put48(p + 6, dst);
	put48(p + 12, src);
	p[18] = (uint8_t)(type >> 8);
	p[19] = (uint8_t)type;
	memset(p + 20, 0, 14);
	p[34] = (uint8_t)(fn >> 8);
	p[35] = (uint8_t)fn;
	for (int i = 0; i < 16; i++)
		p[36 + i] = (uint8_t)(fill + i);
	uint16_t c = ref_crc(p + crc_from, 52u - crc_from);
	p[52] = (uint8_t)(c >> 8);
	p[53] = (uint8_t)c;
}

// An M17P packet by hand: MAGIC, the LSF (its CRC over 28 bytes), the type and data, their CRC
static int hand_packet(uint8_t *p, uint64_t dst, uint64_t src, uint8_t can, const uint8_t *data, int n)
{
	uint16_t type = (uint16_t)(can << 7);	// packet: bit 0 clear, no encryption, text META type
	memcpy(p, "M17P", 4);
	put48(p + 4, dst);
	put48(p + 10, src);
	p[16] = (uint8_t)(type >> 8);
	p[17] = (uint8_t)type;
	memset(p + 18, 0, 14);
	uint16_t c = ref_crc(p + 4, 28);
	p[32] = (uint8_t)(c >> 8);
	p[33] = (uint8_t)c;
	memcpy(p + 34, data, (size_t)n);
	c = ref_crc(data, (size_t)n);
	p[34 + n] = (uint8_t)(c >> 8);
	p[35 + n] = (uint8_t)c;
	return 36 + n;
}

static const char ACKN[] = "ACKN", NACK[] = "NACK", DISC4[] = "DISC";

static void ping(void)
{
	uint8_t p[10];
	memcpy(p, "PING", 4);
	put48(p + 4, b40("M17-XYZ"));
	from_refl(p, 10);
}

static m17_net_target_t target(void)
{
	m17_net_target_t t;
	memset(&t, 0, sizeof t);
	t.ip = REFL_IP;
	strcpy(t.name, "M17-XYZ");
	t.module = 'C';
	t.own = b40("N0CALL");
	return t;
}

// From any state to linked with t
static void link_to(const m17_net_target_t *t)
{
	CHECK(m17_net_link(M17_NET_FROM_UI, t));
	poll();
	from(t->ip, t->port ? t->port : M17_NET_PORT, ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);
}

static void unlink_now(void)
{
	m17_net_unlink(M17_NET_FROM_UI);
	poll();
	from_refl(DISC4, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF);
}

// ---- the codecs ----
static void test_codecs(void)
{
	uint8_t p[64], q[64];
	uint64_t own = b40("N0CALL");

	// The address encoder agrees with this file's own: m17_addr_from_text is the one CONN uses
	uint64_t a;
	CHECK(m17_addr_from_text("N0CALL", &a) && a == own);

	// CONN: "CONN", the from callsign (6), the module (1): 11 bytes; GAP 2: no module letter in the callsign
	CHECK(m17_net_put_conn(p, false, own, 'C') == 11);
	CHECK(memcmp(p, "CONN", 4) == 0 && get48(p + 4) == own && p[10] == 'C');
	CHECK(m17_net_from(own, 'C') == own);
	CHECK(m17_net_put_conn(p, true, own, 'Z') == 11 && memcmp(p, "LSTN", 4) == 0 && p[10] == 'Z');
	// PONG, DISC: 10 bytes
	CHECK(m17_net_put_ctl(p, "PONG", own) == 10 && memcmp(p, "PONG", 4) == 0 && get48(p + 4) == own);
	CHECK(m17_net_put_ctl(p, "DISC", own) == 10 && memcmp(p, "DISC", 4) == 0);

	// The CRC: m17.md 2.8's known answers through this file's reference, then the stream packet's over bytes 0-51
	CHECK(ref_crc((const uint8_t *)"A", 1) == 0x206Eu && ref_crc((const uint8_t *)"123456789", 9) == 0x772Bu);
	uint8_t lsd[M17_LSD_BYTES], pay[16];
	memset(lsd, 0, sizeof lsd);
	put48(lsd, b40("M17-XYZ C"));
	put48(lsd + 6, own);
	lsd[12] = 0x00;
	lsd[13] = 0x05;	// stream, voice 3200
	for (int i = 0; i < 16; i++)
		pay[i] = (uint8_t)(0xA0 + i);
	CHECK(m17_net_put_stream(p, 0x1234u, lsd, 0x8007u, pay) == 54);
	hand_stream(q, 0x1234u, b40("M17-XYZ C"), own, 0x0005u, 0x8007u, 0xA0, 0);
	CHECK(memcmp(p, q, 54) == 0);
	CHECK(memcmp(p, "M17 ", 4) == 0 && p[4] == 0x12 && p[5] == 0x34 && p[34] == 0x80 && p[35] == 0x07 &&
	      p[36] == 0xA0 && p[51] == 0xAF);
	CHECK(ref_crc(p, 54) == 0);	// the CRC follows what it covers: over both, 0
	bool alt = true;
	CHECK(m17_net_stream_ok(q, 54, &alt) && !alt);
	// The other reading (the MAGIC left out) is taken too, and said
	hand_stream(q, 0x1234u, b40("M17-XYZ C"), own, 0x0005u, 7, 0x10, 4);
	CHECK(m17_net_stream_ok(q, 54, &alt) && alt);
	// A changed byte, a wrong length, another MAGIC: refused
	hand_stream(q, 0x1234u, 0, own, 0x0005u, 7, 0x10, 0);
	q[40] ^= 1;
	CHECK(!m17_net_stream_ok(q, 54, &alt));
	hand_stream(q, 0x1234u, 0, own, 0x0005u, 7, 0x10, 0);
	CHECK(m17_net_stream_ok(q, 54, &alt) && !m17_net_stream_ok(q, 53, &alt) && !m17_net_stream_ok(q, 55, &alt));
	q[3] = 'P';
	CHECK(!m17_net_stream_ok(q, 54, &alt));

	// M17P: MAGIC, the LSF with its CRC, type, data and their CRC; at least 38 bytes
	uint8_t lsf[M17_LSF_BYTES], sms[] = { 0x05, 'h', 'i', 0 };
	m17_lsf_make(lsf, b40("W1AW"), own, (uint16_t)(3u << 7), NULL);
	int n = (int)m17_net_put_packet(p, sizeof p, lsf, sms, sizeof sms);
	CHECK(n == 4 + 30 + 4 + 2 && n == hand_packet(q, b40("W1AW"), own, 3, sms, sizeof sms) && memcmp(p, q, (size_t)n) == 0);
	const uint8_t *gl, *gd;
	uint16_t gn = 0;
	CHECK(m17_net_get_packet(q, (size_t)n, &gl, &gd, &gn) && gn == 4 && gd == q + 34 && gl == q + 4 &&
	      memcmp(gd, sms, 4) == 0);
	// One byte of data at the least (38 bytes): type and nothing else is refused both ways
	uint8_t one[] = { 0x05, 'x' };
	CHECK(m17_net_put_packet(p, sizeof p, lsf, one, 2) == 38 && m17_net_put_packet(p, sizeof p, lsf, one, 1) == 0);
	n = hand_packet(q, 1, own, 0, one, 1);
	CHECK(n == 37 && !m17_net_get_packet(q, 37, &gl, &gd, &gn));
	n = hand_packet(q, 1, own, 0, one, 2);
	CHECK(n == 38 && m17_net_get_packet(q, 38, &gl, &gd, &gn) && gn == 2);
	q[20] ^= 1;		// the LSF's CRC fails
	CHECK(!m17_net_get_packet(q, 38, &gl, &gd, &gn));
	n = hand_packet(q, 1, own, 0, one, 2);
	q[35] ^= 1;		// the payload's CRC fails
	CHECK(!m17_net_get_packet(q, 38, &gl, &gd, &gn));
	CHECK(m17_net_put_packet(p, 40, lsf, sms, sizeof sms) == 40 && m17_net_put_packet(p, 39, lsf, sms, sizeof sms) == 0);

	// GAP 5: a module as a destination, the designation left in 8 places and the module 9th
	CHECK(m17_net_module_addr("M17-XYZ", 'C') == b40("M17-XYZ C"));
	CHECK(m17_net_module_addr("URFXYZ", 'A') == b40("URFXYZ  A"));
	CHECK(m17_net_module_addr("M17-ABCD", 'A') == 0 && m17_net_module_addr("", 'A') == 0);

	// What the entry takes as a reflector and module
	char name[M17_NET_NAME_MAX + 1], mod = 0;
	CHECK(m17_net_parse_name("M17-XYZ C", name, &mod) && strcmp(name, "M17-XYZ") == 0 && mod == 'C');
	CHECK(m17_net_parse_name("URFXYZ  A", name, &mod) && strcmp(name, "URFXYZ") == 0 && mod == 'A');
	CHECK(m17_net_parse_name("m17-abc d  ", name, &mod) && strcmp(name, "M17-ABC") == 0 && mod == 'D');
	CHECK(m17_net_parse_name("A B", name, &mod) && strcmp(name, "A") == 0 && mod == 'B');
	CHECK(!m17_net_parse_name("M17-XYZC", name, &mod) && !m17_net_parse_name("M17-XYZ 1", name, &mod) &&
	      !m17_net_parse_name("M17-ABCD E", name, &mod) && !m17_net_parse_name(" C", name, &mod) &&
	      !m17_net_parse_name("", name, &mod) && !m17_net_parse_name("M1 7 C", name, &mod) &&
	      !m17_net_parse_name("M17_XY C", name, &mod));

	// A target: a unicast IPv4 address, a designation, a module letter, our standard callsign
	m17_net_target_t t = target();
	CHECK(m17_net_target_ok(&t));
	t.ip = 0x7F000001u;
	CHECK(!m17_net_target_ok(&t));
	t.ip = 0xE0000001u;
	CHECK(!m17_net_target_ok(&t));
	t.ip = 0x00000001u;
	CHECK(!m17_net_target_ok(&t));
	t = target();
	t.module = 'c';
	CHECK(!m17_net_target_ok(&t));
	t = target();
	t.own = 0;
	CHECK(!m17_net_target_ok(&t));
	t.own = M17_ADDR_STD_MAX + 1u;
	CHECK(!m17_net_target_ok(&t));
	t = target();
	t.name[0] = '\0';
	CHECK(!m17_net_target_ok(&t) && !m17_net_link(M17_NET_FROM_UI, &t));
}

// ---- the link ----
static void test_link(void)
{
	m17_net_target_t t = target();
	m17_net_status_t s;
	int mark;

	m17_net_start();
	CHECK(bound != NULL && bound_port == 17000);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF && !m17_net_talks() && !m17_net_linked());

	// CONN at once, to the reflector's port from ours; again every 2 s, five times, then a wait of 30 s
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t));
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "CONN", 11) == 1);
	const sent_t *c = last_sent("CONN");
	CHECK(c->ip == REFL_IP && c->dport == 17000 && c->sport == 17000 && get48(c->d + 4) == t.own && c->d[10] == 'C');
	run(1990);
	CHECK(count_sent(mark, "CONN", 11) == 1);
	run(20);
	CHECK(count_sent(mark, "CONN", 11) == 2);
	run(3 * 2000);
	CHECK(count_sent(mark, "CONN", 11) == 5 && m17_net_state() == M17_NET_CONNECTING);
	run(2000);
	m17_net_status(&s);
	CHECK(s.state == M17_NET_WAIT && s.why == M17_NET_WHY_NO_ANSWER && count_sent(mark, "CONN", 11) == 5);
	run(29000);
	CHECK(count_sent(mark, "CONN", 11) == 5);
	run(1100);
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "CONN", 11) == 6);

	// An ACKN from elsewhere (another address, another port) links nothing
	from(OTHER_IP, 17000, ACKN, 4);
	from(REFL_IP, 17001, ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING);
	m17_net_stats_t st;
	m17_net_stats(&st);
	CHECK(st.foreign == 2);
	// The reflector's ACKN: linked
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED && m17_net_talks() && m17_net_linked());
	m17_net_status(&s);
	CHECK(s.why == M17_NET_WHY_NONE && strcmp(s.t.name, "M17-XYZ") == 0 && s.t.module == 'C');

	// PONG on every PING, at once, and on our own timer at least every 10 s with no PING
	mark = nsent;
	ping();
	poll();
	CHECK(count_sent(mark, "PONG", 10) == 1 && get48(last_sent("PONG")->d + 4) == t.own);
	ping();
	ping();
	poll();
	CHECK(count_sent(mark, "PONG", 10) == 2);	// two PINGs between polls: one answer
	mark = nsent;
	for (int k = 0; k < 9; k++) {	// a PING every 3 s keeps it up; PONGs at least one per PING
		run(3000);
		ping();
		poll();
	}
	CHECK(count_sent(mark, "PONG", 10) >= 9 && m17_net_state() == M17_NET_LINKED);
	// No PINGs: our own PONGs come at least every 10 s ...
	mark = nsent;
	int last = nsent;
	uint32_t gap = 0, at = now;
	for (uint32_t ms = 0; ms < 29000; ms += 5) {
		run(5);
		if (nsent != last && memcmp(sent[nsent - 1].d, "PONG", 4) == 0) {
			gap = now - at > gap ? now - at : gap;
			at = now;
			last = nsent;
		}
	}
	CHECK(count_sent(mark, "PONG", 10) >= 3 && gap <= 10000u && m17_net_state() == M17_NET_LINKED);
	// ... and 30 s after the last thing received the link is lost: linking again at once
	mark = nsent;
	run(1100);
	m17_net_status(&s);
	m17_net_stats(&st);
	CHECK(st.losses == 1 && count_sent(mark, "CONN", 11) == 1 && s.state == M17_NET_CONNECTING);
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);
	// Anything from the reflector counts as life (a stream, a packet type not known)
	for (int k = 0; k < 8; k++) {
		run(5000);
		from_refl("M17H....", 8);
		poll();
	}
	CHECK(m17_net_state() == M17_NET_LINKED);

	// Unlink: DISC (10 bytes) every second until the 4-byte DISC comes, at most five times
	mark = nsent;
	m17_net_unlink(M17_NET_FROM_UI);
	poll();
	CHECK(m17_net_state() == M17_NET_UNLINKING && count_sent(mark, "DISC", 10) == 1 && !m17_net_talks());
	CHECK(get48(last_sent("DISC")->d + 4) == t.own);
	run(1010);
	CHECK(count_sent(mark, "DISC", 10) == 2);
	from_refl(DISC4, 4);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_USER);
	run(5000);
	CHECK(count_sent(mark, "DISC", 10) == 2);
	// ... with no answer: given up after five
	link_to(&t);
	mark = nsent;
	m17_net_unlink(M17_NET_FROM_UI);
	run(6000);
	CHECK(count_sent(mark, "DISC", 10) == 5 && m17_net_state() == M17_NET_OFF);

	// NACK: refused, and no more CONN
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_DEBUG, &t));
	poll();
	from_refl(NACK, 4);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_REFUSED);
	run(40000);
	CHECK(count_sent(mark, "CONN", 11) == 1);

	// The reflector's DISC (10 bytes): no answer to it, linking again after 30 s
	link_to(&t);
	mark = nsent;
	uint8_t d10[10];
	memcpy(d10, "DISC", 4);
	put48(d10 + 4, b40("M17-XYZ"));
	from_refl(d10, 10);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_WAIT && s.why == M17_NET_WHY_DISC && nsent == mark);
	run(29000);
	CHECK(count_sent(mark, "CONN", 11) == 0);
	run(1100);
	CHECK(count_sent(mark, "CONN", 11) == 1);
	from_refl(ACKN, 4);
	poll();

	// The host link down: waiting, and linking again at once when it is back
	up = false;
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_WAIT && s.why == M17_NET_WHY_NO_NET && !m17_net_talks());
	mark = nsent;
	up = true;
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "CONN", 11) == 1);
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);

	// Another reflector while linked: one DISC to the old, then CONN to the new
	m17_net_target_t t2 = target();
	t2.ip = OTHER_IP;
	t2.port = 17001;
	t2.module = 'D';
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t2));
	poll();
	CHECK(count_sent(mark, "DISC", 10) == 1 && sent[mark].ip == REFL_IP && count_sent(mark, "CONN", 11) == 1);
	c = last_sent("CONN");
	CHECK(c->ip == OTHER_IP && c->dport == 17001 && c->d[10] == 'D' && m17_net_state() == M17_NET_CONNECTING);
	from(OTHER_IP, 17001, ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);
	// The same target again: nothing sent
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t2));
	poll();
	CHECK(nsent == mark && m17_net_state() == M17_NET_LINKED);
	m17_net_unlink(M17_NET_FROM_DEBUG);
	poll();
	from(OTHER_IP, 17001, DISC4, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF);

	// Listen only: LSTN, linked, but PTT stays on the air
	t.listen = true;
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t));
	poll();
	CHECK(count_sent(mark, "LSTN", 11) == 1 && count_sent(mark, "CONN", -1) == 0);
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_linked() && !m17_net_talks());
	m17_net_frame_t f;
	memset(&f, 0, sizeof f);
	CHECK(!m17_net_tx_frame(&f) && m17_net_sms_slot() == NULL);
	unlink_now();

	// Unknown MAGICs are ignored, the link kept; a known MAGIC at a length it doesn't have counts as bad
	t.listen = false;
	link_to(&t);
	m17_net_stats(&st);
	uint32_t unk = st.unknown, bad = st.bad;
	from_refl("M17H", 4);
	from_refl("M17D....................", 26);
	from_refl("XYZWabcd", 8);
	uint8_t c37[37];
	memset(c37, 0, sizeof c37);
	memcpy(c37, "ACKN", 4);
	from_refl(c37, 37);
	memcpy(c37, "PING", 4);
	from_refl(c37, 11);
	from_refl("AB", 2);	// shorter than any MAGIC: dropped unseen
	poll();
	m17_net_stats(&st);
	CHECK(st.unknown == unk + 3 && st.bad == bad + 2 && m17_net_state() == M17_NET_LINKED);
	unlink_now();

	// A link asked with the port table full: refused, and said why
	bind_fails = true;
	m17_net_start();
	CHECK(m17_net_link(M17_NET_FROM_UI, &t));
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_NO_PORT);
	bind_fails = false;
	m17_net_start();
	// A busy USB endpoint: CONN comes again with the retry timer
	usb_busy = true;
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t));
	poll();
	CHECK(nsent == mark && m17_net_state() == M17_NET_CONNECTING);
	usb_busy = false;
	run(2010);
	CHECK(count_sent(mark, "CONN", 11) == 1);
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);
	unlink_now();
}

// A fake reflector: what the client sends to it goes back to the client as a legacy reflector forwards it to another
// client (DST re-addressed to that client, the CRC made again), one datagram per call
static void refl_echo(int mark, uint64_t to)
{
	int end = nsent;
	for (int i = mark; i < end; i++) {
		if (memcmp(sent[i].d, "M17 ", 4) != 0 || sent[i].len != 54)
			continue;
		uint8_t p[54];
		memcpy(p, sent[i].d, 54);
		put48(p + 6, to);
		uint16_t c = ref_crc(p, 52);
		p[52] = (uint8_t)(c >> 8);
		p[53] = (uint8_t)c;
		from_refl(p, 54);
	}
}

static void test_streams(void)
{
	m17_net_target_t t = target();
	m17_net_frame_t f, g;
	m17_net_stats_t st;
	int mark;

	m17_net_start();
	memset(&f, 0, sizeof f);
	CHECK(!m17_net_tx_frame(&f));	// not linked: refused
	link_to(&t);

	// Ours to the reflector: a stream to everyone goes to the module ("M17-XYZ C", GAP 5), one to a station as it is
	mark = nsent;
	f.sid = 0xBEEFu;
	f.type = 0x0005u;
	put48(f.src, t.own);
	put48(f.dst, M17_ADDR_BROADCAST);
	for (uint16_t fn = 0; fn < 4; fn++) {
		f.fn = fn == 3 ? (uint16_t)(fn | M17_FN_END) : fn;
		memset(f.stream, 0x30 + fn, 16);
		CHECK(m17_net_tx_frame(&f));
	}
	memset(&g, 0, sizeof g);
	CHECK(!m17_net_tx_frame(&g));	// the ring holds four: the fifth waits for the host link task
	poll();
	CHECK(count_sent(mark, "M17 ", 54) == 4);
	bool alt;
	for (int i = 0; i < 4; i++) {
		const uint8_t *p = sent[mark + i].d;
		CHECK(m17_net_stream_ok(p, 54, &alt) && !alt && ref_crc(p, 52) == (uint16_t)(p[52] << 8 | p[53]));
		CHECK(p[4] == 0xBE && p[5] == 0xEF && get48(p + 6) == b40("M17-XYZ C") && get48(p + 12) == t.own);
		CHECK(p[18] == 0x00 && p[19] == 0x05 && p[35] == i && (i < 3 ? p[34] == 0 : p[34] == 0x80));
		CHECK(p[36] == 0x30 + i && p[51] == 0x30 + i);
	}
	m17_net_status_t s;
	m17_net_status(&s);
	CHECK(!s.tx);
	mark = nsent;
	f.sid = 0x0102u;
	f.fn = M17_FN_END;
	put48(f.dst, b40("ECHO"));
	CHECK(m17_net_tx_frame(&f));
	poll();
	CHECK(count_sent(mark, "M17 ", 54) == 1 && get48(last_sent("M17 ")->d + 6) == b40("ECHO"));
	// A busy endpoint keeps the frame for the next poll
	mark = nsent;
	usb_busy = true;
	f.sid = 0x0203u;
	f.fn = 0;
	CHECK(m17_net_tx_frame(&f));
	poll();
	CHECK(nsent == mark);
	usb_busy = false;
	poll();
	CHECK(count_sent(mark, "M17 ", 54) == 1);

	// Round trip: the reflector sends ours back to us (re-addressed to us): every frame in order, its fields
	m17_net_rx_flush();
	mark = nsent;
	f.sid = 0x4242u;
	put48(f.dst, M17_ADDR_BROADCAST);
	for (uint16_t fn = 0; fn < 3; fn++) {
		f.fn = fn == 2 ? (uint16_t)(2u | M17_FN_END) : fn;
		memset(f.stream, 0x50 + fn, 16);
		CHECK(m17_net_tx_frame(&f));
	}
	poll();
	refl_echo(mark, t.own);
	CHECK(m17_net_rx_count() == 3);
	for (uint16_t fn = 0; fn < 3; fn++) {
		CHECK(m17_net_rx_take(&g));
		CHECK(g.sid == 0x4242u && g.type == 0x0005u && get48(g.src) == t.own && get48(g.dst) == t.own &&
		      (g.fn & M17_FN_MASK) == fn && ((g.fn & M17_FN_END) != 0) == (fn == 2) && g.stream[0] == 0x50 + fn &&
		      g.stream[15] == 0x50 + fn);
	}
	CHECK(!m17_net_rx_take(&g));

	// A stream from another station: its SRC in the state while it comes; nine frames in a ring of eight: one dropped
	m17_net_stats(&st);
	uint32_t drops = st.rx_dropped, streams = st.rx_streams;
	uint8_t p[54];
	for (uint16_t fn = 0; fn < 9; fn++) {
		hand_stream(p, 0x7777u, b40("M17-XYZ C"), b40("W1AW"), 0x0005u, fn, (uint8_t)fn, 0);
		from_refl(p, 54);
	}
	poll();
	m17_net_status(&s);
	m17_net_stats(&st);
	CHECK(s.rx && s.rx_src == b40("W1AW") && s.rx_dst == b40("M17-XYZ C"));
	CHECK(st.rx_dropped == drops + 1 && st.rx_streams == streams + 1 && m17_net_rx_count() == M17_NET_RX_RING);
	// Its last frame: the state says it is over; a stream with no last frame times out
	hand_stream(p, 0x7777u, 0, b40("W1AW"), 0x0005u, 9 | M17_FN_END, 9, 0);
	m17_net_rx_flush();
	from_refl(p, 54);
	poll();
	m17_net_status(&s);
	CHECK(!s.rx && m17_net_rx_count() == 1);
	hand_stream(p, 0x7778u, 0, b40("W1AW"), 0x0005u, 0, 0, 0);
	from_refl(p, 54);
	poll();
	m17_net_status(&s);
	CHECK(s.rx);
	run(1100);
	m17_net_status(&s);
	CHECK(!s.rx && m17_net_state() == M17_NET_LINKED);
	m17_net_rx_flush();
	// A bad CRC, the other CRC reading: the first dropped, the second taken and counted
	m17_net_stats(&st);
	hand_stream(p, 0x7779u, 0, b40("W1AW"), 0x0005u, 0, 0, 0);
	p[53] ^= 0xFF;
	from_refl(p, 54);
	hand_stream(p, 0x7779u, 0, b40("W1AW"), 0x0005u, 1, 1, 4);
	from_refl(p, 54);
	m17_net_stats_t st2;
	m17_net_stats(&st2);
	CHECK(st2.bad == st.bad + 1 && st2.crc_alt == st.crc_alt + 1 && m17_net_rx_count() == 1);
	m17_net_rx_flush();
	// A stream from another address is never ours
	hand_stream(p, 0x777Au, 0, b40("W1AW"), 0x0005u, 0, 0, 0);
	from(OTHER_IP, 17000, p, 54);
	CHECK(m17_net_rx_count() == 0);

	// SMS to the reflector: M17P, the LSF to the module when it is to everyone, the payload's CRC
	mark = nsent;
	m17_net_sms_t *o = m17_net_sms_slot();
	CHECK(o != NULL);
	o->src = t.own;
	o->dst = M17_ADDR_BROADCAST;
	o->can = 2;
	o->len = 4;
	memcpy(o->data, "\x05hi", 4);
	m17_net_sms_post();
	CHECK(m17_net_sms_slot() == NULL);	// one at a time
	poll();
	CHECK(count_sent(mark, "M17P", 40) == 1 && m17_net_sms_slot() != NULL);
	const uint8_t *gl, *gd;
	uint16_t gn;
	const sent_t *ps = last_sent("M17P");
	CHECK(m17_net_get_packet(ps->d, (size_t)ps->len, &gl, &gd, &gn) && gn == 4 && memcmp(gd, "\x05hi", 4) == 0);
	CHECK(get48(gl) == b40("M17-XYZ C") && get48(gl + 6) == t.own && m17_lsf_type(gl) == (2u << 7) &&
	      (m17_lsf_type(gl) & M17_TYPE_STREAM) == 0);
	// SMS from the reflector: into the slot; one to the module comes as broadcast
	uint8_t q[80];
	int n = hand_packet(q, b40("M17-XYZ C"), b40("W1AW"), 0, (const uint8_t *)"\x05hello", 7);
	from_refl(q, n);
	const m17_net_sms_t *in = m17_net_sms_peek();
	CHECK(in != NULL && in->src == b40("W1AW") && in->dst == M17_ADDR_BROADCAST && in->len == 7 && !in->cut &&
	      memcmp(in->data, "\x05hello", 7) == 0);
	// While it is unread the next is dropped and counted
	m17_net_stats(&st);
	n = hand_packet(q, t.own, b40("K1ABC"), 0, (const uint8_t *)"\x05yo", 4);
	from_refl(q, n);
	m17_net_stats(&st2);
	CHECK(st2.rx_dropped == st.rx_dropped + 1);
	m17_net_sms_done();
	from_refl(q, n);
	in = m17_net_sms_peek();
	CHECK(in != NULL && in->dst == t.own && in->src == b40("K1ABC"));
	m17_net_sms_done();
	// A long one is cut to the slot
	uint8_t big[300];
	big[0] = 0x05;
	memset(big + 1, 'a', sizeof big - 2);
	big[sizeof big - 1] = 0;
	uint8_t qb[400];
	n = hand_packet(qb, t.own, b40("K1ABC"), 0, big, sizeof big);
	from_refl(qb, n);
	in = m17_net_sms_peek();
	CHECK(in != NULL && in->cut && in->len == M17_NET_SMS_MAX);
	m17_net_sms_done();
	// A broken one is dropped
	n = hand_packet(q, t.own, b40("K1ABC"), 0, (const uint8_t *)"\x05yo", 4);
	q[n - 1] ^= 1;
	from_refl(q, n);
	CHECK(m17_net_sms_peek() == NULL);

	// Unlinked: what waits to go out is dropped, nothing is sent
	f.fn = 0;
	CHECK(m17_net_tx_frame(&f));
	o = m17_net_sms_slot();
	CHECK(o != NULL);
	o->len = 4;
	m17_net_sms_post();
	m17_net_unlink(M17_NET_FROM_UI);
	mark = nsent;
	poll();
	CHECK(count_sent(mark, "M17 ", -1) == 0 && count_sent(mark, "M17P", -1) == 0);
	// ... and nothing comes in
	hand_stream(p, 0x9999u, 0, b40("W1AW"), 0x0005u, 0, 0, 0);
	from_refl(p, 54);
	CHECK(m17_net_rx_count() == 0);
	from_refl(DISC4, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF);
}

// The debug link's section: every key, the target and the counters, within one reply
static void test_view(void)
{
	char text[DL_REPLY_MAX];
	dl_text_t t;
	m17_net_target_t tg = target();

	tg.port = 17001;
	m17_net_reset_stats();
	CHECK(m17_net_link(M17_NET_FROM_DEBUG, &tg));
	poll();
	from(REFL_IP, 17001, ACKN, 4);
	ping();		// from 17000: foreign
	poll();
	uint8_t p[54];
	hand_stream(p, 1, 0, b40("M17-M17 C"), 0x0005u, 0, 0, 0);
	from(REFL_IP, 17001, p, 54);
	poll();
	dl_text_init(&t, text, DL_REPLY_MAX - 6u - 16u);
	m17_net_view(&t);
	CHECK(!t.full);
	text[t.len] = '\0';
	static const char *const want[] = {
		"m17net.built=1\n", "m17net.state=linked\n", "m17net.why=none\n", "m17net.reflector=M17-XYZ\n",
		"m17net.module=C\n", "m17net.listen=0\n", "m17net.ip=192.0.2.1\n", "m17net.port=17001\n",
		"m17net.rx=1\n", "m17net.tx=0\n", "m17net.rx_src=M17-M17_C\n", "m17net.rx_packets=2\n",
		"m17net.tx_packets=1\n", "m17net.conns=1\n", "m17net.ackns=1\n", "m17net.nacks=0\n", "m17net.pings=0\n",
		"m17net.pongs=0\n", "m17net.discs_tx=0\n", "m17net.discs_rx=0\n", "m17net.links=1\n", "m17net.losses=0\n",
		"m17net.rx_streams=1\n", "m17net.rx_frames=1\n", "m17net.tx_streams=0\n", "m17net.tx_frames=0\n",
		"m17net.pkt_rx=0\n", "m17net.pkt_tx=0\n", "m17net.bad=0\n", "m17net.crc_alt=0\n", "m17net.unknown=0\n",
		"m17net.foreign=1\n", "m17net.rx_dropped=0\n", "m17net.tx_full=0\n", "m17net.tx_dropped=0\n",
		"m17net.send_busy=0\n",
	};
	size_t total = 0;
	for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
		bool found = strstr(text, want[i]) != NULL;
		if (!found)
			printf("missing %s", want[i]);
		CHECK(found);
		total += strlen(want[i]);
	}
	CHECK(total == t.len);	// nothing else
	m17_net_rx_flush();
	// Off: no target named
	m17_net_unlink(M17_NET_FROM_DEBUG);
	poll();
	from(REFL_IP, 17001, DISC4, 4);
	poll();
	dl_text_init(&t, text, sizeof text);
	m17_net_view(&t);
	text[t.len] = '\0';
	CHECK(strstr(text, "m17net.state=off\n") != NULL && strstr(text, "m17net.why=user\n") != NULL);
	// The widest: every value at its longest fits one reply. The first eleven keys' longest values (built, state
	// "connecting", why "no_answer", a designation, the module, listen, an address, a port, rx, tx, a 12-digit
	// extended address), then the counters' 4294967295
	static const uint8_t wide[] = { 1, 10, 9, M17_NET_NAME_MAX, 1, 1, 15, 5, 1, 1, 12 };
	size_t widest = 0;
	for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
		widest += (size_t)(strchr(want[i], '=') - want[i]) + 2u + (i < sizeof wide ? wide[i] : 10u);
	CHECK(widest <= DL_REPLY_MAX - 6u - 16u);
}

// The link's corners: a link asked while the host link is down, a PING or DISC while connecting, the reflector's own
// DISC ending ours, Unlink while waiting, two tasks asking in one poll
static void test_link_edges(void)
{
	m17_net_target_t t = target();
	m17_net_status_t s;
	uint8_t d10[10];
	int mark;

	m17_net_start();
	CHECK(m17_net_state() == M17_NET_OFF);
	memcpy(d10, "DISC", 4);
	put48(d10 + 4, b40("M17-XYZ"));

	// Asked while the host link is down: waiting for it with nothing sent, then CONN as soon as it is up
	up = false;
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t));
	run(100);
	m17_net_status(&s);
	CHECK(s.state == M17_NET_WAIT && s.why == M17_NET_WHY_NO_NET && nsent == mark);
	up = true;
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "CONN", 11) == 1);

	// While connecting: a PING gets no PONG, a DISC of either form changes nothing, ACKN still links
	mark = nsent;
	ping();
	from_refl(DISC4, 4);
	from_refl(d10, 10);
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "PONG", -1) == 0);
	from_refl(ACKN, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_LINKED);
	// A second ACKN changes nothing; a NACK once linked neither
	from_refl(ACKN, 4);
	from_refl(NACK, 4);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_LINKED && s.why == M17_NET_WHY_NONE);

	// Unlinking: the reflector's own 10-byte DISC ends ours as its acknowledgment would
	m17_net_unlink(M17_NET_FROM_UI);
	poll();
	CHECK(m17_net_state() == M17_NET_UNLINKING);
	from_refl(d10, 10);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_USER);

	// Unlink while waiting to link again: off at once, no DISC to a reflector we aren't linked to
	link_to(&t);
	from_refl(d10, 10);
	poll();
	CHECK(m17_net_state() == M17_NET_WAIT);
	mark = nsent;
	m17_net_unlink(M17_NET_FROM_UI);
	run(3000);
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_USER && nsent == mark);
	// Unlink once more while off: the last target is forgotten
	m17_net_unlink(M17_NET_FROM_DEBUG);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_NONE && s.t.name[0] == '\0');

	// Linked, then asked to unlink in the poll that finds the host link down: off, no DISC with nowhere to go
	link_to(&t);
	mark = nsent;
	up = false;
	m17_net_unlink(M17_NET_FROM_UI);
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_OFF && s.why == M17_NET_WHY_USER && nsent == mark);
	// ... or to link elsewhere: no DISC to the old one either, the new one waits for the host link
	up = true;
	link_to(&t);
	m17_net_target_t t3 = target();
	t3.ip = OTHER_IP;
	mark = nsent;
	up = false;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t3));
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_WAIT && s.why == M17_NET_WHY_NO_NET && s.t.ip == OTHER_IP && nsent == mark);
	up = true;
	poll();
	CHECK(m17_net_state() == M17_NET_CONNECTING && count_sent(mark, "CONN", 11) == 1 && sent[mark].ip == OTHER_IP);
	m17_net_unlink(M17_NET_FROM_UI);
	poll();
	from(OTHER_IP, M17_NET_PORT, DISC4, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF);

	// The UI and the debug link in one poll: the UI's is taken first, the debug link's wins
	m17_net_target_t t2 = target();
	t2.module = 'D';
	mark = nsent;
	CHECK(m17_net_link(M17_NET_FROM_UI, &t) && m17_net_link(M17_NET_FROM_DEBUG, &t2));
	poll();
	m17_net_status(&s);
	CHECK(s.state == M17_NET_CONNECTING && s.t.module == 'D' && last_sent("CONN")->d[10] == 'D');
	// A request from a task number out of range is the debug link's
	m17_net_unlink(M17_NET_FROM_COUNT);
	poll();
	CHECK(m17_net_state() == M17_NET_UNLINKING);
	from_refl(DISC4, 4);
	poll();
	CHECK(m17_net_state() == M17_NET_OFF);
}

int main(void)
{
	test_codecs();
	test_link();
	test_link_edges();
	test_streams();
	test_view();
	printf("m17NetTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
