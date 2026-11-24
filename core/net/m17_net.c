// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 reflector client (m17_net.h; docs/spec/m17-ip.md): the packet codecs, the link in the host link task, and
// the hand-overs to the radio task and to any task
#include "config.h"
#if CONFIG_NET_M17	// the whole file
#include <stddef.h>
#include <string.h>
#include "net/m17_net.h"
#include "m17/m17_frame.h"
#include "dmr/ipGateway.h"
#include "debuglink/dl_fmt.h"

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define BARRIER()	__atomic_signal_fence(__ATOMIC_SEQ_CST)

#if (M17_NET_RX_RING & (M17_NET_RX_RING - 1)) != 0 || (M17_NET_TX_RING & (M17_NET_TX_RING - 1)) != 0 || \
	M17_NET_RX_RING > 128 || M17_NET_TX_RING > 128
#error "the M17 network rings are powers of two up to 128"
#endif
_Static_assert(M17_NET_PKT_MIN == 4u + M17_LSF_BYTES + 4u, "M17P: MAGIC, LSF, type, one byte, CRC");
_Static_assert(4u + M17_LSF_BYTES + M17_NET_SMS_MAX + 2u <= IPGW_UDP_PAYLOAD_MAX, "an SMS fits one datagram");

static const uint8_t MAGIC_STREAM[4] = { 'M', '1', '7', ' ' };
static const uint8_t MAGIC_PACKET[4] = { 'M', '1', '7', 'P' };

// --- Gap decisions (docs/spec/m17-ip.md 8): each one here, so a live test that settles it changes one place ---

// GAP 1, the stream packet's CRC. The sources say it covers "the entire packet", which can't include the CRC itself:
// we read bytes 0-51, the MAGIC included, and send that. A packet received is also taken when its CRC holds over bytes
// 4-51 (the MAGIC left out), the other reading, and counted in crc_alt: a live test shows which one reflectors use
#define CRC_FROM	0u
#define CRC_FROM_ALT	4u

uint16_t m17_net_stream_crc(const uint8_t *p)
{
	return m17_crc(p + CRC_FROM, M17_NET_STREAM_LEN - 2u - CRC_FROM);
}

bool m17_net_stream_ok(const uint8_t *p, size_t n, bool *alt)
{
	uint16_t got;

	*alt = false;
	if (n != M17_NET_STREAM_LEN || memcmp(p, MAGIC_STREAM, 4) != 0)
		return false;
	got = (uint16_t)(p[M17_NET_STREAM_LEN - 2u] << 8 | p[M17_NET_STREAM_LEN - 1u]);
	if (got == m17_net_stream_crc(p))
		return true;
	*alt = got == m17_crc(p + CRC_FROM_ALT, M17_NET_STREAM_LEN - 2u - CRC_FROM_ALT);
	return *alt;
}

// GAP 2, the from callsign of CONN, LSTN, PONG and DISC. Older texts put the client's module letter in its 9th place;
// Part II doesn't. Ours is our callsign as it is (the module goes in CONN's 11th byte only)
uint64_t m17_net_from(uint64_t own, char module)
{
	(void)module;
	return own;
}

// GAP 5, the destination of our streams and packets. Legacy reflectors forward only what is addressed to the linked
// module ("M17-XYZ C"); newer ones change that form to broadcast. A call to everyone (broadcast) goes as that form; one
// to a station or a reflector service (ECHO, INFO) as it is
static uint64_t net_dst(uint64_t dst, const m17_net_target_t *t)
{
	return dst == M17_ADDR_BROADCAST ? m17_net_module_addr(t->name, t->module) : dst;
}

// GAP 7: the two-packet stream ("M17H", "M17D") of an older text is not part of Part II and isn't spoken here: their
// MAGICs are unknown ones (ignored, counted)

// --- Packet codecs ---
static void put_addr(uint8_t *p, uint64_t a)
{
	m17_addr_put(p, a);
}

size_t m17_net_put_conn(uint8_t *out, bool listen, uint64_t own, char module)
{
	memcpy(out, listen ? "LSTN" : "CONN", 4);
	put_addr(out + 4, m17_net_from(own, module));
	out[10] = (uint8_t)module;
	return M17_NET_CONN_LEN;
}

size_t m17_net_put_ctl(uint8_t *out, const char magic[4], uint64_t own)
{
	memcpy(out, magic, 4);
	put_addr(out + 4, own);
	return M17_NET_CTL_LEN;
}

