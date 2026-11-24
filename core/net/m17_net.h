// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 reflector client (CONFIG_NET_M17, docs/spec/m17-ip.md): M17 over UDP on the radio's network address, through
// the host (USB network or PPP), to one module of one reflector. CONN (or LSTN, listen only) with our callsign and the
// module, PONG on every PING and on its own timer, the link lost after 30 s of silence, DISC repeated until the
// reflector acknowledges it; stream packets ("M17 ") and packet mode ("M17P", SMS) both ways.
//
// Three parts, each its own task's:
//  - The packet codecs (m17_net_put_* / m17_net_get_*): pure functions, no state (the host tests' known answers).
//  - The link (m17_net_start, m17_net_poll, the UDP handler): the host link task's, from core/net/net.c (and from
//    core/hostlink/hostlink.c in serial mode, up false). No RTOS call: the time is the poll's (now_ms), the sockets the
//    IP gateway's (ipGatewayUdpBind, ipGatewayUdpSend). Nothing is sent in a poll whose up is false.
//  - The hand-overs: to the radio task (radio_m17.c), single producer / single consumer rings of stream frames each
//    way and one SMS slot each way; to any task (the UI, the debug link) a link request slot and a copy of the state.
//
// The gaps of the sources (docs/spec/m17-ip.md 8) are decided once each, in m17_net.c, next to "GAP n".
#ifndef RFOUNDRY_M17_NET_H
#define RFOUNDRY_M17_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "m17/m17.h"
#include "debuglink/dl_fmt.h"

#define M17_NET_PORT		17000u	// a reflector's port unless the target says another; also ours (the source)
#define M17_NET_NAME_MAX	7u	// a reflector's designation: "M17-XYZ" (the module goes in the 9th place)

// Packet sizes (docs/spec/m17-ip.md 3, 4)
#define M17_NET_STREAM_LEN	54u	// "M17 ": MAGIC, SID, LSD, FN, payload, CRC
#define M17_NET_CONN_LEN	11u	// CONN, LSTN: MAGIC, from, module
#define M17_NET_CTL_LEN		10u	// PING, PONG, DISC: MAGIC, from
#define M17_NET_ACK_LEN		4u	// ACKN, NACK, the DISC acknowledgment: MAGIC alone
#define M17_NET_PKT_MIN		38u	// "M17P": MAGIC, the LSF with its CRC, type, at least one data byte, CRC
#define M17_NET_PKT_PAYLOAD_MAX	825u	// type, data and CRC

// Timing (docs/spec/m17-ip.md 5)
#define M17_NET_CONN_RETRY_MS	2000u	// CONN again when no ACKN or NACK came ...
#define M17_NET_CONN_TRIES	5u	// ... this many times, then wait M17_NET_RETRY_MS
#define M17_NET_RETRY_MS	30000u	// before linking again: no answer, or the reflector disconnected us
#define M17_NET_PONG_MS		5000u	// our own PONG at least this often (the reflector's PINGs answered too)
#define M17_NET_SILENCE_MS	30000u	// nothing from the reflector this long: the link is lost
#define M17_NET_DISC_RETRY_MS	1000u	// DISC again until the 4-byte DISC comes ...
#define M17_NET_DISC_TRIES	5u	// ... at most this many times

// The link's state (m17_net_status_t.state)
enum {
	M17_NET_OFF,		// not linked, and not trying
	M17_NET_WAIT,		// to link again later: no network yet, no answer, the link lost or ended by the reflector
	M17_NET_CONNECTING,	// CONN or LSTN sent, no ACKN yet
	M17_NET_LINKED,
	M17_NET_UNLINKING,	// DISC sent, its acknowledgment not yet in
};
// Why the link isn't up, or why the last one ended (m17_net_status_t.why)
enum {
	M17_NET_WHY_NONE,
	M17_NET_WHY_USER,	// unlinked
	M17_NET_WHY_REFUSED,	// NACK: no such module, refused, or listen only on an encrypted module
	M17_NET_WHY_NO_ANSWER,	// no ACKN or NACK after M17_NET_CONN_TRIES CONNs
	M17_NET_WHY_LOST,	// nothing from the reflector for M17_NET_SILENCE_MS
	M17_NET_WHY_DISC,	// the reflector disconnected us (it may be going down for a while)
	M17_NET_WHY_NO_NET,	// the host link is down
	M17_NET_WHY_NO_PORT,	// the IP gateway's port table is full
};

