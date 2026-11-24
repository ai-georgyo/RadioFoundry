// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on a channel, the radio core's side (docs/spec/m17.md, the radio core's section): the engine
// (core/m17/m17_engine.h) and the voice path (core/m17/m17_voice.h) on the memory they run in (radio_air_mem.h), the
// channel's M17 parameters, the signature work (signing at PTT release, verifying after a signed call, both in the
// radio task on the voice path's lent scratch), received packets into the SMS inbox, the stations heard, the BER test,
// and a compact view for the radio view. radio_sm.c decides when M17 runs and keys the transmitter (radio_air.h);
// nothing here keys it, but a packet or a BERT run asks to be sent (radio_m17_auto_tx).
//
// The parameters of a stored M17 channel are its own (docs/spec/m17-codeplug.md); the VFO and a channel whose mode was
// changed to M17 for now take the RAM settings (radio_req_m17_settings), with our callsign, the call text and the sign
// and position choices from the radio-wide record.
//
// Addresses. In memory a broadcast destination is M17_ADDR_BROADCAST (all 0xFF) everywhere: settings, view, details,
// heard list. Zero means "none / not known yet" and is never a destination. Only the stored parameter block spells
// broadcast as zeros, and only m17_conv_decode / m17_conv_encode convert (m17_codeplug.h).
//
// The UI reads: radio_view_t.m17 every tick (radio_m17_view_t: who calls whom, why it doesn't play, the signature
// state, the transmit phase), radio_m17_details once a call has ended or when Call Details opens (the META text and
// position, the signature's fields, the packet heard), radio_m17_key for the signing key page, radio_m17_bert_get for
// the BER test page, radio_m17_sms_* for Messages. Text is made at display time (m17_addr_text).
#ifndef RADIO_M17_H
#define RADIO_M17_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"

// The M17 settings (the VFO's, and the shape of what is in force). can and mode1600 persist in settings_t (the UI's
// Talk & listen rows); the rest is RAM only
typedef struct {
	uint8_t can;		// 0..15
	uint8_t tx_can;		// 0..15
	uint8_t squelch;	// M17_SQUELCH_* (m17_engine.h)
	bool mode1600;		// PTT sends Codec 2 1600 + data, else 3200
	uint8_t dst[6];		// what PTT calls, 48 bits big endian; all 0xFF: broadcast; never all zero
} radio_m17_settings_t;

// view flags (radio_m17_view_t.flags)
#define RADIO_M17_ON		0x00000001u	// the engine runs on this channel
#define RADIO_M17_RX		0x00000002u	// a transmission is being received
#define RADIO_M17_PLAYED	0x00000004u	// ... and passes the squelch (its voice plays)
#define RADIO_M17_ENC		0x00000008u	// ... is encrypted or scrambled (enc says which)
#define RADIO_M17_LATE		0x00000010u	// ... was entered without its LSF
#define RADIO_M17_LSF		0x00000020u	// ... its source and destination are known
#define RADIO_M17_1600		0x00000040u	// ... is voice + data (Codec 2 1600)
#define RADIO_M17_TEXT		0x00000080u	// ... has META text (radio_m17_details)
#define RADIO_M17_GNSS		0x00000100u	// ... has a position (radio_m17_details)
#define RADIO_M17_PACKET	0x00000200u	// ... is a packet, not a stream
#define RADIO_M17_BERT		0x00000400u	// a BERT run is being received or sent
#define RADIO_M17_TX_1600	0x00000800u	// what PTT sends is 1600 + data
#define RADIO_M17_NO_CALL	0x00001000u	// we have no callsign: PTT is refused (TXDENY_NO_CALLSIGN)
#define RADIO_M17_CALL_BAD	0x00002000u	// the stored callsign does not encode (TXDENY_CALLSIGN_BAD)
#define RADIO_M17_SIGNED	0x00004000u	// heard: the stream says it is signed (TYPE bit 11)
#define RADIO_M17_TO_US		0x00008000u	// heard: DST is our callsign (the reply window applies)
#define RADIO_M17_DATA		0x00010000u	// heard: a stream of data only (no voice)
#define RADIO_M17_TX_SIGN	0x00020000u	// what PTT sends is signed (the radio-wide choice and a usable key)
#define RADIO_M17_TX_ENC	0x00040000u	// what PTT sends is encrypted (the channel's choice and its key)
#define RADIO_M17_BERT_MODE	0x00080000u	// the BER test page holds the engine in BERT mode: PTT sends BERT
#define RADIO_M17_SMS_WAIT	0x00100000u	// an SMS waits for the channel (CSMA)
#define RADIO_M17_HAS_DETAILS	0x00200000u	// radio_m17_details has a call (the last one heard)

