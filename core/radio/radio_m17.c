// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on a channel, the radio core's side (radio_m17.h; docs/spec/m17.md, the radio core): the engine and the voice
// path on the lent memory, the channel's parameters, the signature work in the radio task, packets into the SMS inbox,
// the heard list, the BER test, the benches, and the view. Linked to a reflector (CONFIG_NET_M17), the network path:
// PTT's stream and the SMS go to the reflector instead of the air, its streams play when the air is quiet.
//
// Secrets. A traffic key is read straight into cfg_tmp (static, wiped once the engine has its copy), a signing key
// into the voice path's lent scratch (sig_work_t), never onto a task stack; each read sits inside crash_secret_enter /
// _leave, as do the nonce draw and the curve work, so a crash record made meanwhile keeps no register or stack word.
// The engine brackets its own: the key's copy into it, and each frame's encryption and decryption in the provider's
// task (m17_engine.c, m17_tx.c).
#include "config.h"
#if CONFIG_M17	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "radio/radio_m17.h"
#include "radio/radio_m17_mem.h"
#include "radio/radio_air_mem.h"
#include "radio/radio_air_modes.h"
#include "radio/radio_bands.h"
#include "radio/scan.h"
#include "m17/m17_codeplug.h"
#if CONFIG_M17_PACKET
#include "m17/m17_packet.h"
#endif
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_modes.h"
#include "codeplug/codeplug_tag.h"
#include "crash/crash_secret.h"
#include "crypto/crypto_wipe.h"
#include "position/position.h"
#include "position/position_utc.h"
#if CONFIG_M17_SIG
#include "crypto/p256.h"
#endif
#if CONFIG_ENTROPY
#include "crypto/entropy.h"
#endif
#if CONFIG_HEARD_LIST
#include "radio/radio_heard.h"
#endif
#if CONFIG_NET_M17
#include "net/m17_net.h"
#endif

#define BARRIER()		__atomic_signal_fence(__ATOMIC_SEQ_CST)
// The poll's helpers keep their locals (a packet's bytes, a position, the call's view) in frames of their own: inlined,
// they would all sit under every path through the poll on the radio task's stack (make stack-check)
#define OWN_FRAME		__attribute__((noinline))
#define UTC_FRESH_MS		60000u	// a time source older than this is no clock (timestamps, freshness)
#define SIG_WINDOW_S		300u	// a signed call's timestamp further than this from a trusted now: old (m17.md 7.4)
#define KEY_CHECK_IDLE_MS	2000u	// the key check waits for the channel this long idle
#define GNSS_EVERY_MS		5000u	// our position to the engine at most this often, across transmissions
#define SMS_SLOT_MS		40u	// CSMA slot (m17.md 1.4)
#define CHECK_WAIT_MS		500u	// a signed call's check that never came after its end is lost
#define SIG_TAIL_DEFAULT	4u	// sig_tail before any signature or key check was timed
#define REPLAY_N		8u
#define REPLAY_BYTES		8u
// The time-out an encrypted or signed stream can't pass: its last data frame is M17_FN_ENC_LAST (m17_engine.h)
#define ENC_LIMIT_S		((uint16_t)(((uint32_t)M17_FN_ENC_LAST + 1u) * M17_FRAME_MS / 1000u))

// The view's flags that describe the call heard: kept from one call's end to the next call
#define CALL_FLAGS	(RADIO_M17_ENC | RADIO_M17_LATE | RADIO_M17_LSF | RADIO_M17_1600 | RADIO_M17_TEXT | \
			 RADIO_M17_GNSS | RADIO_M17_PACKET | RADIO_M17_SIGNED | RADIO_M17_TO_US | RADIO_M17_DATA)

enum { AUTO_NONE, AUTO_PACKET, AUTO_BERT };

static const radio_m17_settings_t defaults = {
	.can = 0, .tx_can = 0, .squelch = M17_SQUELCH_CAN, .mode1600 = false,
	.dst = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
};
static radio_m17_settings_t set;
static m17_mem_t *mem;			// while running
// The counters of the engines that ran before this one (the running engine's are added as they are read)
static radio_m17_stats_t stats;
// The engine's configuration as it is put together (begin, apply): a traffic key goes straight in here and is wiped
// once the engine has its copy
static m17_engine_cfg_t cfg_tmp;
// The radio-wide record in force (our callsign, the call text the engine's cfg.text points into, sign, position)
static m17_wide_t wide;

// The channel as params() read it, and the radio core's state the row passes in
static struct {
	uint64_t own;		// our address, 0 none
	bool own_bad;		// the record holds an address that is not a standard one
	uint8_t enc_want;	// the stored channel's transmit encryption as written, built or not (M17_ENC_*)
	uint8_t system;		// its M17 system, CODEPLUG_SYSTEM_NONE
	bool idle;		// the radio core: receiving, no scan or dual watch visit, no hotspot (row_poll)
	uint16_t tot_s;		// the radio core's TX time-out
} ch_now;

// One run's bookkeeping (begin zeroes it)
static struct {
	uint32_t ticks;		// polls
	uint8_t auto_kind;	// AUTO_*: what auto_tx just asked to send ...
	uint32_t auto_tick;	// ... at this poll (the radio core keys in the same tick, or the ask is stale)
	uint8_t tx_kind;	// M17_TX_* of the transmission under way, 0xFF none
	bool tx_aborted;
	uint8_t prep_deny;	// why tx_prepare refused (no nonce, or a stream's refusal with an SMS waiting): tx_deny
				// says it once
	bool masked;		// a retune: the reception the engine still holds is the old frequency's ...
	uint32_t masked_calls;	// ... until it ends or another begins
	uint32_t calls;		// the engine's rx_calls the view last took as a call
	bool active;		// that call was being received at the last poll
	uint8_t text_seq;	// the engine's text_seq the details last took
	uint64_t orig;		// the originator the station-key question was asked for ...
	bool known;		// ... and its answer (m17_station_known)
	uint32_t end_ms;	// when the call ended (0: running, or none)
	uint8_t sig;		// RADIO_M17_SIG_* of the call
	uint32_t idle_since;	// the key check's idle time starts
	uint32_t noise;		// the engine's noise mix last stirred in
	bool dirty;		// the details need a rebuild
	bool rx_on;		// the engine held a reception at the last poll: the channel is busy for an SMS
} run;

// Our position for GNSS META. The engine sends the latest position it was given in each transmission's LSF and, when a
// new one comes, at the next 5 s boundary in the stream; it knows one transmission. So across transmissions the radio
// core keeps the 5 s: air_ms is the latest a position may have gone out, and a transmission that starts sooner sends
// none (its cfg.gnss off)
static uint32_t gnss_ms;		// when our position last went to an engine
static bool gnss_given;			// ... at all, since start-up
static uint32_t gnss_air_ms;		// the latest a position may have gone out ...
static bool gnss_air;			// ... at all
static uint32_t gnss_try_ms;		// when the position was last asked for (once a second without a fix)

// --- Any task's copies: a sequence count around each write (odd while the radio task writes) ---
static radio_m17_details_t det;
static volatile uint8_t det_seq;
static bool det_have;

static void seq_begin(volatile uint8_t *s)
{
	*s = (uint8_t)(*s + 1u);
	BARRIER();
}

static void seq_end(volatile uint8_t *s)
{
	BARRIER();
	*s = (uint8_t)(*s + 1u);
}

// A reader that preempts nothing (the radio task writes, and runs above every reader) copies until the count held
static void seq_copy(volatile const uint8_t *s, void *out, const void *in, size_t n)
{
	uint8_t a;

	do {
		a = *s;
		BARRIER();
		memcpy(out, in, n);
		BARRIER();
	} while ((a & 1u) != 0 || a != *s);
}

// --- Signatures ---
#if CONFIG_M17_SIG
// The scratch the voice path lends for the curve work: P-256's working memory and every value next to it, the signing
// key among them, so none of it is on the radio task's stack
typedef struct {
	p256_scratch_t p;
	uint8_t d[P256_BYTES];
	uint8_t q[P256_PUB_BYTES];
	uint8_t qc[P256_PUB_C_BYTES];
	uint8_t h[P256_BYTES];
	uint8_t rs[P256_SIG_BYTES];
} sig_work_t;
_Static_assert(sizeof(sig_work_t) <= C2_SCRATCH_BYTES, "the curve work fits the codec scratch");

static radio_m17_key_t key;
static volatile uint8_t key_seq;
static bool key_usable;		// key.present with a valid d (the cached read)
static uint32_t key_cp, key_kv;	// the codeplug's serials the cached read is of ...
static bool key_read;		// ... read at all
static bool key_due;		// the key check is due (a codeplug change since the last)
static uint32_t tail_us;	// the latest real signature's time, or the key check's kG time (sig_tail follows it)

// The verification after a signed stream (one key a tick)
static struct {
	bool on;
	uint8_t next;		// the next stored key of the signer
	uint8_t tried;		// keys verified with
	uint32_t calls;		// the call it is for (run.calls)
	m17_sig_check_t chk;
} vj;
static uint8_t replay[REPLAY_N][REPLAY_BYTES];
static uint8_t replay_at;

static sig_work_t *borrow(void)
{
	size_t n = 0;
	void *p = m17_voice_scratch_borrow(&n);

	if (p != NULL && n < sizeof(sig_work_t)) {
		m17_voice_scratch_return();
		p = NULL;
	}
	return p;
}

static void give_back(sig_work_t *w)
{
	crypto_wipe(w, sizeof *w);	// p256 wiped its secrets; the rest goes too before the codec has it again
	m17_voice_scratch_return();
}

// sig_tail = ceil((t + 90 ms) / 40 ms) - 2, 1..8 (docs/spec/m17.md, the radio core), 4 before any time was taken
static uint8_t sig_tail(void)
{
	if (tail_us == 0)
		return SIG_TAIL_DEFAULT;
	uint32_t f = (tail_us + 90000u + 39999u) / 40000u;
	f = f > 2u ? f - 2u : 1u;
	return (uint8_t)(f < 1u ? 1u : f > 8u ? 8u : f);
}

// The signing key of the channel's system (or the first M17 system with one), read again after a codeplug change: is
// there one, is d valid, its stored Q. d is read into buf (a static the caller wipes)
static void key_load(uint8_t ch_system, uint8_t *buf)
{
	uint32_t cp = codeplug_serial(), kv = codeplug_kv_serial();
	uint8_t sys = m17_signing_system(ch_system);
	uint8_t qc[P256_PUB_C_BYTES];
	bool has_q = false, ok = false;

	if (key_read && cp == key_cp && kv == key_kv && sys == key.system)
		return;
	if (sys != CODEPLUG_SYSTEM_NONE) {
		crash_secret_enter();
		ok = m17_system_signing_key(sys, buf, qc, &has_q);
		crypto_wipe(buf, P256_BYTES);
		crash_secret_leave();
	}
	seq_begin(&key_seq);
	memset(&key, 0, sizeof key);
	key.present = ok;
	key.system = sys;
	if (ok && has_q && p256_pub_decompress(qc, key.q))
		m17_key_fp(qc, key.fp);
	key.sign_us = tail_us;
	seq_end(&key_seq);
	key_usable = ok;
	key_read = true;
	key_cp = cp;
	key_kv = kv;
	key_due = ok;		// checked once per change, while idle (key_check)
}
#endif

// --- The channel's parameters ---
static uint8_t voice_kind(bool mode1600)
{
	return mode1600 ? (uint8_t)M17_DATA_VOICE_DATA : (uint8_t)M17_DATA_VOICE;
}

static c2_mode_t c2_mode(uint8_t voice)
{
	return voice == M17_DATA_VOICE_DATA ? C2_MODE_1600 : C2_MODE_3200;
}