size_t m17_net_put_stream(uint8_t *out, uint16_t sid, const uint8_t lsd[M17_LSD_BYTES], uint16_t fn,
			  const uint8_t payload[M17_STREAM_BYTES])
{
	uint16_t crc;

	memcpy(out, MAGIC_STREAM, 4);
	out[4] = (uint8_t)(sid >> 8);
	out[5] = (uint8_t)sid;
	memcpy(out + 6, lsd, M17_LSD_BYTES);
	out[34] = (uint8_t)(fn >> 8);
	out[35] = (uint8_t)fn;
	memcpy(out + 36, payload, M17_STREAM_BYTES);
	crc = m17_net_stream_crc(out);
	out[52] = (uint8_t)(crc >> 8);
	out[53] = (uint8_t)crc;
	return M17_NET_STREAM_LEN;
}

size_t m17_net_put_packet(uint8_t *out, size_t cap, const uint8_t lsf[M17_LSF_BYTES], const uint8_t *data,
			  uint16_t len)
{
	size_t n = 4u + M17_LSF_BYTES + len + 2u;
	uint16_t crc;

	// At least one byte of data after the type (the packet is at least 38 bytes: 37 is the reflectors' own length)
	if (len < 2u || len + 2u > M17_NET_PKT_PAYLOAD_MAX || n > cap)
		return 0;
	memcpy(out, MAGIC_PACKET, 4);
	memcpy(out + 4, lsf, M17_LSF_BYTES);
	memcpy(out + 4 + M17_LSF_BYTES, data, len);
	crc = m17_crc(data, len);
	out[n - 2u] = (uint8_t)(crc >> 8);
	out[n - 1u] = (uint8_t)crc;
	return n;
}

bool m17_net_get_packet(const uint8_t *p, size_t n, const uint8_t **lsf, const uint8_t **data, uint16_t *len)
{
	size_t pl = n - 4u - M17_LSF_BYTES;

	if (n < M17_NET_PKT_MIN || memcmp(p, MAGIC_PACKET, 4) != 0 || pl > M17_NET_PKT_PAYLOAD_MAX)
		return false;
	if (!m17_lsf_crc_ok(p + 4) || m17_crc(p + 4 + M17_LSF_BYTES, pl) != 0)
		return false;	// the CRC over the data and its CRC is 0 (m17.md 2.8)
	*lsf = p + 4;
	*data = p + 4 + M17_LSF_BYTES;
	*len = (uint16_t)(pl - 2u);
	return true;
}

uint64_t m17_net_module_addr(const char *name, char module)
{
	char t[M17_CALL_MAX + 1];
	uint64_t a = 0;
	size_t n = strlen(name);

	if (n == 0 || n > M17_NET_NAME_MAX)
		return 0;
	memset(t, ' ', M17_CALL_MAX);
	memcpy(t, name, n);
	t[M17_CALL_MAX - 1u] = module;
	t[M17_CALL_MAX] = '\0';
	return m17_addr_from_text(t, &a) ? a : 0;
}

static bool name_char(char c)
{
	return c != ' ' && m17_addr_value(c) != 0;
}

bool m17_net_parse_name(const char *text, char name[M17_NET_NAME_MAX + 1], char *module)
{
	size_t n = strlen(text), k;

	while (n > 0 && text[n - 1u] == ' ')
		n--;		// trailing spaces don't count
	// The designation, at least one space, the module letter; nine places at most, the letter in the last at most
	if (n < 3 || n > M17_CALL_MAX || text[n - 2u] != ' ')
		return false;
	char m = text[n - 1u];
	if (m >= 'a' && m <= 'z')
		m = (char)(m - 'a' + 'A');
	if (m < 'A' || m > 'Z')
		return false;
	k = n - 2u;
	while (k > 0 && text[k - 1u] == ' ')
		k--;
	if (k == 0 || k > M17_NET_NAME_MAX)
		return false;
	for (size_t i = 0; i < k; i++) {
		char c = text[i];

		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if (!name_char(c))
			return false;
		name[i] = c;
	}
	name[k] = '\0';
	*module = m;
	return true;
}