// radio_m17_view_t.sig and radio_m17_details_t.sig: what the UI says about a call's signature (docs/spec/m17.md, the
// radio core's section; the UX's signature states). A failure never says "forged": a corrupt frame looks the same
enum {
	RADIO_M17_SIG_NONE,		// unsigned, and no key on file for the originator: nothing shown
	RADIO_M17_SIG_UNSIGNED_KNOWN,	// unsigned, but the radio holds a key for the originator: "Not signed"
	RADIO_M17_SIG_RUNNING,		// signed, the stream still coming: "Signed: checked at end"
	RADIO_M17_SIG_CHECKING,		// signed, ended, the check waiting or running
	RADIO_M17_SIG_VALID,		// verifies with a key of the signer's, and the signer is the originator
	RADIO_M17_SIG_SIGNER_DIFFERS,	// verifies, but the signer is not the originator
	RADIO_M17_SIG_INVALID,		// does not verify with any key tried (or zeros: the sender was late)
	RADIO_M17_SIG_NO_KEY,		// no public key on file for the signer
	RADIO_M17_SIG_LATE,		// joined after frame 0: not checked
	RADIO_M17_SIG_LOST,		// frames lost (an FN gap) or the tail cut off: not checked
	RADIO_M17_SIG_OLD,		// verifies, but a trusted clock puts the timestamp outside the window
	RADIO_M17_SIG_REPLAY,		// verifies, but the hash repeats an earlier call's (the replay cache)
};

// Freshness of a signed call's timestamp (radio_m17_details_t.sig_fresh). A timestamp of 0 is a sender without a
// clock: always UNKNOWN, never NO
enum { RADIO_M17_FRESH_UNKNOWN, RADIO_M17_FRESH_YES, RADIO_M17_FRESH_NO };

// The radio view's part (radio_view_t.m17): the transmission heard now, or the last one, and what PTT does. Addresses
// as bytes (no 64-bit field: the view is copied to the UI every tick and stays small)
typedef struct {
	uint32_t flags;		// RADIO_M17_*
	uint16_t fit;		// what the board's provider gives M17 (AIR_FIT_*), 0: nothing
	uint8_t tx;		// air_tx_state_t of the engine
	uint8_t tx_phase;	// M17_TXP_* (m17_engine.h): M17_TXP_TAIL / _SIG are the UI's "SIGNING"
	uint8_t can;		// the CAN of the transmission heard
	uint8_t rx_can, tx_can;	// in force; rx_can M17_CAN_ANY: every CAN (digital monitor)
	uint8_t enc;		// heard: M17_ENC_* << 4 | subtype
	uint8_t tx_enc;		// sent: the same
	uint8_t sig;		// heard: RADIO_M17_SIG_*
	uint8_t why;		// heard: M17_WHY_* (m17_engine.h), why it does not play
	uint8_t call_seq;	// bumps when a new transmission is heard (the UI's Recent and banners key on it)
	uint8_t meta_seq;	// bumps when radio_m17_details has something new (text, position, signature verdict)
	uint8_t src[6];		// the transmission's SRC, zero until known
	uint8_t dst[6];		// what it called (broadcast: all 0xFF)
	uint8_t orig[6];	// the originator: the extended callsign META's first field when present, else SRC
	uint8_t via[6];		// the reflector (extended callsign META's second field), else SRC when it differs from
				// the originator (a repeater, a gateway), else zero
	uint8_t talk[6];	// what PTT calls (broadcast: all 0xFF)
	uint8_t own[6];		// our callsign (zero: none)
	int16_t rssi_dbm;	// of the transmission heard, 0 unknown
	uint32_t end_ms;	// hal_millis when the last transmission heard ended (0 while one runs, or none yet)
	uint16_t tot_s;		// the TX time-out in force: the radio's, capped by an encrypted or signed stream's limit
				// (M17_FN_ENC_LAST frames, 21 min 50 s)
} radio_m17_view_t;