// The engine's configuration for channel ch at number (0: the VFO) into cfg_tmp: a live stored M17 channel's block
// (docs/spec/m17-codeplug.md 2), else the settings; the radio-wide record either way
static void params(const channel_t *ch, uint16_t number)
{
	m17_engine_cfg_t *c = &cfg_tmp;
	m17_conv_cfg_t cc;
	uint16_t key_id = 0;

	crypto_wipe(c, sizeof *c);
	m17_wide_load(&wide);
	ch_now.own = m17_own_addr(&ch_now.own_bad);
	ch_now.enc_want = M17_ENC_NONE;
	ch_now.system = CODEPLUG_SYSTEM_NONE;
	if (number != 0 && m17_conv_load(number, ch, &cc)) {
		uint8_t b[M17_CP_CONV_LEN + CODEPLUG_TAG_HEAD_MAX];

		c->rx_can = cc.conv.can;
		c->tx_can = cc.conv.tx_can;
		c->squelch = cc.conv.squelch;
		c->voice = voice_kind(cc.conv.mode1600);
		c->dst = cc.conv.dst;
		c->tx_inhibit = !cc.tx_allowed;
		c->tx_enc = cc.conv.tx_enc;
		key_id = cc.conv.key_id;
		ch_now.system = cc.system;
		// What the block asks for, not what this build reads it as: an encryption it lacks refuses PTT
		if (codeplug_tag_params(number, ch, b, M17_CP_CONV_LEN) >= 10 &&
		    (b[9] == M17_ENC_SCRAMBLER || b[9] == M17_ENC_AES))
			ch_now.enc_want = b[9];
	} else {
		c->rx_can = set.can;
		c->tx_can = set.tx_can;
		c->squelch = set.squelch;
		c->voice = voice_kind(set.mode1600);
		c->dst = m17_addr_get(set.dst);
	}
	if (c->squelch == M17_SQUELCH_ANY)
		c->rx_can = M17_CAN_ANY;
	c->src = ch_now.own;
	c->text = wide.text;
	c->text_len = wide.text_len;
	c->gnss = wide.gnss;
	c->silence = m17_voice_silence(c2_mode(c->voice));
#if CONFIG_M17_SIG
	key_load(ch_now.system, c->key);
	c->sign = wide.sign && key_usable && !key.damaged;
	c->sig_tail = sig_tail();
#endif
	if (c->tx_enc != M17_ENC_NONE && ch_now.system != CODEPLUG_SYSTEM_NONE) {
		crash_secret_enter();
		c->key_len = m17_system_key(ch_now.system, c->tx_enc, key_id, c->key);
		crash_secret_leave();
	}
}

// The settings in force as settings (the engine's configuration)
static void cfg_settings(const m17_engine_cfg_t *c, radio_m17_settings_t *out)
{
	out->can = c->rx_can == M17_CAN_ANY ? c->tx_can : c->rx_can;
	out->tx_can = c->tx_can;
	out->squelch = c->squelch;
	out->mode1600 = c->voice == M17_DATA_VOICE_DATA;
	m17_addr_put(out->dst, c->dst);
}

// --- Voice: the engine's hooks (provider's task) to the voice path ---
#if CONFIG_NET_M17
// The voice path's receive queue has one producer; with a reflector linked there are two: the engine's hooks here
// (provider's task) and the network path (net_push, radio task). Each says it is on, then looks at the other (both
// sequentially consistent: at least one of two that overlap sees the other). An air call holds the queue from its start
// to its end (air_rx); a network frame is handed over only while it is clear, and an air call's hook that finds a
// hand-over under way keeps its start for its next frame (air_pending: the mode + 1) or drops that one frame. Its end
// needs no wait: once an air call's start is in, a hand-over under way began after air_rx was set, saw it, and writes
// nothing (no read-modify-write either side: the C-SKY core has none to spare)
#define SC_STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define SC_LOAD(x)	__atomic_load_n(&(x), __ATOMIC_SEQ_CST)
static uint8_t air_rx, net_pushing, air_pending;
static uint8_t air_calls;	// air calls started (the network path starts its own call again after one)
#endif

static void h_start(const m17_call_t *c, void *ctx)
{
	c2_mode_t mode = c2_mode((uint8_t)M17_TYPE_DATA(c->type));

	(void)ctx;
#if CONFIG_NET_M17
	SC_STORE(air_rx, 1);
	SC_STORE(air_calls, (uint8_t)(air_calls + 1u));
	if (SC_LOAD(net_pushing)) {
		air_pending = (uint8_t)(mode + 1);
		return;
	}
	air_pending = 0;
#endif
	m17_voice_rx_start(mode);
}

static void h_voice(const uint8_t stream[M17_STREAM_BYTES], uint8_t errs, void *ctx)
{
	(void)ctx;
#if CONFIG_NET_M17
	SC_STORE(air_rx, 1);
	if (SC_LOAD(net_pushing))
		return;		// a network frame is going in: this one is lost
	if (air_pending != 0) {
		m17_voice_rx_start((c2_mode_t)(air_pending - 1u));
		air_pending = 0;
	}
#endif
	m17_voice_rx_frame(stream, errs);
}

static void h_end(void *ctx)
{
	(void)ctx;
#if CONFIG_NET_M17
	if (air_pending != 0)
		air_pending = 0;		// it never started: nothing to end
	else
		m17_voice_rx_end();		// a hand-over under way now saw air_rx and writes nothing
	SC_STORE(air_rx, 0);
#else
	m17_voice_rx_end();
#endif
}

static bool h_tx(uint8_t stream[M17_STREAM_BYTES], void *ctx)
{
	(void)ctx;
	return m17_voice_tx_frame(stream);
}

static const m17_engine_hooks_t hooks = { h_start, NULL, h_voice, h_end, h_tx };

#if CONFIG_NET_M17
// --- The network path's state (docs/spec/m17-ip.md 7): linked to a reflector, PTT on the channel sends to it and its
// streams play while the air is quiet. Radio task only; the link itself is the host link task's (m17_net.h) ---
#define NET_PRIME		3u	// frames in hand before a reflector stream starts to play (120 ms for the jitter) ...
#define NET_PRIME_MS		200u	// ... or fewer once the first has waited this long
#define NET_AHEAD		5u	// more in hand than this: one more each tick until it is back (a burst after a gap)
#define NET_GAP_MS		500u	// no frame this long: the stream is over (its last frame lost)
enum { NTX_OFF, NTX_ON, NTX_DONE };
static struct {
	uint8_t tx;		// NTX_*: the transmission tx_prepare sent to the reflector
	uint8_t tx_mode;	// c2_mode_t
	uint16_t tx_sid, tx_fn, last_sid;
	uint16_t tx_type;
	uint8_t tx_src[M17_ADDR_BYTES], tx_dst[M17_ADDR_BYTES];
	uint32_t rng;
	bool rx;		// a reflector stream plays ...
	bool skip;		// ... or is dropped (not voice, or encrypted)
	uint16_t rx_sid;	// that stream ...
	uint16_t rx_type;	// ... its TYPE
	uint8_t rx_src[M17_ADDR_BYTES], rx_dst[M17_ADDR_BYTES];	// ... its caller and what it called (the heard list)
	uint8_t rx_mode;	// c2_mode_t
	uint8_t air_calls;	// air_calls when it last started its call in the voice path
	uint32_t wait_ms;	// the first frame of the next stream waits to prime since (0: none waits)
	uint32_t rel_ms;	// the next frame's release
	uint32_t last_ms;	// the latest frame taken
	uint32_t end_ms;	// when the reflector call the view shows ended (0: running)
	bool view;		// the view shows a reflector call (the engine's call is no longer the view's)
} net;
#define NET_VIEW()	(net.view)
#else
#define NET_VIEW()	false
#endif

// --- SMS ---
#if CONFIG_M17_PACKET
static struct {
	bool staged;		// another task copied a text in (radio_m17_sms_stage)
	bool waiting;		// queued, not on air yet
	bool inflight;		// on air (tx_prepare took it)
	uint8_t tries;		// transmissions cut short
	uint8_t result, seq;	// RADIO_M17_SMS_* of the latest, and a count of outcomes
	uint8_t to[M17_ADDR_BYTES];
	uint64_t dst;
	uint16_t len;
	uint32_t queued_ms, slot_ms;
	uint32_t asked_ms;	// auto_tx's last look at the channel
	uint32_t rng;
	uint8_t data[1u + RADIO_M17_SMS_MAX + 1u];	// type 5, the text, its zero byte (the engine's until AIR_TX_DONE)
} sms;
static radio_m17_sms_t inbox[RADIO_M17_SMS_INBOX];
static uint8_t inbox_n;
#endif
static void (*lock_fn)(void), (*unlock_fn)(void);

void radio_m17_set_lock(void (*lock)(void), void (*unlock)(void))
{
	lock_fn = lock;
	unlock_fn = unlock;
}

static void lock(void)
{
	if (lock_fn != NULL)
		lock_fn();
}

static void unlock(void)
{
	if (unlock_fn != NULL)
		unlock_fn();
}

#if CONFIG_M17_PACKET

// At most max bytes of UTF-8, never inside a character: the length kept
static uint16_t utf8_cut(const uint8_t *s, uint16_t n, uint16_t max)
{
	if (n <= max)
		return n;
	while (max > 0 && (s[max] & 0xC0u) == 0x80u)
		max--;
	return max;
}

// A message into the inbox, newest first; the oldest goes when it is full
static void inbox_add(const uint8_t peer[M17_ADDR_BYTES], bool sent, const uint8_t *text, uint16_t n, bool cut)
{
	radio_m17_sms_t *m = &inbox[0];
	uint16_t k = utf8_cut(text, n, RADIO_M17_SMS_MAX);

	lock();
	memmove(&inbox[1], &inbox[0], (RADIO_M17_SMS_INBOX - 1u) * sizeof inbox[0]);
	if (inbox_n < RADIO_M17_SMS_INBOX)
		inbox_n++;
	memcpy(m->peer, peer, M17_ADDR_BYTES);
	m->unread = !sent;
	m->sent = sent;
	m->cut = cut || k < n;
	m->at_ms = hal_millis();
	memcpy(m->text, text, k);
	m->text[k] = 0;
	unlock();
}

static void sms_done(uint8_t result)
{
	sms.waiting = sms.inflight = false;
	sms.result = result;
	sms.seq++;
	if (result == RADIO_M17_SMS_BUSY)
		stats.sms_busy++;
}

// The text into the send's buffer: type 5, the text cut on a character boundary, its zero byte
static void sms_copy(uint64_t dst, const char *utf8)
{
	uint16_t n = 0;

	while (utf8[n] != 0 && n <= RADIO_M17_SMS_MAX)
		n++;
	n = utf8_cut((const uint8_t *)utf8, n, RADIO_M17_SMS_MAX);
	sms.data[0] = M17_PKT_SMS;
	memcpy(&sms.data[1], utf8, n);
	sms.data[1u + n] = 0;
	sms.len = (uint16_t)(n + 2u);
	sms.dst = dst;
}

// The channel is set to encrypt (scrambler or AES, built or not, key or not): no SMS goes from it. Packet mode has no
// encryption (m17.md 8), and a channel set to encrypt never sends in the clear (m17.md 17.3)
static bool sms_clear_refused(void)
{
	return ch_now.enc_want != M17_ENC_NONE || (mem != NULL && mem->eng.cfg.tx_enc != M17_ENC_NONE);
}