bool m17_net_target_ok(const m17_net_target_t *t)
{
	size_t n = strnlen(t->name, sizeof t->name);
	uint32_t top = t->ip >> 24;

	if (n == 0 || n > M17_NET_NAME_MAX || t->module < 'A' || t->module > 'Z')
		return false;
	for (size_t i = 0; i < n; i++)
		if (!name_char(t->name[i]))
			return false;
	// A unicast IPv4 address: not 0/8, loopback, multicast or above
	if (top == 0 || top == 127 || top >= 224)
		return false;
	return t->own != 0 && t->own <= M17_ADDR_STD_MAX;
}

// --- State ---
enum { REQ_NONE, REQ_LINK, REQ_UNLINK };

// The request slots, one per asking task (m17_net.h): its writer steps seq to odd, writes, steps it to even; the host
// link task takes an even seq it hasn't seen once the copy held
typedef struct {
	uint8_t seq;
	uint8_t op;
	m17_net_target_t t;
} req_t;
static req_t req[M17_NET_FROM_COUNT];
// The state any task copies: the host link task writes it inside an odd / even sequence count
static m17_net_status_t pub;
static uint8_t pub_seq;
static uint8_t state_byte;		// pub.state, readable alone
static uint8_t talks_byte;		// linked, not listen only

// The host link task's own
static struct {
	bool bound;
	bool up;			// the latest poll's: the host link is up (nothing is sent while it isn't)
	uint8_t req_seen[M17_NET_FROM_COUNT];
	uint32_t now;			// the latest poll's time (the UDP handler has none of its own)
	uint8_t state, why;
	m17_net_target_t t;
	uint8_t tries;			// CONNs or DISCs sent in this round
	uint32_t sent_ms;		// the latest of them
	uint32_t retry_ms;		// WAIT: link again from here
	uint32_t rx_ms;			// anything from the reflector
	uint32_t pong_ms;		// our latest PONG
	bool pong_due;			// a PING to answer
	bool disc_due;			// the reflector's DISC to act on
	bool dirty;			// pub needs a rewrite
	uint16_t rx_sid;		// the stream coming in
	uint64_t rx_src, rx_dst;
	bool rx_on;
	uint32_t rx_frame_ms;
	uint16_t tx_sid;
	bool tx_on;
} lk;

static m17_net_stats_t st;

// Rings: RX producer the host link task (head), consumer the radio task (tail); TX the other way round
static m17_net_frame_t rx_ring[M17_NET_RX_RING];
static uint8_t rx_head, rx_tail;
static m17_net_frame_t tx_ring[M17_NET_TX_RING];
static uint8_t tx_head, tx_tail;
// SMS slots: full set by the producer once the slot is written, cleared by the consumer once it is read
static m17_net_sms_t sms_out, sms_in;
static uint8_t sms_out_full, sms_in_full;

// Packets are built in the IP gateway's own frame buffer (ipGatewayUdpPayload), sent before anything else uses it
#define OUT()		ipGatewayUdpPayload()
#define OUT_CAP		((size_t)IPGW_UDP_PAYLOAD_MAX)

// --- Any task ---
static void req_put(uint8_t from, uint8_t op, const m17_net_target_t *t)
{
	req_t *r = &req[from < M17_NET_FROM_COUNT ? from : M17_NET_FROM_DEBUG];

	STORE(r->seq, (uint8_t)(r->seq | 1u));
	BARRIER();
	r->op = op;
	if (t != NULL)
		r->t = *t;
	BARRIER();
	STORE(r->seq, (uint8_t)(r->seq + 1u));
}

bool m17_net_link(uint8_t from, const m17_net_target_t *t)
{
	if (!m17_net_target_ok(t))
		return false;
	req_put(from, REQ_LINK, t);
	return true;
}

void m17_net_unlink(uint8_t from)
{
	req_put(from, REQ_UNLINK, NULL);
}

// A reader below the writer's priority, or at it (the debug link, the UI): the writer finishes while it waits. A
// bounded wait all the same: a copy torn by a writer that never ran is still a status, never a crash
void m17_net_status(m17_net_status_t *out)
{
	for (unsigned i = 0; i < 100000u; i++) {
		uint8_t a = LOAD(pub_seq);

		BARRIER();
		memcpy(out, &pub, sizeof *out);
		BARRIER();
		if ((a & 1u) == 0 && a == LOAD(pub_seq))
			return;
	}
}

bool m17_net_status_try(m17_net_status_t *out)
{
	uint8_t a = LOAD(pub_seq);

	BARRIER();
	memcpy(out, &pub, sizeof *out);
	BARRIER();
	return (a & 1u) == 0 && a == LOAD(pub_seq);
}