// The last call heard in full (Call Details; radio_m17_details). One record, overwritten by the next call
typedef struct {
	uint8_t src[6], dst[6], orig[6], via[6];
	uint8_t can;
	uint8_t voice;		// M17_DATA_* of the stream (0 for a packet)
	uint8_t enc;		// M17_ENC_* << 4 | subtype
	bool played;
	char text[53];		// META text, UTF-8, NUL-terminated, "" none
	bool gnss;		// the fields below hold a position
	int32_t lat_e7, lon_e7;
	bool alt_ok, vel_ok;
	int32_t alt_dm;		// decimeters
	uint16_t speed_kmh10, bearing_deg;
	uint8_t station;	// M17_STATION_*
	uint8_t sig;		// RADIO_M17_SIG_*
	uint8_t signer[6];	// the authentication frame's signer, zero none
	uint8_t sig_dst[6];	// ... its destination (no warning when it differs from dst: gateways rewrite DST)
	uint32_t sig_ts;	// ... its timestamp, seconds since 2020-01-01 UTC; 0: the sender sent none
	uint8_t sig_fresh;	// RADIO_M17_FRESH_*
	bool packet;		// a packet, not a stream
	uint32_t pkt_type;	// M17_PKT_*
	uint16_t pkt_len;	// its bytes (data type and payload)
	int16_t rssi_dbm;
	uint32_t end_ms;	// hal_millis when it ended
} radio_m17_details_t;

// Our signing key as the radio sees it (radio_m17_key; the Signing Key page). Never d
typedef struct {
	bool present;		// the channel's system (or the first M17 system with a secret) has a signing key
	bool checked;		// the key check ran on it since the last codeplug change (d * G against the stored Q)
	bool damaged;		// ... and found d * G != Q: PTT with signing on is refused (TXDENY_SIG_KEY_BAD)
	uint8_t system;		// the system it belongs to
	uint8_t q[64];		// the public key X || Y (from the stored Q, or derived by the check)
	uint8_t fp[8];		// its fingerprint (m17_key_fp, m17_sig.h)
	uint32_t sign_us;	// the latest signature's time (0: none yet); sig_tail follows it
} radio_m17_key_t;

// The BER test (radio_m17_bert_get): the receiver's counters and what is sent
typedef struct {
	uint8_t state;		// RADIO_M17_BERT_*
	uint32_t frames, bits, errs, locks, losses;
	uint32_t tx_frames;	// BERT frames sent by the latest run
	uint32_t tx_ms;		// how long it has been sending (0: not sending)
	int16_t rssi_dbm;
} radio_m17_bert_t;
enum { RADIO_M17_BERT_NO_SIGNAL, RADIO_M17_BERT_SEARCHING, RADIO_M17_BERT_LOCKED, RADIO_M17_BERT_LOST };