bool radio_m17_sms_stage(uint64_t dst, const char *utf8)
{
	bool ok;

	if (utf8 == NULL)
		return false;
	lock();
	ok = mem != NULL && !sms.staged && !sms.waiting && !sms.inflight && !sms_clear_refused();
	if (ok) {
		sms_copy(dst, utf8);
		sms.staged = true;
	}
	unlock();
	return ok;
}

bool radio_m17_sms_send(uint64_t dst, const char *utf8)
{
	bool staged = utf8 == NULL;

	if (staged) {
		lock();
		staged = sms.staged;
		sms.staged = false;
		dst = sms.dst;
		unlock();
		if (!staged)
			return false;
	}
	if (mem == NULL || sms.waiting || sms.inflight || ch_now.own == 0 || dst == 0 || sms_clear_refused()) {
		if (staged)
			sms_done(RADIO_M17_SMS_BUSY);	// the other task learns it was not sent
		return false;
	}
	if (!staged)
		sms_copy(dst, utf8);
	m17_addr_put(sms.to, dst);
	sms.waiting = true;
	sms.tries = 0;
	sms.queued_ms = sms.slot_ms = hal_millis();
	sms.rng ^= sms.queued_ms * 2654435761u + run.noise;
	return true;
}

uint8_t radio_m17_sms_result(uint8_t *seq, uint8_t to[6])
{
	if (seq != NULL)
		*seq = sms.seq;
	if (to != NULL)
		memcpy(to, sms.to, M17_ADDR_BYTES);
	return sms.result;
}

uint8_t radio_m17_sms_count(void)
{
	return inbox_n;
}

uint8_t radio_m17_sms_unread(void)
{
	uint8_t u = 0;

	lock();
	for (uint8_t i = 0; i < inbox_n; i++)
		u = (uint8_t)(u + inbox[i].unread);
	unlock();
	return u;
}

bool radio_m17_sms_get(uint8_t i, radio_m17_sms_t *out)
{
	bool ok;

	lock();
	ok = i < inbox_n;
	if (ok)
		*out = inbox[i];
	unlock();
	return ok;
}

void radio_m17_sms_read(uint8_t i)
{
	lock();
	if (i < inbox_n)
		inbox[i].unread = false;
	unlock();
}

bool radio_m17_sms_delete(uint8_t i)
{
	bool ok;

	lock();
	ok = i < inbox_n;
	if (ok) {
		memmove(&inbox[i], &inbox[i + 1u], (size_t)(inbox_n - i - 1u) * sizeof inbox[0]);
		inbox_n--;
		memset(&inbox[inbox_n], 0, sizeof inbox[0]);
	}
	unlock();
	return ok;
}

// An SMS heard is ours when it is addressed to us, to everyone, or to what the channel calls (a group, a reflector),
// and is not our own sent back (a repeater's): the engine's addressed squelch, whatever squelch the channel has. One
// between two other stations is not kept (DMR's data service does the same)
static bool sms_ours(const m17_packet_info_t *info, uint64_t talk)
{
	if (ch_now.own != 0 && info->src == ch_now.own)
		return false;
	return (ch_now.own != 0 && info->dst == ch_now.own) || info->dst == M17_ADDR_BROADCAST ||
	       (talk != 0 && info->dst == talk);
}

// A packet the engine received, taken every tick: an SMS for us to the inbox (others counted in sms_dropped), any
// packet into the details
static OWN_FRAME void packets(m17_engine_t *e, const m17_engine_view_t *ev)
{
	m17_packet_info_t info;
	uint8_t buf[M17_ENGINE_PKT_KEEP];
	uint8_t peer[M17_ADDR_BYTES], tb[4];

	memset(&info, 0, sizeof info);
	(void)m17_engine_packet_take(e, &info, buf, sizeof buf);
	if (info.type == M17_PKT_SMS && !sms_ours(&info, e->cfg.dst)) {
		stats.sms_dropped++;
	} else if (info.type == M17_PKT_SMS) {
		uint16_t k = 0;

		while (k < info.len && buf[k] != 0)
			k++;
		m17_addr_put(peer, info.src);
		inbox_add(peer, false, buf, k, info.cut);
		stats.sms_rx++;
	}
	seq_begin(&det_seq);
	memset(&det, 0, sizeof det);
	m17_addr_put(det.src, info.src);
	m17_addr_put(det.dst, info.dst);
	memcpy(det.orig, det.src, M17_ADDR_BYTES);
	det.can = (uint8_t)M17_TYPE_CAN(ev->rx.type);
	det.played = true;
	det.packet = true;
	det.pkt_type = info.type;
	det.pkt_len = (uint16_t)(info.len + m17_pkt_type_put(info.type, tb));	// the data type's bytes too
	det.rssi_dbm = ev->rx.rssi_dbm;
	det.end_ms = hal_millis();
	seq_end(&det_seq);
	det_have = true;
}
#endif

// --- BERT ---
#if CONFIG_M17_BERT
static struct {
	bool mode, tx;
	uint32_t frames, bits, errs, locks, losses;	// the engine's counters at the last reset
	uint32_t last_frames, last_ms;			// BERT frames seen, and when they last moved
	uint32_t tx_start_ms, tx_syms0;
} bert;
static radio_m17_bert_t bert_pub;
static volatile uint8_t bert_seq;
#endif

// --- Start, stop ---
void radio_m17_init(const hal_audio_t *audio)
{
	set = defaults;
	mem = NULL;
	radio_air_mem_put(RADIO_MODE_M17);	// nothing runs from here: a block held before is free
	m17_voice_init(audio, hal_micros);
}

void radio_m17_settings(radio_m17_settings_t *out)
{
	*out = set;
}

void radio_m17_set_settings(const radio_m17_settings_t *s)
{
	set = *s;
	set.can &= M17_CAN_MAX;
	set.tx_can &= M17_CAN_MAX;
	if (set.squelch > M17_SQUELCH_ADDRESSED)
		set.squelch = M17_SQUELCH_CAN;
	if (m17_addr_get(set.dst) == 0)
		memset(set.dst, 0xFF, sizeof set.dst);	// zero is never a destination: broadcast
}

bool radio_m17_in_force(radio_m17_settings_t *out)
{
	m17_mem_t *m = mem;

	if (m == NULL)
		return false;
	cfg_settings(&m->eng.cfg, out);
	return true;
}

uint32_t radio_m17_fit(void)
{
	return board.air != NULL ? m17_engine_fit(board.air) : 0;
}