uint8_t m17_net_state(void)
{
	return LOAD(state_byte);
}

void m17_net_stats(m17_net_stats_t *out)
{
	*out = st;
}

void m17_net_reset_stats(void)
{
	memset(&st, 0, sizeof st);
}

// --- The radio task ---
bool m17_net_talks(void)
{
	return LOAD(talks_byte) != 0;
}

bool m17_net_linked(void)
{
	return LOAD(state_byte) == M17_NET_LINKED;
}

bool m17_net_rx_take(m17_net_frame_t *out)
{
	uint8_t t = rx_tail;

	if (t == LOAD(rx_head))
		return false;
	*out = rx_ring[t & (M17_NET_RX_RING - 1u)];
	STORE(rx_tail, (uint8_t)(t + 1u));
	return true;
}

uint8_t m17_net_rx_count(void)
{
	return (uint8_t)(LOAD(rx_head) - rx_tail);
}

void m17_net_rx_flush(void)
{
	STORE(rx_tail, LOAD(rx_head));
}

bool m17_net_tx_frame(const m17_net_frame_t *f)
{
	uint8_t h = tx_head;

	if (!m17_net_talks() || (uint8_t)(h - LOAD(tx_tail)) >= M17_NET_TX_RING) {
		st.tx_full++;
		return false;
	}
	tx_ring[h & (M17_NET_TX_RING - 1u)] = *f;
	STORE(tx_head, (uint8_t)(h + 1u));
	return true;
}

m17_net_sms_t *m17_net_sms_slot(void)
{
	return m17_net_talks() && !LOAD(sms_out_full) ? &sms_out : NULL;
}

void m17_net_sms_post(void)
{
	STORE(sms_out_full, 1);
}

const m17_net_sms_t *m17_net_sms_peek(void)
{
	return LOAD(sms_in_full) ? &sms_in : NULL;
}

void m17_net_sms_done(void)
{
	STORE(sms_in_full, 0);
}

// --- The host link task ---
static void publish(void)
{
	lk.dirty = false;
	STORE(pub_seq, (uint8_t)(pub_seq + 1u));
	BARRIER();
	pub.state = lk.state;
	pub.why = lk.why;
	pub.rx = lk.rx_on;
	pub.tx = lk.tx_on;
	pub.t = lk.t;
	pub.rx_src = lk.rx_src;
	pub.rx_dst = lk.rx_dst;
	BARRIER();
	STORE(pub_seq, (uint8_t)(pub_seq + 1u));
	STORE(state_byte, lk.state);
	STORE(talks_byte, (uint8_t)(lk.state == M17_NET_LINKED && !lk.t.listen));
}

static void set_state(uint8_t s, uint8_t why)
{
	if (s != M17_NET_LINKED) {
		lk.rx_on = lk.tx_on = false;
	}
	lk.state = s;
	lk.why = why;
	lk.dirty = true;
}

static uint16_t peer_port(const m17_net_target_t *t)
{
	return t->port != 0 ? t->port : (uint16_t)M17_NET_PORT;
}

static bool send_to(const m17_net_target_t *t, const uint8_t *p, size_t n)
{
	if (!ipGatewayUdpSend(t->ip, M17_NET_PORT, peer_port(t), p, (int)n)) {
		st.send_busy++;
		return false;
	}
	st.tx_packets++;
	return true;
}

static void send_conn(void)
{
	uint8_t *out = OUT();
	size_t n = m17_net_put_conn(out, lk.t.listen, lk.t.own, lk.t.module);

	(void)send_to(&lk.t, out, n);	// a busy endpoint: the retry timer sends again
	st.conns++;
	lk.tries++;
	lk.sent_ms = lk.now;
}

static void send_ctl(const m17_net_target_t *t, const char magic[4])
{
	uint8_t *out = OUT();
	size_t n = m17_net_put_ctl(out, magic, m17_net_from(t->own, t->module));

	if (send_to(t, out, n)) {
		if (magic[1] == 'O')
			st.pongs++;	// PONG
		else
			st.discs_tx++;
	}
}