// Counters for the debug link (any task reads; the radio task writes while M17 runs)
typedef struct {
	uint32_t begins, begin_fails;	// engine starts, and the ones refused (no provider, no fit, no memory)
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t frames, lsf_ok, lsf_bad, lich_ok, lich_bad, stream, packet, bert, fec_bits, can_rejects, eot,
		 head_rejects, enc_skipped, expect_miss, fn_limit;	// the engine's (m17_engine_stats_t), summed
	uint32_t rx_chunks, tx_underruns, tx_syms;	// the session's
	uint32_t pkt_rx, pkt_bad, pkt_tx, pkt_dropped, sms_rx, sms_tx, sms_dropped, sms_busy;
	uint32_t sig_valid, sig_invalid, sig_nokey, sig_incomplete, sig_late, sig_signed;
	uint32_t sig_keys_capped;	// verifications that stopped at RADIO_M17_VERIFY_KEYS_MAX keys
	uint32_t key_checks, key_damaged;	// key checks run, and the ones that found d * G != Q
	uint32_t sign_us, verify_us;	// the latest signature's and verification's time in the radio task
	uint8_t sig_tail;		// silence frames a signed stream ends with (from sign_us)
	uint32_t nonce_refused;		// AES key-ups refused: no nonce could be promised new (entropy_nonce)
	uint32_t mem_lent;		// the memory came from the board's vocoder
} radio_m17_stats_t;

// At most this many stored keys are tried for one signer per verification (each try up to about 200 ms in the radio
// task, with a yield between: docs/spec/watchdog.md 4); more is counted in sig_keys_capped
#define RADIO_M17_VERIFY_KEYS_MAX	4u

#if CONFIG_M17
#include "radio/radio_air.h"
#include "hal_audio.h"

extern const radio_air_ops_t radio_m17_air;

void radio_m17_init(const hal_audio_t *audio);
// The radio core's short copy lock (radio_task.c: the scheduler held), around the SMS inbox's reads and writes from
// any task. Not set (the host tests): none
void radio_m17_set_lock(void (*lock)(void), void (*unlock)(void));
// The board's provider gives M17 something (receive or transmit; any task): what the UI and the mode request offer
// M17 on (codeplug_modes.h CODEPLUG_RUNS_M17)
bool radio_m17_fits(void);
uint32_t radio_m17_fit(void);	// AIR_FIT_* (any task)
// A channel read from the codeplug: a live M17 channel (kind 6) becomes RADIO_MODE_M17 where the board carries M17 and
// its frequency allows a digital mode. False: left as read (parked)
bool radio_m17_take(channel_t *ch);
bool radio_m17_receives(const channel_t *ch, uint16_t number);
void radio_m17_settings(radio_m17_settings_t *out);		// any task
void radio_m17_set_settings(const radio_m17_settings_t *s);	// radio task (RREQ_M17_SET)
bool radio_m17_in_force(radio_m17_settings_t *out);		// any task, diagnostics

// Radio task, as radio_nxdn.h. begin: number 0 is the VFO
bool radio_m17_begin(const channel_t *ch, uint16_t number);
void radio_m17_end(void);
bool radio_m17_running(void);
void radio_m17_retuned(void);
void radio_m17_apply(const channel_t *ch, uint16_t number);
// Every tick: the view and the counters; the signature work, packets taken into the inbox, the heard list, entropy,
// the key check. True when the view changed. Long work (a verification, the key check) runs here, one key a tick at
// most, so radio_task.c yields between them
bool radio_m17_poll(radio_m17_view_t *v);
// Why PTT is refused before anything is queued (TXDENY_NONE: not refused), checked after the shared reasons:
// TXDENY_NO_CALLSIGN, TXDENY_CALLSIGN_BAD, TXDENY_ENC_NOT_BUILT, TXDENY_ENC_NO_KEY, TXDENY_ENC_NOT_READY,
// TXDENY_SIG_NO_KEY, TXDENY_SIG_KEY_BAD (radio.h)
uint8_t radio_m17_tx_deny(void);
// The mode wants to transmit on its own (a queued SMS: ONCE after the CSMA of m17.md 1.4, p = 0.25 in 40 ms slots;
// the debug link's BERT run: HOLD until radio_m17_bert_tx(false)). RADIO_AIR_AUTO_* (radio_air_modes.h)
uint8_t radio_m17_auto_tx(bool busy, bool keyed, uint32_t now_ms);