bool radio_m17_fits(void)
{
	return (radio_m17_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_m17_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_M17 || !radio_m17_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_M17;
	ch->bw = RADIO_BW_12K5;
	return true;
}

bool radio_m17_receives(const channel_t *ch, uint16_t number)
{
	(void)ch;
	(void)number;
	return (radio_m17_fit() & AIR_FIT_RX) != 0;
}

static void mem_put(void)
{
	mem = NULL;
	radio_air_mem_put(RADIO_MODE_M17);
}

bool radio_m17_begin(const channel_t *ch, uint16_t number)
{
	m17_mem_t *m;

	radio_m17_end();
	stats.begins++;
	m = radio_air_mem_get(RADIO_MODE_M17, sizeof(m17_mem_t));
	if (m == NULL || board.air == NULL || m17_engine_fit(board.air) == 0) {
		if (m != NULL)
			radio_air_mem_put(RADIO_MODE_M17);
		stats.begin_fails++;
		return false;
	}
	if (radio_air_mem_lent(RADIO_MODE_M17))
		stats.mem_lent++;
	params(ch, number);
	memset(&run, 0, sizeof run);
#if CONFIG_NET_M17
	{
		uint16_t last = net.last_sid;
		uint32_t rng = net.rng;

		memset(&net, 0, sizeof net);	// what the network gave meanwhile is stale
		net.last_sid = last;
		net.rng = rng;
		m17_net_rx_flush();
		air_rx = air_pending = net_pushing = 0;	// no provider runs yet
	}
#endif
	run.tx_kind = 0xFFu;
	run.idle_since = hal_millis();	// the key check's 2 s start here
	m17_voice_begin(&m->voice);	// before the engine: its first call may come at once
	bool ok = m17_engine_begin(&m->eng, board.air, &cfg_tmp, &hooks, NULL);
	crypto_wipe(cfg_tmp.key, sizeof cfg_tmp.key);
	if (!ok) {
		m17_voice_end();
		radio_air_mem_put(RADIO_MODE_M17);
		stats.begin_fails++;
		return false;
	}
	mem = m;
#if CONFIG_M17_SIG
	vj.on = false;
#endif
#if CONFIG_M17_BERT
	bert.last_frames = 0;
	bert.frames = bert.bits = bert.errs = bert.locks = bert.losses = 0;
#endif
	return true;
}

// An engine's counters (they start at 0 with it) into s, added or taken away (unsigned: a reset while one runs takes
// its counts so far away, and the sum read later is what came since)
static void engine_counts(radio_m17_stats_t *s, const m17_engine_t *e, bool add)
{
	const m17_engine_stats_t *c = &e->stats;
	const air_session_stats_t *a = &e->session.stats;
	uint32_t k = add ? 1u : (uint32_t)-1;

#define ADD(f, v)	s->f += k * (v)
	ADD(rx_calls, e->view.rx_calls); ADD(tx_calls, e->view.tx_calls); ADD(tx_filler, e->view.tx_filler);
	ADD(frames, c->frames); ADD(lsf_ok, c->lsf_ok); ADD(lsf_bad, c->lsf_bad); ADD(lich_ok, c->lich_ok);
	ADD(lich_bad, c->lich_bad); ADD(stream, c->stream); ADD(packet, c->packet); ADD(bert, c->bert);
	ADD(fec_bits, c->fec_bits); ADD(can_rejects, c->can_rejects); ADD(eot, c->eot); ADD(head_rejects, c->head_rejects);
	ADD(enc_skipped, c->enc_skipped); ADD(expect_miss, c->expect_miss); ADD(fn_limit, c->fn_limit);
	ADD(pkt_rx, c->pkt_rx); ADD(pkt_bad, c->pkt_bad); ADD(pkt_tx, c->pkt_tx); ADD(pkt_dropped, c->pkt_dropped);
	ADD(sig_late, c->sig_late);
	ADD(rx_chunks, a->rx_chunks); ADD(tx_underruns, a->tx_underruns); ADD(tx_syms, a->tx_syms);
#undef ADD
}

void radio_m17_end(void)
{
	if (mem == NULL)
		return;
	m17_engine_end(&mem->eng);	// the provider calls nothing after its close; every secret in it is wiped
	m17_voice_scratch_return();	// (none is lent between ticks; a request not answered is withdrawn)
	m17_voice_end();
	// Its counters into ours and the memory back (a lending vocoder rebuilds its state at its next call), under the
	// lock: a reader of the counters adds the running engine's
	lock();
	engine_counts(&stats, &mem->eng, true);
	mem_put();
	unlock();
#if CONFIG_M17_SIG
	if (vj.on) {
		vj.on = false;
		if (run.sig == RADIO_M17_SIG_CHECKING)
			run.sig = RADIO_M17_SIG_LOST;	// not checked: the channel was left first
	}
#endif
#if CONFIG_M17_PACKET
	if (sms.waiting || sms.inflight || sms.staged) {
		sms.staged = false;
		sms_done(RADIO_M17_SMS_BUSY);	// not sent: the channel was left
	}
#endif
#if CONFIG_M17_BERT
	bert.tx = false;
#endif
#if CONFIG_NET_M17
	net.tx = NTX_OFF;
	net.rx = net.skip = false;
#endif
}

bool radio_m17_running(void)
{
	return mem != NULL;
}

void radio_m17_retuned(void)
{
	m17_engine_view_t ev;

	if (mem == NULL)
		return;
	m17_engine_retuned(&mem->eng);
	// The engine ends a reception it holds at the next lock; on a silent frequency none comes. Until then what it
	// holds is the old frequency's and shows as nothing
	m17_engine_view(&mem->eng, &ev);
	run.masked = ev.rx_active;
	run.masked_calls = ev.rx_calls;
}

void radio_m17_apply(const channel_t *ch, uint16_t number)
{
	if (mem == NULL || m17_engine_tx_state(&mem->eng) != AIR_TX_IDLE)
		return;
	params(ch, number);
	m17_engine_set(&mem->eng, &cfg_tmp);
	crypto_wipe(cfg_tmp.key, sizeof cfg_tmp.key);
}

// --- The receive side's work, each tick ---
#if CONFIG_M17_SIG
// What a signed call's check concluded: the view, the details, the counters, the heard list
static void sig_verdict(uint8_t v, uint8_t fresh);
static uint8_t heard_sig;

// The check the engine handed over is in vj.chk
static void sig_job_start(void)
{
	if (vj.chk.state != M17_SIG_PENDING) {
		stats.sig_incomplete++;
		sig_verdict(vj.chk.state == M17_SIG_LATE ? RADIO_M17_SIG_LATE : RADIO_M17_SIG_LOST, RADIO_M17_FRESH_UNKNOWN);
		return;
	}
	vj.on = true;
	vj.next = vj.tried = 0;
	vj.calls = run.calls;
	if (run.sig == RADIO_M17_SIG_RUNNING)
		run.sig = RADIO_M17_SIG_CHECKING;
}

static void sig_valid(void)
{
	uint8_t v = vj.chk.signer == vj.chk.src ? RADIO_M17_SIG_VALID : RADIO_M17_SIG_SIGNER_DIFFERS;
	uint8_t fresh = RADIO_M17_FRESH_UNKNOWN;	// a timestamp of 0: the sender has no clock, never "old"
	uint32_t unix_s;
	bool rep = false;

	stats.sig_valid++;
	if (vj.chk.ts != 0 && position_utc_now(hal_millis(), UTC_FRESH_MS, &unix_s, NULL)) {
		uint32_t now = utc_since_2020(unix_s);
		uint32_t d = now > vj.chk.ts ? now - vj.chk.ts : vj.chk.ts - now;

		fresh = d > SIG_WINDOW_S ? RADIO_M17_FRESH_NO : RADIO_M17_FRESH_YES;
	}
	for (uint8_t i = 0; i < REPLAY_N; i++)
		rep = rep || memcmp(replay[i], vj.chk.hash, REPLAY_BYTES) == 0;
	if (!rep) {
		memcpy(replay[replay_at], vj.chk.hash, REPLAY_BYTES);
		replay_at = (uint8_t)((replay_at + 1u) % REPLAY_N);
	}
	if (v == RADIO_M17_SIG_VALID && fresh == RADIO_M17_FRESH_NO)
		v = RADIO_M17_SIG_OLD;
	else if (v == RADIO_M17_SIG_VALID && rep)
		v = RADIO_M17_SIG_REPLAY;
	sig_verdict(v, fresh);
}

// One step of the verification: the signer's next stored key (the channel's system first), verified. At most
// RADIO_M17_VERIFY_KEYS_MAX keys; up to about 200 ms in the radio task, which yields before its next tick
static OWN_FRAME void sig_job_step(void)
{
	sig_work_t *w;

	if (vj.calls != run.calls) {
		vj.on = false;		// another call came meanwhile: this one's details are gone
		return;
	}
	w = borrow();
	if (w == NULL)
		return;			// the codec still plays: the next tick
	if (!m17_station_key(ch_now.system, vj.chk.signer, vj.next, w->qc)) {
		stats.sig_nokey += vj.tried == 0;
		stats.sig_invalid += vj.tried != 0;
		sig_verdict(vj.tried == 0 ? RADIO_M17_SIG_NO_KEY : RADIO_M17_SIG_INVALID, RADIO_M17_FRESH_UNKNOWN);
	} else if (vj.next >= RADIO_M17_VERIFY_KEYS_MAX) {
		stats.sig_keys_capped++;
		stats.sig_invalid++;
		sig_verdict(RADIO_M17_SIG_INVALID, RADIO_M17_FRESH_UNKNOWN);
	} else {
		uint32_t t0 = hal_micros();
		bool ok = p256_pub_decompress(w->qc, w->q) && p256_verify(w->q, vj.chk.hash, vj.chk.rs, &w->p);

		stats.verify_us = hal_micros() - t0;
		vj.next++;
		vj.tried++;
		if (ok)
			sig_valid();
	}
	give_back(w);
}

// Signing at PTT release (the engine's sign-ahead tail): d into the lent scratch, signed, wiped
static OWN_FRAME bool sign_step(m17_engine_t *e)
{
	uint8_t h[M17_HASH_BYTES];
	sig_work_t *w;
	bool has_q, ok;

	if (!m17_engine_sig_want(e, h))
		return false;
	// The tail is silence: the encoder stops (the FN limit starts the tail with PTT still held), else the voice
	// path never lends its scratch and the tail carries zeros
	m17_voice_tx(false, C2_MODE_3200);
	w = borrow();
	if (w == NULL)
		return true;		// the callback answers at its next block: the next tick
	memcpy(w->h, h, sizeof h);
	crash_secret_enter();
	ok = !key.damaged && m17_system_signing_key(key.system, w->d, NULL, &has_q);
	if (ok) {
		uint32_t t0 = hal_micros();
		ok = p256_sign(w->d, w->h, w->rs, &w->p);
		stats.sign_us = hal_micros() - t0;
	}
	crypto_wipe(w->d, sizeof w->d);
	crash_secret_leave();
	m17_engine_sig_give(e, ok ? w->rs : NULL);
	if (ok) {
		stats.sig_signed++;
		tail_us = stats.sign_us;
		seq_begin(&key_seq);
		key.sign_us = stats.sign_us;
		seq_end(&key_seq);
	}
	give_back(w);
	return true;
}

// The key check (docs/spec/m17.md, the radio core): d * G against the stored Q, once per codeplug change, while the
// channel has been idle a while (it holds the radio task up to about 200 ms)
static OWN_FRAME void key_check(const m17_engine_view_t *ev, uint32_t now)
{
	sig_work_t *w;
	bool has_q = false, ok;

	if (!ch_now.idle || ev->rx_active || ev->tx != AIR_TX_IDLE || vj.on) {
		run.idle_since = now;
		return;
	}
	if (!key_due || now - run.idle_since < KEY_CHECK_IDLE_MS)
		return;
	w = borrow();
	if (w == NULL)
		return;
	crash_secret_enter();
	ok = m17_system_signing_key(key.system, w->d, w->qc, &has_q);
	uint32_t t0 = hal_micros();
	ok = ok && p256_pub_from_priv(w->d, w->q, &w->p);
	uint32_t dt = hal_micros() - t0;
	crypto_wipe(w->d, sizeof w->d);
	crash_secret_leave();
	key_due = false;
	stats.key_checks++;
	seq_begin(&key_seq);
	key.checked = true;
	key.damaged = false;
	if (ok) {
		// The stored Q decompressed into rs (no secret there), against the one d gives
		if (has_q && (!p256_pub_decompress(w->qc, w->rs) || memcmp(w->rs, w->q, P256_PUB_BYTES) != 0))
			key.damaged = true;
		memcpy(key.q, w->q, sizeof key.q);
		p256_pub_compress(w->q, w->qc);
		m17_key_fp(w->qc, key.fp);
		if (tail_us == 0)
			tail_us = dt;
		if (key.sign_us == 0)
			key.sign_us = dt;
	} else {
		key.present = false;
	}
	seq_end(&key_seq);
	key_usable = ok;
	if (key.damaged)
		stats.key_damaged++;
	give_back(w);
}
#endif

// The details' call fields from the engine's view of it
static OWN_FRAME void det_fill(const m17_engine_t *e, const m17_engine_view_t *ev, const radio_m17_view_t *n)
{
	const m17_call_t *c = &ev->rx;

	seq_begin(&det_seq);
	memcpy(det.src, n->src, M17_ADDR_BYTES);
	memcpy(det.dst, n->dst, M17_ADDR_BYTES);
	memcpy(det.orig, n->orig, M17_ADDR_BYTES);
	memcpy(det.via, n->via, M17_ADDR_BYTES);
	det.can = n->can;
	det.voice = (uint8_t)M17_TYPE_DATA(c->type);
	det.enc = n->enc;
	det.played = det.played || ev->rx_played;
	if (ev->text_seq != run.text_seq) {
		run.text_seq = ev->text_seq;
		m17_engine_text(e, det.text);
	}
	det.gnss = (c->flags & M17_CALL_GNSS) != 0;
	if (det.gnss) {
		m17_gnss_to_fix(&c->gnss, &det.lat_e7, &det.lon_e7, &det.alt_dm, &det.speed_kmh10);
		det.alt_ok = (c->gnss.valid & M17_GNSS_VALID_ALT) != 0;
		det.vel_ok = (c->gnss.valid & M17_GNSS_VALID_VEL) != 0;
		det.bearing_deg = c->gnss.bearing;
		det.station = c->gnss.station;
	}
	det.sig = run.sig;
	det.rssi_dbm = c->rssi_dbm;
	det.end_ms = run.end_ms;
	seq_end(&det_seq);
	det_have = true;
}

#if CONFIG_M17_SIG
static void sig_verdict(uint8_t v, uint8_t fresh)
{
	vj.on = false;
	run.sig = v;
	heard_sig = v;
	seq_begin(&det_seq);
	det.sig = v;
	det.sig_fresh = fresh;
	m17_addr_put(det.signer, vj.chk.signer);
	m17_addr_put(det.sig_dst, vj.chk.dst);
	det.sig_ts = vj.chk.ts;
	seq_end(&det_seq);
	run.dirty = true;
}
#endif

#if CONFIG_HEARD_LIST
// c: the call's fields (its position), NULL when the engine holds another call already
static OWN_FRAME void heard_add(const radio_m17_view_t *n, const m17_call_t *c)
{
	radio_heard_t h;

	memset(&h, 0, sizeof h);
	h.mode = RADIO_MODE_M17;
	h.flags = RADIO_HEARD_ADDR;
	if (run.sig == RADIO_M17_SIG_VALID)
		h.flags |= RADIO_HEARD_SIG_OK;
	else if (run.sig == RADIO_M17_SIG_INVALID || run.sig == RADIO_M17_SIG_SIGNER_DIFFERS)
		h.flags |= RADIO_HEARD_SIG_BAD;
	if (n->flags & RADIO_M17_ENC)
		h.flags |= RADIO_HEARD_ENC;
	if (n->flags & RADIO_M17_PACKET)
		h.flags |= RADIO_HEARD_DATA;
	if (c != NULL && (c->flags & M17_CALL_GNSS)) {
		int32_t alt;
		uint16_t sp;

		h.flags |= RADIO_HEARD_GNSS;
		m17_gnss_to_fix(&c->gnss, &h.lat_e7, &h.lon_e7, &alt, &sp);
	}
	memcpy(h.addr, n->orig, sizeof h.addr);
	memcpy(h.to, n->dst, sizeof h.to);
	h.rssi_dbm = n->rssi_dbm;
	if (m17_addr_get(h.addr) != 0)
		radio_heard_add(&h);
}
#endif

// What the screen shows of the parameters: those in force while the engine runs, else the settings
static void settings_view(radio_m17_view_t *n)
{
	const m17_engine_cfg_t *c = mem != NULL ? &mem->eng.cfg : NULL;
	radio_m17_settings_t s = set;

	if (c != NULL)
		cfg_settings(c, &s);
	n->rx_can = c != NULL ? c->rx_can : s.squelch == M17_SQUELCH_ANY ? (uint8_t)M17_CAN_ANY : s.can;
	n->tx_can = s.tx_can;
	memcpy(n->talk, s.dst, sizeof n->talk);
	m17_addr_put(n->own, ch_now.own);
	n->fit = (uint16_t)radio_m17_fit();
	if (s.mode1600)
		n->flags |= RADIO_M17_TX_1600;
	if (ch_now.own == 0)
		n->flags |= ch_now.own_bad ? RADIO_M17_CALL_BAD : RADIO_M17_NO_CALL;
	n->tot_s = ch_now.tot_s;
	if (c != NULL && (c->tx_enc != M17_ENC_NONE || c->sign) && (n->tot_s == 0 || n->tot_s > ENC_LIMIT_S))
		n->tot_s = ENC_LIMIT_S;
	if (c != NULL && c->tx_enc != M17_ENC_NONE) {
		uint8_t sub = c->tx_enc == M17_ENC_AES ? (uint8_t)(c->key_len == 32 ? 2 : c->key_len == 24 ? 1 : 0) :
			      (uint8_t)(c->key_len ? c->key_len - 1u : 0u);
		n->flags |= RADIO_M17_TX_ENC;
		n->tx_enc = (uint8_t)(c->tx_enc << 4 | sub);
	} else {
		n->tx_enc = 0;
	}
	if (c != NULL && c->sign)
		n->flags |= RADIO_M17_TX_SIGN;
	if (det_have)
		n->flags |= RADIO_M17_HAS_DETAILS;
#if CONFIG_M17_BERT
	if (bert.mode)
		n->flags |= RADIO_M17_BERT_MODE;
#endif
#if CONFIG_M17_PACKET
	if (sms.waiting && !sms.inflight)
		n->flags |= RADIO_M17_SMS_WAIT;
#endif
}

// The call heard into the view: who calls whom, what it is (kept after it ends)
static OWN_FRAME void call_view(radio_m17_view_t *n, const m17_engine_view_t *ev, bool active)
{
	const m17_call_t *c = &ev->rx;
	uint16_t t = c->type;
	uint64_t orig = (c->flags & M17_CALL_EXTCALL) ? c->ext1 : c->src;
	uint64_t via = (c->flags & M17_CALL_EXTCALL) && c->ext2 != 0 ? c->ext2 : c->src != orig ? c->src : 0;
	uint8_t enc = (uint8_t)M17_TYPE_ENC(t);

	// A packet's TYPE has no encryption and no signed stream: bit 11 is reserved there, and a receiver ignores what
	// a sender put in either (m17.md 3.1). Only a stream is read as encrypted or signed, here and in the signature
	// state below
	bool stream = (c->flags & (M17_CALL_TYPE | M17_CALL_PACKET)) == M17_CALL_TYPE;
	bool signed_stream = stream && (t & M17_TYPE_SIGNED);

	if (!stream)
		enc = M17_ENC_NONE;
	n->flags &= ~(uint32_t)CALL_FLAGS;
	if (enc != M17_ENC_NONE)
		n->flags |= RADIO_M17_ENC;
	if (c->flags & M17_CALL_LATE)
		n->flags |= RADIO_M17_LATE;
	if (c->flags & M17_CALL_LSF)
		n->flags |= RADIO_M17_LSF;
	if (c->flags & M17_CALL_TEXT)
		n->flags |= RADIO_M17_TEXT;
	if (c->flags & M17_CALL_GNSS)
		n->flags |= RADIO_M17_GNSS;
	if (c->flags & M17_CALL_PACKET)
		n->flags |= RADIO_M17_PACKET;
	if (stream) {
		if (M17_TYPE_DATA(t) == M17_DATA_VOICE_DATA)
			n->flags |= RADIO_M17_1600;
		if (M17_TYPE_DATA(t) == M17_DATA_DATA)
			n->flags |= RADIO_M17_DATA;
	}
	if (signed_stream)
		n->flags |= RADIO_M17_SIGNED;
	if (c->dst != 0 && c->dst == ch_now.own)
		n->flags |= RADIO_M17_TO_US;
	n->can = (uint8_t)M17_TYPE_CAN(t);
	n->enc = enc != M17_ENC_NONE ? (uint8_t)(enc << 4 | M17_TYPE_SUB(t)) : 0u;
	n->why = active ? ev->rx_why : n->why;
	m17_addr_put(n->src, c->src);
	m17_addr_put(n->dst, c->dst);
	m17_addr_put(n->orig, orig);
	m17_addr_put(n->via, via);
	n->rssi_dbm = c->rssi_dbm;
	// The unsigned call's marker: is a key on file for its originator (asked once per originator)
	if (orig != run.orig) {
		run.orig = orig;
		run.known = orig != 0 && m17_station_known(orig);
	}
	if (!signed_stream)
		run.sig = run.known ? RADIO_M17_SIG_UNSIGNED_KNOWN : RADIO_M17_SIG_NONE;
	else if (run.sig < RADIO_M17_SIG_RUNNING)
		run.sig = RADIO_M17_SIG_RUNNING;
	// At its end the check is due; a build without signatures has nothing to check it with (no key)
	if (!active && run.sig == RADIO_M17_SIG_RUNNING)
		run.sig = CONFIG_M17_SIG ? RADIO_M17_SIG_CHECKING : RADIO_M17_SIG_NO_KEY;
	n->sig = run.sig;
}

#if CONFIG_M17_BERT
static OWN_FRAME void bert_poll(const m17_engine_view_t *ev, bool active, uint32_t now, radio_m17_view_t *n)
{
	radio_m17_bert_t b;

	if (ev->bert_frames != bert.last_frames) {
		if (ev->bert_frames < bert.last_frames)
			bert.frames = bert.bits = bert.errs = bert.locks = bert.losses = 0;	// a new run started over
		bert.last_frames = ev->bert_frames;
		bert.last_ms = now != 0 ? now : 1u;
	}
	bool rx = active && ev->rx.flags == 0 && bert.last_ms != 0 && now - bert.last_ms < 200u;
	memset(&b, 0, sizeof b);
	b.frames = ev->bert_frames - bert.frames;
	b.bits = ev->bert_bits - bert.bits;
	b.errs = ev->bert_errs - bert.errs;
	b.locks = ev->bert_locks - bert.locks;
	b.losses = ev->bert_losses - bert.losses;
	b.rssi_dbm = ev->rx.rssi_dbm;
	if (bert.last_ms == 0 || now - bert.last_ms > 1000u)
		b.state = RADIO_M17_BERT_NO_SIGNAL;
	else if (ev->bert_state == M17_BERT_COUNT)
		b.state = RADIO_M17_BERT_LOCKED;
	else
		b.state = b.losses != 0 ? RADIO_M17_BERT_LOST : RADIO_M17_BERT_SEARCHING;
	if (run.tx_kind == M17_TX_BERT) {
		b.tx_ms = now - bert.tx_start_ms;
		b.tx_frames = (mem->eng.session.stats.tx_syms - bert.tx_syms0) / M17_FRAME_SYMS;
		rx = true;
	} else {
		b.tx_frames = bert_pub.tx_frames;
	}
	if (rx)
		n->flags |= RADIO_M17_BERT;
	if (memcmp(&b, &bert_pub, sizeof b) != 0) {
		seq_begin(&bert_seq);
		bert_pub = b;
		seq_end(&bert_seq);
	}
}
#endif

// Our position for GNSS META, at most once every 5 s whatever the transmissions (the engine knows only its own)
// Whether a position may go out now: none yet, or the latest one 5 s ago
static bool gnss_window(uint32_t now)
{
	return !gnss_air || (int32_t)(now - gnss_air_ms) >= (int32_t)GNSS_EVERY_MS;
}

static OWN_FRAME void gnss_poll(m17_engine_t *e, uint32_t now)
{
	position_fix_t f;
	uint32_t err_m = 0;
	m17_gnss_t g;
	bool tx = m17_engine_tx_state(e) != AIR_TX_IDLE;

	if (!wide.gnss || (gnss_given && now - gnss_ms < GNSS_EVERY_MS) || now - gnss_try_ms < 1000u)
		return;
	// During a transmission a new position goes out at the stream's next 5 s boundary at the latest
	if (tx && (!e->cfg.gnss || !gnss_window(now)))
		return;
	gnss_try_ms = now;
	if (position_report(&f, &err_m, now) == POSITION_SRC_NONE)
		return;
	m17_gnss_from_fix(&g, f.lat, f.lon, f.alt_m, f.alt_m != POSITION_ALT_NONE, f.speed_kmh, f.course,
			  f.speed_kmh != 0xFFFFu && f.course != 0xFFFFu, err_m, wide.station);
	m17_engine_gnss(e, &g);
	gnss_ms = now;
	gnss_given = true;
	if (tx) {
		gnss_air_ms = now + GNSS_EVERY_MS + M17_FRAME_MS;	// (21 superframes: 5.04 s)
		gnss_air = true;
	}
}

#if CONFIG_NET_M17
// --- The network path's work, each tick (docs/spec/m17-ip.md 7) ---

// One frame of ours to the reflector (the ring full or the link gone: counted there)
static void net_tx_send(const uint8_t stream[M17_STREAM_BYTES], uint16_t fn)
{
	m17_net_frame_t f;

	f.sid = net.tx_sid;
	f.fn = fn;
	f.type = net.tx_type;
	memcpy(f.src, net.tx_src, M17_ADDR_BYTES);
	memcpy(f.dst, net.tx_dst, M17_ADDR_BYTES);
	memcpy(f.stream, stream, M17_STREAM_BYTES);
	(void)m17_net_tx_frame(&f);
}

// The encoder's frames, as they come (one every 40 ms; the engine takes none: it sends nothing now)
static void net_tx_pump(void)
{
	uint8_t st[M17_STREAM_BYTES];

	while (net.tx == NTX_ON && m17_voice_tx_frame(st)) {
		net_tx_send(st, net.tx_fn);
		net.tx_fn = (uint16_t)((net.tx_fn + 1u) & M17_FN_MASK);
	}
}

// PTT released, or the time-out: what the encoder has, then a silent last frame (FN with its end bit)
static void net_tx_end(void)
{
	if (net.tx != NTX_ON)
		return;
	net_tx_pump();
	m17_voice_tx(false, C2_MODE_3200);
	net_tx_send(m17_voice_silence((c2_mode_t)net.tx_mode), (uint16_t)(net.tx_fn | M17_FN_END));
	net.tx = NTX_DONE;	// the radio core ends the TX state at the next tick (tx_state)
}

// A reflector stream the voice path plays: voice (3200 or 1600), in the clear
static bool net_voice(uint16_t type)
{
	return (type & M17_TYPE_STREAM) != 0 && M17_TYPE_ENC(type) == M17_ENC_NONE &&
	       (M17_TYPE_DATA(type) == M17_DATA_VOICE || M17_TYPE_DATA(type) == M17_DATA_VOICE_DATA);
}

// A hand-over to the voice path's queue (air_rx above): a call's start, a frame, an end. False: an air call holds it
static bool net_push(const uint8_t *stream, bool start, bool end)
{
	bool ok;

	SC_STORE(net_pushing, 1);
	ok = SC_LOAD(air_rx) == 0;
	if (ok) {
		uint8_t calls = SC_LOAD(air_calls);

		if (start || calls != net.air_calls) {
			net.air_calls = calls;	// (an air call came between our frames: ours starts again)
			m17_voice_rx_start((c2_mode_t)net.rx_mode);
		}
		if (stream != NULL)
			m17_voice_rx_frame(stream, 0);	// no FEC over IP: never "bad"
		if (end)
			m17_voice_rx_end();
	}
	SC_STORE(net_pushing, 0);
	return ok;
}

// The details of a reflector call (Call Details): who and what, no META over the network
static void net_det(const radio_m17_view_t *n, uint16_t type)
{
	seq_begin(&det_seq);
	memset(&det, 0, sizeof det);
	memcpy(det.src, n->src, M17_ADDR_BYTES);
	memcpy(det.dst, n->dst, M17_ADDR_BYTES);
	memcpy(det.orig, n->orig, M17_ADDR_BYTES);
	memcpy(det.via, n->via, M17_ADDR_BYTES);
	det.can = n->can;
	det.voice = (uint8_t)M17_TYPE_DATA(type);
	det.played = true;
	det.end_ms = net.end_ms;
	seq_end(&det_seq);
	det_have = true;
}

// A reflector stream takes the view: its caller, what it called, the reflector as via. A reflector rewrites DST (to
// the client, or to everyone): a call from it is never "to us", so no reply window opens on it
static OWN_FRAME void net_call_view(radio_m17_view_t *n, const m17_net_frame_t *f)
{
	m17_net_status_t s;
	uint64_t via = 0;

	if (m17_net_status_try(&s))
		via = m17_net_module_addr(s.t.name, s.t.module);
	net.view = true;
	net.end_ms = 0;
	n->flags &= ~(uint32_t)CALL_FLAGS;
	n->flags |= RADIO_M17_LSF;
	if (M17_TYPE_DATA(f->type) == M17_DATA_VOICE_DATA)
		n->flags |= RADIO_M17_1600;
	n->can = (uint8_t)M17_TYPE_CAN(f->type);
	n->enc = 0;
	n->why = M17_WHY_NONE;
	n->sig = RADIO_M17_SIG_NONE;
	memcpy(n->src, f->src, M17_ADDR_BYTES);
	memcpy(n->dst, f->dst, M17_ADDR_BYTES);
	memcpy(n->orig, f->src, M17_ADDR_BYTES);
	m17_addr_put(n->via, via);
	n->rssi_dbm = 0;
	n->call_seq++;
	n->meta_seq++;
	net_det(n, f->type);
}

// The reflector stream stops playing (its end, a gap, the air or PTT first)
static void net_rx_stop(radio_m17_view_t *n, uint32_t now)
{
	net.skip = false;
	if (!net.rx)
		return;
	net.rx = false;
	(void)net_push(NULL, false, true);
	if (net.view) {		// (an air call may have taken the view first)
		net.end_ms = now != 0 ? now : 1u;
		net_det(n, net.rx_type);
		n->meta_seq++;
	}
#if CONFIG_HEARD_LIST
	radio_heard_t h;

	memset(&h, 0, sizeof h);
	h.mode = RADIO_MODE_M17;
	h.flags = RADIO_HEARD_ADDR;
	memcpy(h.addr, net.rx_src, sizeof h.addr);
	memcpy(h.to, net.rx_dst, sizeof h.to);
	if (m17_addr_get(h.addr) != 0)
		radio_heard_add(&h);
#endif
}

// One frame from the reflector, in its turn
static void net_frame(radio_m17_view_t *n, const m17_net_frame_t *f, uint32_t now)
{
	bool end = (f->fn & M17_FN_END) != 0;

	net.last_ms = now;
	if (!((net.rx || net.skip) && f->sid == net.rx_sid)) {
		net_rx_stop(n, now);		// a new stream (the one before lost its last frame)
		net.rx_sid = f->sid;
		if (!net_voice(f->type)) {
			net.skip = true;	// data, or encrypted: nothing to play
		} else {
			net.rx = true;
			net.rx_type = f->type;
			memcpy(net.rx_src, f->src, M17_ADDR_BYTES);
			memcpy(net.rx_dst, f->dst, M17_ADDR_BYTES);
			net.rx_mode = (uint8_t)c2_mode((uint8_t)M17_TYPE_DATA(f->type));
			net_call_view(n, f);
			(void)net_push(NULL, true, false);
		}
	}
	// A signed stream's tail (the authentication and signature frames) is no voice; not checked over the network
	bool tail = (f->type & M17_TYPE_SIGNED) != 0 && (f->fn & M17_FN_MASK) >= M17_FN_AUTH;
	if (net.rx && !tail)
		(void)net_push(f->stream, false, false);
	if (end) {
		if (net.rx)
			net_rx_stop(n, now);
		net.skip = false;
	}
}

// The reflector's streams: played while nothing is heard on the air and nothing is sent, paced at a frame every 40 ms
// from a few in hand (the network's jitter); dropped otherwise
static OWN_FRAME void net_rx(radio_m17_view_t *n, const m17_engine_view_t *ev, uint32_t now)
{
	m17_net_frame_t f;

	if (ev->rx_active || ev->tx != AIR_TX_IDLE || net.tx != NTX_OFF || run.tx_kind != 0xFFu || !m17_net_linked()) {
		net_rx_stop(n, now);
		m17_net_rx_flush();
		net.wait_ms = 0;
	} else {
		if ((net.rx || net.skip) && now - net.last_ms >= NET_GAP_MS)
			net_rx_stop(n, now);
		if (net.rx && (int32_t)(now - net.rel_ms) > (int32_t)(2u * M17_FRAME_MS))
			net.rel_ms = now;	// it ran dry: the pace starts again from the next frame
		for (;;) {
			uint8_t q = m17_net_rx_count();
			bool due = (int32_t)(now - net.rel_ms) >= 0;

			if (q == 0)
				break;
			if (!net.rx && !net.skip) {
				// Between streams: the next one gathers a few frames first
				if (net.wait_ms == 0)
					net.wait_ms = now != 0 ? now : 1u;
				if (q < NET_PRIME && now - net.wait_ms < NET_PRIME_MS)
					break;
				net.wait_ms = 0;
				net.rel_ms = now;
				due = true;
			} else if (net.rx && !due && q <= NET_AHEAD) {
				break;
			}
			if (!m17_net_rx_take(&f))
				break;
			net_frame(n, &f, now);
			if (due)
				net.rel_ms += M17_FRAME_MS;
		}
	}
	if (net.rx)
		n->flags |= RADIO_M17_RX | RADIO_M17_PLAYED;
	if (net.view)
		n->end_ms = net.rx ? 0 : net.end_ms;
}

#if CONFIG_M17_PACKET
_Static_assert(sizeof sms.data <= M17_NET_SMS_MAX, "an SMS fits the network's slot");

// SMS through the reflector: one from it into the inbox when it is ours; ours to it, at once (no channel to wait for)
static OWN_FRAME void net_sms(m17_engine_t *e)
{
	const m17_net_sms_t *in = m17_net_sms_peek();

	if (in != NULL) {
		m17_packet_info_t info;
		uint8_t tl = 0;

		memset(&info, 0, sizeof info);
		info.src = in->src;
		info.dst = in->dst;
		info.type = m17_pkt_type_get(in->data, in->len, &tl);
		if (tl != 0 && info.type == M17_PKT_SMS) {
			if (!sms_ours(&info, e->cfg.dst)) {
				stats.sms_dropped++;
			} else {
				uint8_t peer[M17_ADDR_BYTES];
				uint16_t k = 0;

				while (tl + k < in->len && in->data[tl + k] != 0)
					k++;
				m17_addr_put(peer, in->src);
				inbox_add(peer, false, in->data + tl, k, in->cut);
				stats.sms_rx++;
			}
		}
		m17_net_sms_done();
	}
	if (sms.waiting && !sms.inflight && m17_net_talks()) {
		m17_net_sms_t *o;

		if (sms_clear_refused()) {
			sms_done(RADIO_M17_SMS_BUSY);	// the channel was set to encrypt: never in the clear
		} else if ((o = m17_net_sms_slot()) != NULL) {
			o->src = ch_now.own;
			o->dst = sms.dst;
			o->can = e->cfg.tx_can;
			o->cut = false;
			o->len = sms.len;
			memcpy(o->data, sms.data, sms.len);
			m17_net_sms_post();
			inbox_add(sms.to, true, &sms.data[1], (uint16_t)(sms.len - 2u), false);
			stats.sms_tx++;
			sms_done(RADIO_M17_SMS_SENT);	// handed to the reflector: M17 has no delivery report
		}
	}
}
#endif

// A new stream ID per PTT, never the last one
static uint16_t net_sid(void)
{
	uint16_t sid;
#if CONFIG_ENTROPY
	uint8_t b[2];

	if (entropy_get(b, sizeof b)) {
		sid = (uint16_t)(b[0] << 8 | b[1]);
	} else
#endif
	{
		net.rng = net.rng * 1103515245u + 12345u + hal_micros();
		sid = (uint16_t)(net.rng >> 16);
	}
	if (sid == net.last_sid)
		sid ^= 0xA5A5u;
	net.last_sid = sid;
	return sid;
}

// PTT linked to talk: the stream goes to the reflector, not the air. In the clear and unsigned: a channel set to
// encrypt refuses (never in the clear), signing is not done over the network
static bool net_tx_start(m17_engine_t *e)
{
	const m17_engine_cfg_t *c = &e->cfg;

	if (c->tx_enc != M17_ENC_NONE) {
		run.prep_deny = TXDENY_REFUSED;
		return false;
	}
	net.tx_sid = net_sid();
	net.tx_fn = 0;
	net.tx_mode = (uint8_t)c2_mode(c->voice);
	net.tx_type = m17_type(true, c->voice == M17_DATA_VOICE_DATA ? M17_DATA_VOICE_DATA : M17_DATA_VOICE,
			       M17_ENC_NONE, M17_META_TEXT, c->tx_can, false);
	m17_addr_put(net.tx_src, c->src);
	m17_addr_put(net.tx_dst, c->dst);
	net.tx = NTX_ON;
	run.tx_kind = M17_TX_STREAM;
	run.tx_aborted = false;
	m17_voice_tx(true, (c2_mode_t)net.tx_mode);
	return true;
}

static bool net_elsewhere(void)
{
	return net.tx != NTX_OFF;
}
#endif

bool radio_m17_poll(radio_m17_view_t *v)
{
	m17_engine_view_t ev;
	radio_m17_view_t n = *v;
	uint32_t now = hal_millis();
	m17_engine_t *e;

	run.ticks++;
	n.flags &= (uint32_t)CALL_FLAGS;	// the rest is made again below
	if (mem == NULL) {
		n.tx = AIR_TX_IDLE;
		n.tx_phase = M17_TXP_IDLE;
		settings_view(&n);
		bool changed = memcmp(&n, v, sizeof n) != 0;
		*v = n;
		return changed;
	}
	e = &mem->eng;
	m17_engine_view(e, &ev);
	m17_voice_poll();
#if CONFIG_NET_M17
	net_tx_pump();		// what the encoder made since the last tick, to the reflector
#endif
	if (run.masked) {
		if (!ev.rx_active || ev.rx_calls != run.masked_calls)
			run.masked = false;
		else
			ev.rx_active = ev.rx_played = false;
	}
#if CONFIG_ENTROPY
	if (ev.noise != run.noise) {
		run.noise = ev.noise;
		entropy_mix_u32(ev.noise, 1);	// soft values of at least a chunk since the last: far over 8 bytes
	}
#endif
	run.rx_on = ev.rx_active;
	n.flags |= RADIO_M17_ON;
	n.tx = ev.tx;
	n.tx_phase = ev.tx_phase;
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF) {
		n.tx = net.tx == NTX_ON ? (uint8_t)AIR_TX_ON : (uint8_t)AIR_TX_DONE;	// our stream to the reflector
		n.tx_phase = net.tx == NTX_ON ? (uint8_t)M17_TXP_DATA : (uint8_t)M17_TXP_EOT;
	}
#endif
	// A new call: its fields from now on; a call that ended: its end, the heard list
	bool fresh = ev.rx_calls != run.calls && ev.rx.flags != 0 && !run.masked;
	if ((run.active && (!ev.rx_active || fresh))) {
		run.end_ms = now;
		run.dirty = true;
#if CONFIG_HEARD_LIST
		// A signed call goes in once its check concluded (sig_verdict); without signatures there is none
		if (!CONFIG_M17_SIG || !(n.flags & RADIO_M17_SIGNED))
			heard_add(&n, fresh ? NULL : &ev.rx);
#endif
	}
	if (fresh) {
#if CONFIG_NET_M17
		net.view = false;	// an air call takes the view back
#endif
		run.calls = ev.rx_calls;
		run.end_ms = 0;
		run.sig = RADIO_M17_SIG_NONE;
		run.text_seq = ev.text_seq;
		n.call_seq++;
		seq_begin(&det_seq);
		memset(&det, 0, sizeof det);
		seq_end(&det_seq);
		run.dirty = true;
	}
	// the engine's call is the one the view has (not after a reflector call took it)
	bool mine = run.calls != 0 && ev.rx_calls == run.calls && !NET_VIEW();
	run.active = mine && ev.rx_active;
	if (mine) {
		uint32_t flags0 = n.flags;
		uint8_t sig0 = n.sig;
		uint64_t src0 = m17_addr_get(n.src), orig0 = m17_addr_get(n.orig);

		call_view(&n, &ev, run.active);
		if (run.active && ev.rx_played)
			n.flags |= RADIO_M17_PLAYED;
		if (((n.flags ^ flags0) & CALL_FLAGS) != 0 || n.sig != sig0 || ev.text_seq != run.text_seq ||
		    m17_addr_get(n.src) != src0 || m17_addr_get(n.orig) != orig0)
			run.dirty = true;
	}
	// Any reception is one (a call, a BERT run, a stream's first frames before its fields are in)
	if (ev.rx_active)
		n.flags |= RADIO_M17_RX;
	n.end_ms = run.active ? 0 : run.end_ms != 0 ? run.end_ms : n.end_ms;
#if CONFIG_M17_SIG
	// One piece of curve work a tick: our signature first (its tail is on air), then a verification's key, then the
	// key check. A check is taken into the job's own record (not while one runs: the next tick takes it)
	if (sign_step(e)) {
	} else if (!vj.on && m17_engine_sig_take(e, &vj.chk)) {
		if (mine)
			sig_job_start();
	} else if (vj.on) {
		sig_job_step();
	} else if (mine && !run.active && run.sig == RADIO_M17_SIG_CHECKING && now - run.end_ms > CHECK_WAIT_MS) {
		vj.chk.signer = vj.chk.dst = 0;
		vj.chk.ts = 0;
		sig_verdict(RADIO_M17_SIG_LOST, RADIO_M17_FRESH_UNKNOWN);	// the check never came (another reception took it first)
	} else {
		key_check(&ev, now);
	}
	if (heard_sig != RADIO_M17_SIG_NONE) {
		if (!NET_VIEW())
			n.sig = run.sig;
#if CONFIG_HEARD_LIST
		if (mine && !run.active)
			heard_add(&n, &ev.rx);
#endif
		heard_sig = RADIO_M17_SIG_NONE;
		n.meta_seq++;
	}
#endif
	if (run.dirty && mine) {
		run.dirty = false;
		det_fill(e, &ev, &n);
		n.meta_seq++;
	}
#if CONFIG_M17_PACKET
	if (e->pkt_hold.full) {
		packets(e, &ev);
		n.meta_seq++;
	}
	if (sms.staged)
		radio_m17_sms_send(0, NULL);	// (its request is on its way, or was lost with a full queue)
	if (sms.waiting && !sms.inflight && now - sms.queued_ms >= RADIO_M17_SMS_BUSY_MS)
		sms_done(RADIO_M17_SMS_BUSY);	// the channel stayed busy (or the radio would not key)
#endif
#if CONFIG_M17_BERT
	bert_poll(&ev, ev.rx_active, now, &n);
#endif
#if CONFIG_NET_M17
	net_rx(&n, &ev, now);
#if CONFIG_M17_PACKET
	net_sms(e);
#endif
#endif
	gnss_poll(e, now);
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof n) != 0;
	*v = n;
	return changed;
}