// What to link to
typedef struct {
	uint32_t ip;		// IPv4, host byte order
	uint16_t port;		// 0: M17_NET_PORT
	char name[M17_NET_NAME_MAX + 1];	// the designation, NUL-terminated
	char module;		// 'A'..'Z'
	bool listen;		// listen only (LSTN): PTT stays on the air
	uint64_t own;		// our callsign as an address (CONN's from)
} m17_net_target_t;

typedef struct {
	uint8_t state;		// M17_NET_*
	uint8_t why;		// M17_NET_WHY_*
	bool rx;		// a reflector stream is coming in
	bool tx;		// one of ours is going out
	m17_net_target_t t;	// the target (OFF: the last one, if any)
	uint64_t rx_src;	// the stream coming in (or the last one): its SRC ...
	uint64_t rx_dst;	// ... and DST
} m17_net_status_t;

typedef struct {
	uint32_t rx_packets, tx_packets;	// UDP datagrams from and to the reflector
	uint32_t conns, ackns, nacks;		// CONN or LSTN sent; their answers
	uint32_t pings, pongs;			// PINGs received; PONGs sent
	uint32_t discs_tx, discs_rx;		// DISC sent; DISC received (either form)
	uint32_t links, losses;			// links made; links lost to silence
	uint32_t rx_streams, rx_frames;		// streams and stream packets received
	uint32_t tx_streams, tx_frames;		// streams and stream packets sent
	uint32_t pkt_rx, pkt_tx;		// M17P received (any type), sent
	uint32_t bad;				// a length, CRC or field check failed (dropped)
	uint32_t crc_alt;			// stream packets whose CRC held only without the MAGIC (GAP 1)
	uint32_t unknown;			// MAGICs not known (ignored)
	uint32_t foreign;			// datagrams from another address or port (dropped)
	uint32_t rx_dropped;			// stream frames or SMS the radio task had no room for
	uint32_t tx_full;			// ours the radio task couldn't hand over: the ring full, or not linked
	uint32_t tx_dropped;			// ours dropped here: the link went down before they went out
	uint32_t send_busy;			// sends that found the USB IN endpoint busy (tried again)
} m17_net_stats_t;

// One stream frame between the link and the radio task, either way
typedef struct {
	uint16_t sid;		// the stream
	uint16_t fn;		// as on air: M17_FN_END on the last
	uint16_t type;		// the LSD's TYPE
	uint8_t src[M17_ADDR_BYTES];
	uint8_t dst[M17_ADDR_BYTES];
	uint8_t stream[M17_STREAM_BYTES];
} m17_net_frame_t;

#define M17_NET_RX_RING		8u	// frames from the reflector waiting for the radio task (320 ms), power of two
#define M17_NET_TX_RING		4u	// ours waiting for the host link task (160 ms), power of two
#define M17_NET_SMS_MAX		160u	// a packet's type and data, either way (an SMS: type, text, its NUL)

// An SMS (or any packet) through the reflector: data is the packet's type and user data, without the CRC
typedef struct {
	uint64_t src, dst;
	uint8_t can;
	bool cut;		// longer than M17_NET_SMS_MAX: data holds its start
	uint16_t len;
	uint8_t data[M17_NET_SMS_MAX];
} m17_net_sms_t;

// --- Packet codecs (pure; out holds at least the packet's length) ---
// CONN or LSTN: 11 bytes
size_t m17_net_put_conn(uint8_t *out, bool listen, uint64_t own, char module);
// PONG or DISC (or any 10-byte control packet): magic's 4 characters and our callsign
size_t m17_net_put_ctl(uint8_t *out, const char magic[4], uint64_t own);
// A stream packet from its fields: 54 bytes, the CRC computed (GAP 1)
size_t m17_net_put_stream(uint8_t *out, uint16_t sid, const uint8_t lsd[M17_LSD_BYTES], uint16_t fn,
			  const uint8_t payload[M17_STREAM_BYTES]);