static void connect_now(void)
{
	lk.tries = 0;
	if (!lk.up) {
		// A link asked while the host link is down (or the radio isn't in network mode): it waits for it, and nothing
		// goes out meanwhile (the IP gateway's frame would have nowhere to go)
		set_state(M17_NET_WAIT, M17_NET_WHY_NO_NET);
		lk.retry_ms = lk.now;
		return;
	}
	set_state(M17_NET_CONNECTING, lk.why);
	send_conn();
}

static void unlink_now(void)
{
	lk.tries = 1;
	lk.sent_ms = lk.now;
	send_ctl(&lk.t, "DISC");
	set_state(M17_NET_UNLINKING, M17_NET_WHY_USER);
}

static bool same_target(const m17_net_target_t *a, const m17_net_target_t *b)
{
	return a->ip == b->ip && peer_port(a) == peer_port(b) && a->module == b->module && a->listen == b->listen &&
	       a->own == b->own && strcmp(a->name, b->name) == 0;
}

static void take_one(uint8_t op, const m17_net_target_t *t)
{
	if (op == REQ_UNLINK) {
		if ((lk.state == M17_NET_LINKED || lk.state == M17_NET_CONNECTING) && lk.up) {
			unlink_now();
		} else if (lk.state == M17_NET_OFF) {
			memset(&lk.t, 0, sizeof lk.t);	// not linked: the last target is forgotten
			lk.rx_src = lk.rx_dst = 0;
			set_state(M17_NET_OFF, M17_NET_WHY_NONE);
		} else if (lk.state != M17_NET_UNLINKING) {
			set_state(M17_NET_OFF, M17_NET_WHY_USER);
		}
		return;
	}
	if (op != REQ_LINK || !m17_net_target_ok(t))
		return;
	if ((lk.state == M17_NET_LINKED || lk.state == M17_NET_CONNECTING) && !same_target(&lk.t, t) && lk.up)
		send_ctl(&lk.t, "DISC");	// leave the old one: once, its acknowledgment not waited for
	else if (lk.state == M17_NET_LINKED && same_target(&lk.t, t))
		return;		// linked there already
	lk.t = *t;
	lk.why = M17_NET_WHY_NONE;
	lk.rx_ms = lk.pong_ms = lk.now;
	if (!lk.bound) {
		set_state(M17_NET_OFF, M17_NET_WHY_NO_PORT);
		return;
	}
	connect_now();
}

static void take_requests(void)
{
	for (uint8_t i = 0; i < M17_NET_FROM_COUNT; i++) {
		req_t *r = &req[i];
		uint8_t s = LOAD(r->seq), op;
		m17_net_target_t t;

		if ((s & 1u) != 0 || s == lk.req_seen[i])
			continue;
		BARRIER();
		op = r->op;
		t = r->t;
		BARRIER();
		if (LOAD(r->seq) != s)
			continue;	// written meanwhile: the next poll
		lk.req_seen[i] = s;
		take_one(op, &t);
	}
}

// A stream packet from the reflector into the ring for the radio task
static void stream_in(const uint8_t *p, size_t n)
{
	bool alt;
	m17_net_frame_t *f;
	uint8_t h = rx_head;
	uint16_t sid;

	if (!m17_net_stream_ok(p, n, &alt)) {
		st.bad++;
		return;
	}
	if (alt)
		st.crc_alt++;
	st.rx_frames++;
	sid = (uint16_t)(p[4] << 8 | p[5]);
	if (!lk.rx_on || sid != lk.rx_sid) {
		lk.rx_on = true;
		lk.rx_sid = sid;
		st.rx_streams++;
		lk.rx_src = m17_addr_get(p + 6 + M17_LSF_SRC);
		lk.rx_dst = m17_addr_get(p + 6 + M17_LSF_DST);
		lk.dirty = true;
	}
	lk.rx_frame_ms = lk.now;
	if ((uint8_t)(h - LOAD(rx_tail)) >= M17_NET_RX_RING) {
		st.rx_dropped++;
		return;
	}
	f = &rx_ring[h & (M17_NET_RX_RING - 1u)];
	f->sid = sid;
	memcpy(f->dst, p + 6 + M17_LSF_DST, M17_ADDR_BYTES);
	memcpy(f->src, p + 6 + M17_LSF_SRC, M17_ADDR_BYTES);
	f->type = (uint16_t)(p[6 + M17_LSF_TYPE] << 8 | p[6 + M17_LSF_TYPE + 1]);
	f->fn = (uint16_t)(p[34] << 8 | p[35]);
	memcpy(f->stream, p + 36, M17_STREAM_BYTES);
	STORE(rx_head, (uint8_t)(h + 1u));
	if (f->fn & M17_FN_END) {
		lk.rx_on = false;
		lk.dirty = true;
	}
}