bool radio_m17_details(radio_m17_details_t *out)
{
	if (!det_have)
		return false;
	seq_copy(&det_seq, out, &det, sizeof *out);
	return true;
}

bool radio_m17_key(radio_m17_key_t *out)
{
#if CONFIG_M17_SIG
	seq_copy(&key_seq, out, &key, sizeof *out);
	return true;
#else
	memset(out, 0, sizeof *out);
	return false;
#endif
}

// --- Transmit ---
// What refuses a voice stream (not a packet or a BERT run: neither is encrypted or signed, m17.md 8, 9)
static uint8_t stream_deny(void)
{
	const m17_engine_cfg_t *c = &mem->eng.cfg;

	if (ch_now.enc_want != M17_ENC_NONE && c->tx_enc == M17_ENC_NONE)
		return TXDENY_ENC_NOT_BUILT;
	if (c->tx_enc != M17_ENC_NONE && c->key_len == 0)
		return TXDENY_ENC_NO_KEY;
#if CONFIG_M17_SIG
	if (wide.sign && !key_usable)
		return TXDENY_SIG_NO_KEY;
	if (wide.sign && key.damaged)
		return TXDENY_SIG_KEY_BAD;
#endif
	return TXDENY_NONE;	// (a build without signatures sends unsigned: no Settings row asks for it there)
}