// A stream packet's CRC as this client computes it (GAP 1), and whether p (54 bytes) holds; *alt: it holds only by the
// other reading (the MAGIC left out)
uint16_t m17_net_stream_crc(const uint8_t *p);
bool m17_net_stream_ok(const uint8_t *p, size_t n, bool *alt);
// A packet-mode packet: MAGIC, lsf (with its CRC, m17_lsf_make), data (type and user data, len >= 2) and its CRC.
// 0 when it doesn't fit cap or the data is too short or too long
size_t m17_net_put_packet(uint8_t *out, size_t cap, const uint8_t lsf[M17_LSF_BYTES], const uint8_t *data,
			  uint16_t len);
// Its checks (length, LSF CRC, payload CRC): *lsf and *data point into p, *len is the type and data without the CRC
bool m17_net_get_packet(const uint8_t *p, size_t n, const uint8_t **lsf, const uint8_t **data, uint16_t *len);
// The from callsign every control packet carries (GAP 2)
uint64_t m17_net_from(uint64_t own, char module);
// A reflector module as a destination: the designation left-justified in 8 places, the module letter 9th (GAP 5)
uint64_t m17_net_module_addr(const char *name, char module);
// "M17-XYZ C" (a designation, spaces, the module letter) into its parts; false when it isn't one
bool m17_net_parse_name(const char *text, char name[M17_NET_NAME_MAX + 1], char *module);
// A target holds together (an address, a designation, a module letter, our callsign)
bool m17_net_target_ok(const m17_net_target_t *t);

// --- Any task ---
// Who asks: each has a request slot of its own, so two tasks never write one slot at once (no compare-and-swap on
// every CPU). One poll takes the UI's before the debug link's: the later one wins
enum { M17_NET_FROM_UI, M17_NET_FROM_DEBUG, M17_NET_FROM_COUNT };
// Link to t (another link first ends: one DISC to the old reflector). False: t isn't valid. Unlink: DISC; while not
// linked, the last target is forgotten
bool m17_net_link(uint8_t from, const m17_net_target_t *t);
void m17_net_unlink(uint8_t from);
// The state, a consistent copy (a task below or at the host link task's priority: it may wait for a write to end)
void m17_net_status(m17_net_status_t *out);
// The same without waiting, for a task above the host link task (the radio task): false when a write was under way
bool m17_net_status_try(m17_net_status_t *out);
uint8_t m17_net_state(void);		// one byte: M17_NET_*
void m17_net_stats(m17_net_stats_t *out);
void m17_net_reset_stats(void);

// --- The radio task (radio_m17.c) ---
// Linked to talk (not listen only): PTT on an M17 channel goes to the reflector
bool m17_net_talks(void);
// Linked at all: the reflector's streams and SMS come in
bool m17_net_linked(void);
// The next frame from the reflector; false: none
bool m17_net_rx_take(m17_net_frame_t *out);
uint8_t m17_net_rx_count(void);
void m17_net_rx_flush(void);
// One frame of ours (fn M17_FN_END on the last); false: no room, or not linked (counted in tx_dropped)
bool m17_net_tx_frame(const m17_net_frame_t *f);
// An SMS to the reflector, in place: the slot to fill (NULL: one is still waiting, or not linked to talk), then post
// it (len 2 .. M17_NET_SMS_MAX, else it is dropped and counted bad)
m17_net_sms_t *m17_net_sms_slot(void);
void m17_net_sms_post(void);
// An SMS from the reflector, in place (NULL: none), then done with it. A message to the linked module ("M17-XYZ C")
// comes with dst broadcast: to everyone on it
const m17_net_sms_t *m17_net_sms_peek(void);
void m17_net_sms_done(void);

// --- The host link task (core/net/net.c) ---
void m17_net_start(void);
void m17_net_poll(uint32_t now_ms, bool up);
// The debug link's m17net section: the state, the target, the counters (key=value lines; any task)
void m17_net_view(dl_text_t *t);

#endif