// A packet-mode packet: its LSF and payload checked, then into the SMS slot for the radio task (any data type: the
// radio core counts what it doesn't keep)
static void packet_in(const uint8_t *p, size_t n)
{
	const uint8_t *lsf, *data;
	uint16_t len;

	if (!m17_net_get_packet(p, n, &lsf, &data, &len)) {
		st.bad++;
		return;
	}
	st.pkt_rx++;
	if (LOAD(sms_in_full)) {
		st.rx_dropped++;
		return;
	}
	sms_in.src = m17_addr_get(lsf + M17_LSF_SRC);
	sms_in.dst = m17_addr_get(lsf + M17_LSF_DST);
	if (sms_in.dst == m17_net_module_addr(lk.t.name, lk.t.module))
		sms_in.dst = M17_ADDR_BROADCAST;	// to the module: everyone on it
	sms_in.can = (uint8_t)M17_TYPE_CAN(m17_lsf_type(lsf));
	sms_in.cut = len > M17_NET_SMS_MAX;
	sms_in.len = sms_in.cut ? (uint16_t)M17_NET_SMS_MAX : len;
	memcpy(sms_in.data, data, sms_in.len);
	STORE(sms_in_full, 1);
}

static bool magic_is(const uint8_t *p, const char *m)
{
	return memcmp(p, m, 4) == 0;
}

static void udp_in(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *p, int length)
{
	size_t n = (size_t)length;

	(void)dst;
	(void)dport;
	if (air || length < 4)
		return;
	if (lk.state == M17_NET_OFF || lk.state == M17_NET_WAIT || src != lk.t.ip || sport != peer_port(&lk.t)) {
		st.foreign++;
		return;		// only the reflector we link to, from its port
	}
	st.rx_packets++;
	lk.rx_ms = lk.now;		// anything from it: alive
	if (magic_is(p, "ACKN") && n == M17_NET_ACK_LEN) {
		if (lk.state == M17_NET_CONNECTING) {
			st.ackns++;
			st.links++;
			lk.pong_ms = lk.now;
			set_state(M17_NET_LINKED, M17_NET_WHY_NONE);
		}
	} else if (magic_is(p, "NACK") && n == M17_NET_ACK_LEN) {
		if (lk.state == M17_NET_CONNECTING) {
			st.nacks++;
			set_state(M17_NET_OFF, M17_NET_WHY_REFUSED);
		}
	} else if (magic_is(p, "PING") && n == M17_NET_CTL_LEN) {
		st.pings++;
		lk.pong_due = lk.state == M17_NET_LINKED;
	} else if (magic_is(p, "DISC") && (n == M17_NET_ACK_LEN || n == M17_NET_CTL_LEN)) {
		st.discs_rx++;
		if (lk.state == M17_NET_UNLINKING)
			set_state(M17_NET_OFF, M17_NET_WHY_USER);	// either form ends ours
		else if (n == M17_NET_CTL_LEN)
			lk.disc_due = true;	// the reflector's: it may be going down; link again later
	} else if (magic_is(p, "M17 ")) {
		if (lk.state == M17_NET_LINKED)
			stream_in(p, n);
	} else if (magic_is(p, "M17P")) {
		if (lk.state == M17_NET_LINKED)
			packet_in(p, n);
	} else if (magic_is(p, "ACKN") || magic_is(p, "NACK") || magic_is(p, "PING") || magic_is(p, "DISC")) {
		st.bad++;	// a known MAGIC at a length it doesn't have (a reflector-to-reflector form)
	} else {
		st.unknown++;	// another packet type: ignored, as the sources ask
	}
}

void m17_net_start(void)
{
	lk.bound = ipGatewayUdpBind(M17_NET_PORT, udp_in);
	lk.dirty = true;
}