uint8_t radio_m17_tx_deny(void)
{
	if (mem == NULL)
		return TXDENY_NONE;	// air_key refuses first: no engine
	if (ch_now.own == 0)
		return ch_now.own_bad ? TXDENY_CALLSIGN_BAD : TXDENY_NO_CALLSIGN;
	if (run.prep_deny != TXDENY_NONE) {
		uint8_t d = run.prep_deny;

		run.prep_deny = TXDENY_NONE;	// said once, after the prepare that refused
		return d;
	}
#if CONFIG_M17_BERT
	if (bert.mode || bert.tx)
		return TXDENY_NONE;	// BERT frames are neither encrypted nor signed
#endif
#if CONFIG_M17_PACKET
	// An SMS waiting for the channel: the radio core asks before the mode's own transmission, which a stream's
	// refusal must not stop. A PTT meanwhile is a stream, refused by the prepare (prep_deny)
	if (sms.waiting && !sms.inflight)
		return TXDENY_NONE;
#endif
	return stream_deny();
}

uint8_t radio_m17_auto_tx(bool busy, bool keyed, uint32_t now_ms)
{
	if (mem == NULL)
		return RADIO_AIR_AUTO_NO;
#if CONFIG_M17_BERT
	if (keyed)
		return run.tx_kind == M17_TX_BERT && bert.tx ? RADIO_AIR_AUTO_HOLD : RADIO_AIR_AUTO_NO;
	if (bert.tx) {
		run.auto_kind = AUTO_BERT;
		run.auto_tick = run.ticks;
		return RADIO_AIR_AUTO_HOLD;
	}
#else
	if (keyed)
		return RADIO_AIR_AUTO_NO;
#endif
#if CONFIG_M17_PACKET
#if CONFIG_NET_M17
	if (sms.waiting && !sms.inflight && m17_net_talks())
		return RADIO_AIR_AUTO_NO;	// it goes to the reflector (net_sms), not on the air
#endif
	if (sms.waiting && !sms.inflight && sms_clear_refused()) {
		sms_done(RADIO_M17_SMS_BUSY);	// the channel was set to encrypt after it was queued: never in the clear
		return RADIO_AIR_AUTO_NO;
	}
	if (sms.waiting && !sms.inflight) {
		// A slot is 40 ms of a clear channel, seen clear: a transmission the engine hears is busy whatever the radio
		// core's state says (a PTT refused, then let go, shows idle before it shows the call again), and a slot
		// starts over when the channel went unwatched (the radio core didn't ask: a PTT held, a refusal)
		bool unwatched = now_ms - sms.asked_ms > SMS_SLOT_MS;

		sms.asked_ms = now_ms;
		if (busy || run.rx_on || unwatched) {
			sms.slot_ms = now_ms;
			return RADIO_AIR_AUTO_NO;
		}
		if (now_ms - sms.slot_ms < SMS_SLOT_MS)
			return RADIO_AIR_AUTO_NO;
		sms.slot_ms = now_ms;
		sms.rng = sms.rng * 1103515245u + 12345u;
		if (((sms.rng >> 16) & 3u) != 0)
			return RADIO_AIR_AUTO_NO;	// p = 0.25: the slot is someone else's
		if (!m17_engine_packet_load(&mem->eng, sms.dst, sms.data, sms.len))
			return RADIO_AIR_AUTO_NO;
		run.auto_kind = AUTO_PACKET;
		run.auto_tick = run.ticks;
		return RADIO_AIR_AUTO_ONCE;
	}
#else
	(void)busy;
	(void)now_ms;
#endif
	return RADIO_AIR_AUTO_NO;
}