// Any task: the last call heard in full (a consistent copy). False when none was heard since M17 began
bool radio_m17_details(radio_m17_details_t *out);
// Any task: our signing key's state (a consistent copy). False without CONFIG_M17_SIG
bool radio_m17_key(radio_m17_key_t *out);

#if CONFIG_M17_PACKET
// SMS (m17.md 8, type 5). Send: queue text (UTF-8, at most RADIO_M17_SMS_MAX bytes) to dst on the M17 channel in use;
// it goes out by radio_m17_auto_tx. Radio task (RREQ_M17_SMS; another task calls radio_req_m17_sms). False when one is
// already waiting, M17 isn't running or we have no callsign
#define RADIO_M17_SMS_MAX	143u	// as the UI's messages (ui_msg_t)
#define RADIO_M17_SMS_INBOX	4u
#define RADIO_M17_SMS_BUSY_MS	10000u	// the channel stayed busy this long: the send gives up (RADIO_M17_SMS_BUSY)
bool radio_m17_sms_send(uint64_t dst, const char *utf8);
// Another task's way in (radio_req_m17_sms): the text copied straight into the send's buffer, under the lock; then
// RREQ_M17_SMS has the radio task call radio_m17_sms_send(0, NULL), which takes the staged one (and refuses it as it
// would any). False when a send is already staged, waiting or on air
bool radio_m17_sms_stage(uint64_t dst, const char *utf8);
// The latest send's outcome, and a count that bumps with each outcome (the UI's banner): sent (on air; M17 has no
// delivery report), or given up because the channel stayed busy
enum { RADIO_M17_SMS_NONE, RADIO_M17_SMS_SENT, RADIO_M17_SMS_BUSY };
uint8_t radio_m17_sms_result(uint8_t *seq, uint8_t to[6]);
// The inbox: received messages and the ones we sent, newest first, kept while the radio is on (RAM only, outside the
// lent memory: it outlives a mode change). Any task (under the radio core's view lock)
typedef struct {
	uint8_t peer[6];	// the sender, or for one we sent the recipient (48 bits big endian)
	bool unread, sent, cut;	// cut: the message was longer than what is kept
	uint32_t at_ms;		// hal_millis when it came or went
	char text[RADIO_M17_SMS_MAX + 1];	// UTF-8, NUL-terminated, cut on a character boundary
} radio_m17_sms_t;
uint8_t radio_m17_sms_count(void);
uint8_t radio_m17_sms_unread(void);
bool radio_m17_sms_get(uint8_t i, radio_m17_sms_t *out);
void radio_m17_sms_read(uint8_t i);
bool radio_m17_sms_delete(uint8_t i);
#endif
#if CONFIG_M17_BERT
// The BER test. mode: the BER test page holds the engine in BERT mode (PTT sends BERT frames, the counters show);
// off returns it to voice. tx: the debug link's run, sent on its own until off or the TX time-out (auto_tx HOLD).
// reset: the counters start over. Radio task (RREQ_M17_BERT; another task calls radio_req_m17_bert)
void radio_m17_bert_mode(bool on);
void radio_m17_bert_tx(bool on);
void radio_m17_bert_reset(void);
// Any task: the counters (a consistent copy)
void radio_m17_bert_get(radio_m17_bert_t *out);
#endif

// The benches (debug link op 44; no RF). Radio task (RREQ_M17_BENCH): sig signs and verifies the m17.md 7.5 vector
// on the voice path's scratch and times both into the stats (false: M17 not running, or the scratch is busy); codec
// runs m17_voice_bench's phase in mode
bool radio_m17_bench_sig(bool *ok);
void radio_m17_bench_codec(uint8_t phase, uint8_t mode);

void radio_m17_stats(radio_m17_stats_t *out);
void radio_m17_reset_stats(void);
#endif

#endif