// Our frames to the reflector, in order; one the endpoint refuses stays for the next poll
static void send_frames(void)
{
	uint8_t lsd[M17_LSD_BYTES];

	for (;;) {
		uint8_t t = tx_tail;
		const m17_net_frame_t *f;

		if (t == LOAD(tx_head))
			return;
		f = &tx_ring[t & (M17_NET_TX_RING - 1u)];
		if (lk.state != M17_NET_LINKED || lk.t.listen) {
			st.tx_dropped++;
		} else {
			if (!lk.tx_on || f->sid != lk.tx_sid) {
				lk.tx_on = true;
				lk.tx_sid = f->sid;
				lk.dirty = true;
				st.tx_streams++;
			}
			m17_addr_put(lsd + M17_LSF_DST, net_dst(m17_addr_get(f->dst), &lk.t));
			memcpy(lsd + M17_LSF_SRC, f->src, M17_ADDR_BYTES);
			lsd[M17_LSF_TYPE] = (uint8_t)(f->type >> 8);
			lsd[M17_LSF_TYPE + 1] = (uint8_t)f->type;
			memset(lsd + M17_LSF_META, 0, M17_META_BYTES);	// no META over the network yet
			m17_net_put_stream(OUT(), f->sid, lsd, f->fn, f->stream);
			if (!send_to(&lk.t, OUT(), M17_NET_STREAM_LEN))
				return;
			st.tx_frames++;
			if (f->fn & M17_FN_END) {
				lk.tx_on = false;
				lk.dirty = true;
			}
		}
		STORE(tx_tail, (uint8_t)(t + 1u));
	}
}

static void send_sms(void)
{
	uint8_t lsf[M17_LSF_BYTES];
	size_t n;

	if (!LOAD(sms_out_full))
		return;
	if (lk.state != M17_NET_LINKED || lk.t.listen || sms_out.len < 2u || sms_out.len > M17_NET_SMS_MAX) {
		STORE(sms_out_full, 0);
		st.tx_dropped++;
		return;
	}
	m17_lsf_make(lsf, net_dst(sms_out.dst, &lk.t), sms_out.src,
		     m17_type(false, 0, M17_ENC_NONE, M17_META_TEXT, sms_out.can, false), NULL);
	n = m17_net_put_packet(OUT(), OUT_CAP, lsf, sms_out.data, sms_out.len);
	if (n == 0) {
		STORE(sms_out_full, 0);
		st.bad++;
		return;
	}
	if (!send_to(&lk.t, OUT(), n))
		return;		// the next poll
	st.pkt_tx++;
	STORE(sms_out_full, 0);
}

void m17_net_poll(uint32_t now_ms, bool up)
{
	lk.now = now_ms;
	lk.up = up;
	take_requests();
	if (!up) {
		if (lk.state == M17_NET_UNLINKING)
			set_state(M17_NET_OFF, M17_NET_WHY_USER);	// no way to say goodbye: the reflector times us out
		else if (lk.state == M17_NET_LINKED || lk.state == M17_NET_CONNECTING)
			set_state(M17_NET_WAIT, M17_NET_WHY_NO_NET);
		if (lk.state == M17_NET_WAIT)
			lk.retry_ms = now_ms;	// link again as soon as the host is back
	} else {
		switch (lk.state) {
		case M17_NET_WAIT:
			if ((int32_t)(now_ms - lk.retry_ms) >= 0)
				connect_now();
			break;
		case M17_NET_CONNECTING:
			if (now_ms - lk.sent_ms < M17_NET_CONN_RETRY_MS)
				break;
			if (lk.tries < M17_NET_CONN_TRIES) {
				send_conn();
			} else {
				set_state(M17_NET_WAIT, M17_NET_WHY_NO_ANSWER);
				lk.retry_ms = now_ms + M17_NET_RETRY_MS;
			}
			break;
		case M17_NET_LINKED:
			if (lk.disc_due) {
				lk.disc_due = false;
				set_state(M17_NET_WAIT, M17_NET_WHY_DISC);	// no acknowledgment needed
				lk.retry_ms = now_ms + M17_NET_RETRY_MS;
				break;
			}
			if (now_ms - lk.rx_ms >= M17_NET_SILENCE_MS) {
				st.losses++;
				set_state(M17_NET_WAIT, M17_NET_WHY_LOST);
				lk.retry_ms = now_ms;	// try at once: the reflector may be back
				break;
			}
			if (lk.pong_due || now_ms - lk.pong_ms >= M17_NET_PONG_MS) {
				lk.pong_due = false;
				lk.pong_ms = now_ms;
				send_ctl(&lk.t, "PONG");
			}
			if (lk.rx_on && now_ms - lk.rx_frame_ms >= 1000u) {
				lk.rx_on = false;	// its last frame never came
				lk.dirty = true;
			}
			send_frames();
			send_sms();
			break;
		case M17_NET_UNLINKING:
			if (now_ms - lk.sent_ms < M17_NET_DISC_RETRY_MS)
				break;
			if (lk.tries < M17_NET_DISC_TRIES) {
				lk.tries++;
				lk.sent_ms = now_ms;
				send_ctl(&lk.t, "DISC");
			} else {
				set_state(M17_NET_OFF, M17_NET_WHY_USER);	// gone without a word: it times us out
			}
			break;
		default:
			break;
		}
	}
	lk.disc_due = lk.disc_due && lk.state == M17_NET_LINKED;
	if (lk.state != M17_NET_LINKED) {
		send_frames();	// nothing goes out unlinked: dropped, counted
		send_sms();
	}
	if (lk.dirty)
		publish();
}