static bool m17_no_tx(void)
{
#if CONFIG_NET_M17
	if (m17_net_talks())
		return false;	// PTT goes to the reflector: the provider's transmit doesn't matter
#endif
	return mem != NULL && (mem->eng.session.fit & AIR_FIT_TX) == 0;
}

static bool m17_tx_prepare(void)
{
	m17_tx_req_t req;
	m17_engine_t *e;
	uint32_t unix_s;
	uint8_t a = run.auto_kind;

	if (mem == NULL)
		return false;
	e = &mem->eng;
	run.auto_kind = AUTO_NONE;
	if (run.auto_tick != run.ticks)
		a = AUTO_NONE;		// a stale ask: this is the user's PTT
	memset(&req, 0, sizeof req);
	req.kind = a == AUTO_PACKET ? (uint8_t)M17_TX_PACKET : (uint8_t)M17_TX_STREAM;
#if CONFIG_M17_BERT
	if (a == AUTO_BERT || (a == AUTO_NONE && bert.mode))
		req.kind = M17_TX_BERT;
#endif
	if (req.kind == M17_TX_STREAM && (run.prep_deny = stream_deny()) != TXDENY_NONE)
		return false;		// the user's PTT while an SMS waited (tx_deny let it through for the SMS)
#if CONFIG_NET_M17
	if (req.kind == M17_TX_STREAM && m17_net_talks())
		return net_tx_start(e);
#endif
	if (position_utc_now(hal_millis(), UTC_FRESH_MS, &unix_s, NULL))
		req.ts = utc_since_2020(unix_s);
#if CONFIG_M17_AES
	if (req.kind == M17_TX_STREAM && e->cfg.tx_enc == M17_ENC_AES) {
		bool ok;

		// The key-up instant, in microseconds since start-up: a person's timing, new at every press. On a radio
		// without a hardware generator it is what keeps the nonce apart from an earlier start-up's when the boot
		// count went back (docs/spec/crypto.md 8.3); entropy_nonce refuses a weak pool that had no such stir
		entropy_mix_u32(hal_micros(), 2);
		crash_secret_enter();
		ok = entropy_nonce(req.rnd, sizeof req.rnd);
		crash_secret_leave();
		if (!ok) {
			stats.nonce_refused++;
			run.prep_deny = TXDENY_ENC_NOT_READY;	// the radio core asks tx_deny why
			return false;
		}
	}
#endif
	// Our position in the LSF only when 5 s have passed since one may have gone out (the engine would send the latest it
	// has in every transmission). A single flag of the configuration, written while nothing is sent
	if (req.kind == M17_TX_STREAM) {
		uint32_t now = hal_millis();

		e->cfg.gnss = wide.gnss && gnss_window(now);
		if (e->cfg.gnss && gnss_given) {
			gnss_air_ms = now;
			gnss_air = true;
		}
	}
	bool ok = m17_engine_tx_prepare(e, &req);
	crypto_wipe(req.rnd, sizeof req.rnd);
	if (!ok)
		return false;
	run.tx_kind = req.kind;
	run.tx_aborted = false;
	if (req.kind == M17_TX_STREAM)
		m17_voice_tx(true, c2_mode(e->cfg.voice));
#if CONFIG_M17_PACKET
	if (req.kind == M17_TX_PACKET)
		sms.inflight = true;
#endif
#if CONFIG_M17_BERT
	if (req.kind == M17_TX_BERT) {
		bert.tx_start_ms = hal_millis();
		bert.tx_syms0 = e->session.stats.tx_syms;
	}
#endif
	return true;
}