// --- The debug link's m17net section (docs/spec/debuglink.md 3.7) ---
static const char *const STATE_NAMES[] = { "off", "wait", "connecting", "linked", "unlinking" };
static const char *const WHY_NAMES[] = { "none", "user", "refused", "no_answer", "lost", "disc", "no_net", "no_port" };
_Static_assert(sizeof STATE_NAMES / sizeof STATE_NAMES[0] == M17_NET_UNLINKING + 1u &&
	       sizeof WHY_NAMES / sizeof WHY_NAMES[0] == M17_NET_WHY_NO_PORT + 1u, "a name for each state and reason");
_Static_assert(offsetof(m17_net_stats_t, rx_packets) == 0 && offsetof(m17_net_stats_t, send_busy) == 24u * 4u &&
	       sizeof(m17_net_stats_t) == 25u * 4u, "m17_net_stats_t: the words the view walks, in this order");

// An address as one word: a callsign (a space inside as '_'), "-" none, "ALL" broadcast, else 12 hex digits
static void addr_word(uint64_t a, char out[M17_ADDR_TEXT])
{
	if (a == 0) {
		memcpy(out, "-", 2);
		return;
	}
	if (m17_addr_text(a, out) == M17_ADDR_KIND_BROADCAST) {
		memcpy(out, "ALL", 4);
		return;
	}
	for (char *p = out; *p != '\0'; p++)
		*p = *p == ' ' ? '_' : *p;
}

void m17_net_view(dl_text_t *t)
{
	static const char COUNTERS[] = "rx_packets\0tx_packets\0conns\0ackns\0nacks\0pings\0pongs\0discs_tx\0discs_rx\0"
		"links\0losses\0rx_streams\0rx_frames\0tx_streams\0tx_frames\0pkt_rx\0pkt_tx\0bad\0crc_alt\0unknown\0"
		"foreign\0rx_dropped\0tx_full\0tx_dropped\0send_busy\0";
	m17_net_status_t s;
	m17_net_stats_t c;
	char src[M17_ADDR_TEXT];
	const uint32_t *w = (const uint32_t *)(const void *)&c;
	const char *name = COUNTERS;

	m17_net_status(&s);
	m17_net_stats(&c);
	addr_word(s.rx_src, src);
	dl_text(t, "m17net.built=1\nm17net.state=%s\nm17net.why=%s\nm17net.reflector=%s\nm17net.module=%c\n"
		"m17net.listen=%u\nm17net.ip=%u.%u.%u.%u\nm17net.port=%u\nm17net.rx=%u\nm17net.tx=%u\nm17net.rx_src=%s\n",
		STATE_NAMES[s.state < M17_NET_UNLINKING + 1u ? s.state : 0], WHY_NAMES[s.why <= M17_NET_WHY_NO_PORT ? s.why : 0],
		s.t.name[0] != '\0' ? s.t.name : "-", s.t.module != 0 ? s.t.module : '-', s.t.listen,
		(unsigned)(s.t.ip >> 24), (unsigned)((s.t.ip >> 16) & 0xFFu), (unsigned)((s.t.ip >> 8) & 0xFFu),
		(unsigned)(s.t.ip & 0xFFu), (unsigned)peer_port(&s.t), s.rx, s.tx, src);
	for (unsigned i = 0; *name != '\0'; i++, name += strlen(name) + 1)
		dl_text(t, "m17net.%s=%u\n", name, (unsigned)w[i]);
}

#endif	// CONFIG_NET_M17