static bool m17_tx_go(void)
{
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF)
		return true;	// nothing to start on the air
#endif
	return mem != NULL && m17_engine_tx_go(&mem->eng);
}

static void m17_tx_stop(void)
{
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF) {
		net_tx_end();
		return;
	}
#endif
	if (mem != NULL) {
		m17_voice_tx(false, C2_MODE_3200);	// the engine fills the frame on air, and a signed tail, with silence
		m17_engine_tx_stop(&mem->eng);
	}
}

static void m17_tx_abort(void)
{
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF) {
		net_tx_end();
		run.tx_aborted = true;
		return;
	}
#endif
	if (mem != NULL) {
		m17_voice_tx(false, C2_MODE_3200);
		m17_engine_tx_abort(&mem->eng);
		run.tx_aborted = true;
#if CONFIG_M17_BERT
		if (run.tx_kind == M17_TX_BERT)
			bert.tx = false;	// the time-out ended the run: the radio core doesn't key it again
#endif
	}
}

static uint8_t m17_tx_state(void)
{
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF)
		return net.tx == NTX_ON ? (uint8_t)AIR_TX_ON : (uint8_t)AIR_TX_DONE;
#endif
	return mem != NULL ? (uint8_t)m17_engine_tx_state(&mem->eng) : (uint8_t)AIR_TX_IDLE;
}

static void m17_tx_clear(void)
{
	if (mem == NULL)
		return;
#if CONFIG_NET_M17
	if (net.tx != NTX_OFF) {
		net.tx = NTX_OFF;
		m17_voice_tx(false, C2_MODE_3200);
		run.tx_kind = 0xFFu;
		return;
	}
#endif
	m17_voice_tx(false, C2_MODE_3200);
	m17_engine_tx_clear(&mem->eng);
#if CONFIG_M17_PACKET
	if (run.tx_kind == M17_TX_PACKET && sms.inflight) {
		sms.inflight = false;
		if (!run.tx_aborted) {
			inbox_add(sms.to, true, &sms.data[1], (uint16_t)(sms.len - 2u), false);
			stats.sms_tx++;
			sms_done(RADIO_M17_SMS_SENT);	// on air: M17 has no delivery report
		} else if (++sms.tries >= 3u) {
			sms_done(RADIO_M17_SMS_BUSY);	// cut short again and again
		}
	}
#endif
#if CONFIG_M17_BERT
	if (run.tx_kind == M17_TX_BERT) {
		uint32_t frames = (mem->eng.session.stats.tx_syms - bert.tx_syms0) / M17_FRAME_SYMS;

		seq_begin(&bert_seq);	// the pair as one, as bert_poll writes it
		bert_pub.tx_frames = frames;
		bert_pub.tx_ms = 0;
		seq_end(&bert_seq);
	}
#endif
	run.tx_kind = 0xFFu;
}

const radio_air_ops_t radio_m17_air = {
	radio_m17_running, m17_no_tx, m17_tx_prepare, m17_tx_go, m17_tx_stop, m17_tx_abort, m17_tx_state, m17_tx_clear,
};

// --- The BER test ---
#if CONFIG_M17_BERT
void radio_m17_bert_mode(bool on)
{
	bert.mode = on;
}

void radio_m17_bert_tx(bool on)
{
	bert.tx = on && mem != NULL;
}

void radio_m17_bert_reset(void)
{
	m17_engine_view_t ev;

	if (mem == NULL)
		return;
	m17_engine_view(&mem->eng, &ev);
	bert.frames = ev.bert_frames;
	bert.bits = ev.bert_bits;
	bert.errs = ev.bert_errs;
	bert.locks = ev.bert_locks;
	bert.losses = ev.bert_losses;
}

void radio_m17_bert_get(radio_m17_bert_t *out)
{
	seq_copy(&bert_seq, out, &bert_pub, sizeof *out);
}
#endif

// --- The benches ---
bool radio_m17_bench_sig(bool *ok)
{
#if CONFIG_M17_SIG
	// docs/spec/m17.md 7.5: d, H, and the r || s RFC 6979 gives
	static const uint8_t d[32] = {
		0x3F, 0x9B, 0xC0, 0x00, 0xFA, 0xB8, 0x3A, 0xED, 0x45, 0x51, 0xEC, 0x40, 0xA5, 0xF6, 0xA7, 0xC1,
		0x78, 0xB2, 0x55, 0xBA, 0x65, 0x4C, 0x06, 0x84, 0xD2, 0x42, 0xBC, 0x8F, 0x59, 0xF8, 0xDA, 0x18,
	};
	static const uint8_t h[32] = {
		0x2D, 0xD1, 0xBC, 0x7B, 0x5C, 0x01, 0x7F, 0x10, 0x0F, 0x93, 0x79, 0xF4, 0x2E, 0xAA, 0x20, 0x5C,
		0xE9, 0x0B, 0x8C, 0x7E, 0xA2, 0x63, 0xF2, 0xF8, 0x0E, 0x37, 0xBA, 0x6E, 0x6D, 0x6B, 0x58, 0x8D,
	};
	static const uint8_t rs[64] = {
		0xE6, 0x89, 0x78, 0x8A, 0x5C, 0x2F, 0xBD, 0xD0, 0xFA, 0x21, 0x37, 0x42, 0x95, 0x17, 0xB7, 0xC9,
		0xDD, 0x5E, 0x3C, 0x2B, 0xFF, 0xB9, 0x66, 0x7D, 0x4E, 0xDB, 0x9A, 0x84, 0x72, 0xF3, 0xCF, 0x31,
		0xC4, 0x54, 0x21, 0xDE, 0xD7, 0xF7, 0x02, 0x56, 0x08, 0xBF, 0xD9, 0x28, 0xB0, 0x6B, 0xEF, 0xD4,
		0xA4, 0x45, 0x26, 0x60, 0x9B, 0x3D, 0xC5, 0x4F, 0xC2, 0xB3, 0x69, 0x44, 0xF6, 0x12, 0x1D, 0x54,
	};
	sig_work_t *w;
	uint32_t t0;
	bool good;

	*ok = false;
	if (mem == NULL || (w = borrow()) == NULL)
		return false;
	memcpy(w->d, d, sizeof d);
	memcpy(w->h, h, sizeof h);
	t0 = hal_micros();
	good = p256_sign(w->d, w->h, w->rs, &w->p) && memcmp(w->rs, rs, sizeof rs) == 0;
	stats.sign_us = hal_micros() - t0;
	good = good && p256_pub_from_priv(w->d, w->q, &w->p);
	t0 = hal_micros();
	good = p256_verify(w->q, w->h, rs, &w->p) && good;
	stats.verify_us = hal_micros() - t0;
	give_back(w);
	*ok = good;
	return true;
#else
	*ok = false;
	return false;
#endif
}

void radio_m17_bench_codec(uint8_t phase, uint8_t mode)
{
	if (mem != NULL && m17_engine_tx_state(&mem->eng) == AIR_TX_IDLE)
		m17_voice_bench(phase, mode ? C2_MODE_1600 : C2_MODE_3200);
}

void radio_m17_stats(radio_m17_stats_t *out)
{
	lock();
	*out = stats;
	if (mem != NULL)
		engine_counts(out, &mem->eng, true);
#if CONFIG_M17_SIG
	out->sig_tail = mem != NULL ? mem->eng.cfg.sig_tail : sig_tail();
#endif
	unlock();
}

void radio_m17_reset_stats(void)
{
	lock();
	memset(&stats, 0, sizeof stats);
	if (mem != NULL)
		engine_counts(&stats, &mem->eng, false);
	unlock();
}

// --- The radio core's row (radio_air_modes.h): radio_sm.c runs M17 through it ---
static bool row_begin(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	(void)own_id;		// M17's address is its callsign, never the DMR ID
	return radio_m17_begin(ch, number);
}

static void row_apply(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	(void)own_id;
	radio_m17_apply(ch, number);
}

static bool row_poll(radio_view_t *v)
{
	// The key check waits for a radio idle on this channel: no scan or dual watch visit (dual watch on a second
	// receiver leaves the main one here), no hotspot, nothing sent
	ch_now.idle = v->state == RS_IDLE && !v->hotspot && (v->scan == SCAN_OFF || (v->scan == SCAN_DUAL && v->rx2));
	ch_now.tot_s = v->tot_s;
	return radio_m17_poll(&v->m17);
}

static bool row_rx(const radio_view_t *v)
{
	return (v->m17.flags & RADIO_M17_RX) != 0;
}

// A transmission on the channel: pending while its squelch can't tell yet (no TYPE: a late joiner's first frames),
// played, or decided against (another CAN, encrypted, data)
static uint8_t row_heard(const radio_view_t *v)
{
	const radio_m17_view_t *n = &v->m17;

	if ((n->flags & RADIO_M17_RX) == 0)
		return RADIO_AIR_HEARD_NONE;
	if ((n->flags & RADIO_M17_PLAYED) != 0)
		return RADIO_AIR_HEARD_PLAYED;
	return n->why == M17_WHY_NONE ? RADIO_AIR_HEARD_PENDING : RADIO_AIR_HEARD_OTHER;
}

static bool row_played(const radio_view_t *v)
{
	return (v->m17.flags & RADIO_M17_PLAYED) != 0;
}

// The last call heard and the settings stay in the view
static void row_view_off(radio_view_t *v)
{
	v->m17.flags &= ~(uint32_t)(RADIO_M17_ON | RADIO_M17_RX | RADIO_M17_PLAYED | RADIO_M17_BERT | RADIO_M17_SMS_WAIT);
	v->m17.tx = AIR_TX_IDLE;
	v->m17.tx_phase = M17_TXP_IDLE;
}

const radio_air_mode_t radio_m17_mode = {
	.mode = RADIO_MODE_M17,
	.key = "m17",
	.name = "M17",
	.features = "M17,",
	.fits = radio_m17_fits,
	.take = radio_m17_take,
	.receives = radio_m17_receives,
	.begin = row_begin,
	.end = radio_m17_end,
	.running = radio_m17_running,
	.retuned = radio_m17_retuned,
	.apply = row_apply,
	.poll = row_poll,
	.rx = row_rx,
	.heard = row_heard,
	.played = row_played,
	.view_off = row_view_off,
	.tx_deny = radio_m17_tx_deny,
	.auto_tx = radio_m17_auto_tx,
#if CONFIG_NET_M17
	.tx_elsewhere = net_elsewhere,
#endif
	.ops = &radio_m17_air,
};

#endif	// CONFIG_M17
