<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Settings store and safe flash writes

How the radio keeps its own state across power cycles (`core/settings`) and how the codeplug's flash sectors are
rewritten without a power cut corrupting them (`core/storage`). Both live in the board's SPI NOR. Both are generic:
the board gives the addresses in its `hal_flash_map_t` (`board.flash->map`) and core code never names a radio.

| Section | Topic |
|---|---|
| 1, 2, 3 | Areas, settings records, fields and the append-only rule |
| 4, 4.1, 5 | The sector journal, factory ranges, the foreign region guard |
| 6 | Testing |

## 1. Areas

The board's flash map (`hal/hal_flash.h`) names these areas. A board without a map runs everything from RAM: the
settings start from the defaults at every boot and nothing can be written.

| Field | Use |
|---|---|
| `settings_addr` / `settings_size` | the settings ring (2), at least 2 sectors |
| `journal_addr` | the sector journal (4), 2 sectors |
| `openfw_eeprom` | the open-firmware 64 KB "EEPROM" image (`open-firmware-cps.md` 7) |
| `openfw_flash` | the open-firmware 1 MB "flash" image |
| `openfw_end` | the end of the area an open-firmware CPS may write |
| `calibration_addr` / `_size` | the calibration working copy (the CPS window, `cps-protocol.md` 2.4); 0 when the board has none |
| `native_addr` / `native_size` | reserved for the native codeplug (`codeplug-native.md` 4) |
| `staging_addr` / `staging_size` | a new codeplug image waits here, checked, before it replaces the stored one (the TFTP write, `tftp.md` 4); never written by the open-firmware CPS (above `openfw_end`); 0 when the board has none |
| `satellite_addr` / `satellite_size` | satellite TLE data (`satellite.md` 6), apart from the staging area; 0 when the board has none |
| `factory[]` | up to `HAL_FLASH_FACTORY_MAX` (4) byte ranges of the maker's data (its factory calibration, its records): bytes no writer of ours changes (4.1). Read only on a board built with `STORAGE_FACTORY_KEEP`. Size 0 ends the list; empty on every other board. The last field of the map |

The settings ring and journal go where no open-firmware tool reads or writes, and the CPS shim refuses to write there.
Each board fixes the values in its `hal_flash_map_t`, and its hardware page lists the map. The usual order, low to
high, is the open-firmware EEPROM image, the settings ring (32 KB, 8 sectors), the journal (2 sectors), the
open-firmware flash image (up to `openfw_end`), the native codeplug (1 MB) and, on boards with room, a 256 KB staging
area. A board that keeps its calibration in the vendor layout lists no calibration window. A map assumes its chip's
size: when the probed chip is smaller than the map's highest area (`hal_flash_map_end`), the driver offers the board's
map for a smaller part if it has one and the chip holds it (`hal_flash_map_for`), else no map, and an address past the
chip fails rather than wrapping to a lower one. A map without a native area makes the factory reset skip that step. The boards are in
[feature-matrix.md](../feature-matrix.md).

The core works in 4 KB erase units (`STORAGE_SECTOR`). A board whose flash erases in other units gets no writes. A
NAND board puts `drivers/flash/nand_sectors` in between, which gives NOR behavior in 4 KB sectors
([nand-sectors.md](nand-sectors.md)).

## 2. Settings records

The settings area is a ring of sectors. Each sector holds 16 slots of 256 bytes, and each slot holds at most one
record (little endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x5445534F` ("OSET") |
| 4 | 4 | sequence number, +1 per record (compared modulo 2^32) |
| 8 | 2 | bits 0-11: layout (`SETTINGS_LAYOUT`, now 1); bits 12-15: the page (2.1) |
| 10 | 2 | payload length n (at most 236) |
| 12 | n | payload: the page's bytes of `settings_t` as it is in RAM (fixed-width fields, no padding) |
| 12 + n | 4 | CRC-32 (IEEE 802.3) of bytes 0 to 11 + n |
| 252 | 4 | commit word: programmed to 0 last |

**Load** (`settings_init`, once at boot, after `storage_init`): every slot is read. A slot is free when all its
bytes are 0xFF. It is valid when the magic, the length and the CRC match and the commit word is 0. Anything else
(a torn write) is counted in `settings.skipped` and ignored. The valid page 0 record with the highest sequence
number is in force (2.1 has the other pages). When its layout is not `SETTINGS_LAYOUT`, or no page 0 record is
valid, the radio starts from the defaults (`settings.load=defaults`). A shorter or longer payload of the same layout
fills the fields both firmwares know, and the rest keep their defaults (`upgraded`).

**Save**: the records go to the next free slots after the newest record. When a sector is full, the next sector of
the ring is erased first. Each record is programmed, then its commit word, then the slot is read back. A cut before
page 0's commit word leaves the previous set in force. A slot that doesn't read back is skipped and the next one is
tried, up to 4 slots per record. No save erases a sector that holds a page of the set in force, or a page this save
already wrote: the walk passes over such a sector to the next one. When every sector is protected that way the save
fails, and the set in force stays. Erases go round the ring: 8 sectors (every board's ring today) hold 128 records,
so with two pages a sector is erased once every 64 saves.

### 2.1 Pages

`settings_t` is longer than one slot's payload, so it is stored as pages: page k holds bytes `236 * k` on.
`SETTINGS_PAGES` is the number its size needs (2 now, 412 bytes); the page field allows 16 (3776 bytes). Each page is
a record with its own length, so `settings_t` grows by appending fields, and a short page fills the fields it has.

| Rule | Why |
|---|---|
| A save writes every page, the last first and page 0 last, each with the next sequence number | Page 0's commit makes the set the one in force. A cut before it leaves the previous set |
| Load: the newest page 0, then for each other page k the newest page k older than that page 0 | A page newer than page 0 belongs to a save that was cut |
| `loaded` only when every page is exact (page 0's sequence minus k) and full length, else `upgraded` | A firmware with one page (before pages) writes page 0 alone: the other pages come from the set before it, or their defaults |
| The next save numbers past the newest record of any page | Sequence numbers stay unique |
| No save erases a sector holding a page of the set in force (up to 32 sectors are tracked) | A page that an older firmware left behind stays readable until the next full save |

Firmware from before pages reads a page 1 record as an ordinary record of layout 0x1001, which it doesn't know, and
never sees it as newest: page 0 is always written last.

**When**: `settings_begin()` returns the live copy under a short RAM-only lock (`storage_quick_lock`) and
`settings_end(now)` releases it. If anything changed, it notes the time. No flash operation happens inside, so the
radio task may change settings. The housekeeping task calls `settings_poll()` every 100 ms. It saves once the
changes have rested `SETTINGS_SAVE_DELAY_MS` (2 s), so turning the knob through 20 channels writes one record.
`settings_flush()` saves at once: at power-off, on the CPS's "save" (`C 6 0` and `C 6 2`), and before a debug link
restart or boot stub entry (a change still in its save delay would otherwise be lost).

**Reading**: `settings_read(&copy)` takes the whole record, a few hundred bytes. A caller deep in a task's stack (a menu's
value getter, the debug link) reads one field instead: `SETTINGS_FIELD(brightness)` is `settings_read_field(offset, size)`
with the field's own type, a 1, 2 or 4 byte value copied under the same lock. A rule or a mapping the record's readers
share (`settings_theme`, `settings_crash_contact`) has an `_of` form that takes the field values.

**Who writes what**:

- The radio core (`radio_sm.c` `persist()`): VFO or channel mode, the zone, the channel number and its position,
  the VFO's frequency and settings, the privacy key in use, and `squelch` when it was changed in the VFO (squelch is
  radio-wide, so a VFO change is the default; a saved channel writes it the same way).
- The radio core, for `radio_set_settings()`: the `rs_*` fields (TOT, busy channel lockout, scan, beeps, VOX,
  battery saving, backlight times; `radio-features.md` 1). `rs_stored` 0 means the radio core's defaults.
- The radio core's data tick, for the DMR data service (`dmr-data.md`; the ARS server and IP gateway follow the
  codeplug's settings area when it has their keys, [codeplug-settings.md](codeplug-settings.md) 5): the `ds_*` fields (ARS server, radio
  check reply, call alert ACK, LRRP reply, the formats). `ds_stored` 0 means the service's defaults.
- The UI and the debug link's radio op 27: `usb_mode`, `net_flags`, `net_link_ip` (`usb-network.md` 5.4); the host
  link reads them at start and on `hostlink_settings_changed()`.
- The UI: brightness.
- The CPS shim: the VFO when a CPS writes a changed VFO A record (`cps-protocol.md` 2.3).

At boot, `apply_prefs` and `restore_vfo` read the squelch, the master power, the VFO step, the radio
name override and the privacy key. The radio name override is the talker alias text; empty means the codeplug's
callsign. The own DMR ID is always the codeplug's.

## 3. Fields and the append-only rule

`settings_t` (`core/settings/settings.h`) is the payload. The rules that keep old records readable:

1. **Fields are only appended.** A new field goes at the end, before nothing. Its default comes from
   `settings_defaults()`, which a record written by an older firmware doesn't overwrite.
2. **No padding.** Members are fixed width and ordered so the compiler adds none. `sizeof` is a multiple of 4, which
   a static assert checks. Page 1 starts at byte 236 (a field there must not straddle the page boundary; a static
   assert checks the first page 1 field's offset).
3. **Reserved bytes stay 0** in records we write. A later firmware may give them a meaning only if 0 means "as
   before".
4. **`SETTINGS_LAYOUT` is bumped only when a field moves or changes meaning.** Old records are then ignored and the
   radio starts from the defaults. Avoid this: append a new field instead.
5. 8-bit "not set" is `SETTINGS_UNSET8` (0xFF), meaning the radio's or the codeplug's default applies.

| Group | Fields |
|---|---|
| position | `vfo`, `squelch`, `zone_index`, `channel`, `zone_pos` (used when the channel number is gone after a codeplug write) |
| VFO | `vfo_rx_hz` (0 = never set), `vfo_tx_hz`, `vfo_step_hz` (0 = auto: the width's step), `vfo_mode` (bits 0-3: FM, DMR or P25 (`radio_mode_t`); a record from before the DMR VFO holds FM, a build without the DMR VFO reads DMR as FM, and one without P25, or a board that can't run it, reads P25 as its default mode), `vfo_bw` (bits 0-3), `vfo_power`, `vfo_cc` (bits 0-3, the color code), `vfo_ts` (bits 0-1, the slot), tone types and values; the DMR VFO's talk target (a 24-bit ID and a private call flag) is packed into bytes that were reserved or hold a small number, so the record stays 236 bytes and an old record reads as no target (ID 0, talkgroup 9): ID bits 0-7 in `vfo_talk_lo` (the old `reserved1`'s second byte; `accent` has the first), bits 8-11 in `vfo_mode` bits 4-7, bits 12-15 in `vfo_bw` bits 4-7, bits 16-19 in `vfo_cc` bits 4-7, bits 20-23 in `vfo_ts` bits 2-5, private call in `vfo_ts` bit 7 (always through `settings_vfo_mode` / `_bw` / `_cc` / `_ts` / `_talk_id` / `_talk_private` and the setters in `settings.h`; appended for the DMR VFO); `net_flags` sits in the VFO block's spare byte (was `reserved0`; 0 = as before): bit 0 refuses TFTP codeplug writes (`tftp.md` 4, appended for mdnstftp), bit 1 control station passthrough, bit 2 the fixed link (`usb-network.md` 5.4, appended for dispatch) |
| preferences | `brightness`, `backlight_s`, `beeps` (0 or 1), `hotspot_allowed`, `master_power`, `usb_mode` (the USB personality at power-on: 0 serial, 1 network; `usb-network.md` 1), `accent` (in the old `reserved1`'s first byte: bits 0-2 the highlight color, 0 the theme's own, 1 red, 2 orange, 3 yellow, 4 teal, 5 light blue, 6 purple, 7 pink; bit 3 set: the status strip not filled; bits 4-5 the crash report mode; bits 6-7 the vocoder's mic options, bit 6 noise reduction off and bit 7 AGC on (`dmr-voice.md` 6), so a record from before them reads as noise reduction on and AGC off; 0 = the defaults, the theme's highlight and the strip filled with it; `ui.md` 8) |
| identity | `unused_dmr_id` (was an own DMR ID override; ignored: the own ID is the codeplug's, which Settings > My radio writes), `radio_name[16]` ("" = the codeplug's callsign) |
| privacy keys | `privacy_key[4]`, `privacy_type[4]`, `privacy_active` (appended for dmr-voice) |
| radio core settings | `rs_tot_s rs_tot_warn_s rs_bcl rs_flags` (roger beep, TX inhibit, scan priority, scan beep, key beep, no talk permit tone: 0 = on, as before) `rs_scan_resume rs_scan_hold_s rs_beep_level rs_scan_delay_ms rs_scan_dwell_ms rs_vox rs_vox_level rs_vox_tail_ms rs_rx_save rs_stored rs_rx_save_idle_s rs_battery_off_s rs_backlight_dim_s rs_backlight_off_s` (appended for radio-features) |
| DMR data settings | `ds_ars_server ds_flags` (radio check reply, call alert ACK, LRRP reply, SMS format ETSI short data, radio check ETSI form, UDP compression off, UDP compression all, SMS format ETSI IP text) `ds_stored` (appended for dmr-data; the ETSI bits for dmr-ext and dispatch, 0 = as before: a record with neither SMS format bit sends TMS, while the default without a record is ETSI IP text, `dmr-data.md` 8), `ds_ip_cnf` (in the old reserved4: confirmed IP tries in bits 0-3, 0 = the default 4; bits 4-5 0 rules, 1 off, 2 all; bit 6 set: the radio check reply bit is the user's choice, clear: a record from before the reply default became On, which answers) and `ds_ip_rule[2]` (appended at the end for sap3: 0 unused, a radio ID, or 0x80000000 \| a UDP port); the two compression bits 0 = auto ([dmr-data.md](dmr-data.md) 11, 12). Page 1: `ds_ip_gateway`, the IP gateway's off-net route (`ipGatewayId`: a gateway radio's DMR ID in bits 0-23, 0 = off-net packets dropped; a record without it reads 0, as before; [usb-network.md](usb-network.md) 3.1) |
| UI settings | `rs_reply_s` (the reply window in s, 0..30; `SETTINGS_UNSET8` = the codeplug's hang times; a radio core setting), `mic_gain` (0..10, default 5 = the board's levels; `radio_set_mic_gain`, radio-features.md 3.6), `auto_lock_s` (0 = off), `long_press_ms`, `repeat_ms` (0 = the input layer's defaults), `gps` (Settings > Location > GPS: `ui_svc_apply` hands it to `board_gps_enable` and `position_gps_enable`, at start-up and on each change, see position.md 2), `pos_format`, `pos_send_s` (stored for a GPS owner, act on nothing yet) (appended for glue) |
| DMR emergency | `em_dst em_mode em_flags` (target is a radio, ETSI alarm FID, no alert tone) `em_alarm_tries em_hot_mic_s em_stored` (appended for dmr-ext, `dmr-emergency.md` 5) |
| hotspot CW ID | `cw_wpm cw_tone_hz` (0 = 20 wpm / 800 Hz; appended for dmr-ext) |
| DMR roaming | `roam_rssi_dbm roam_hyst_db roam_probe roam_stored roam_check_s` (the radio core's roaming settings, taken when `roam_stored` is 1) and `roam_zones[10]` (bit z: zone z is a roaming zone, zones 0..79; written by the radio core on RREQ_ROAM) (appended for roaming, `dmr-roaming.md` 5.1) |
| more UI settings | `theme` (bits 0-1: 0 dark, 1 light, 2 high contrast, 3 night; firmware from before night reads 3 as dark), `ui_flags` (large text, hint row, message tone, talker alias shown, PTT when locked, hear all by default, 0x40 vibrate off, 0x80 `SETTINGS_UI_DUAL_TALK_BACK` dual watch talk back; default hint row, message tone, talker alias and PTT when locked on, and the vibrate-off and talk back bits clear, so vibrate is on and talk back is off), `updown_home` (0 channel, 1 zone, 2 talk target; bit 7 `SETTINGS_HOME_LABEL_FIRST`, Home's talk line with the talkgroup's name first, 0 in older records: the number first), `low_batt_pct` (5..30, default 15; 0 = the radio core's voltage levels only), `auto_off_min` (0 = never) and a reserved u16, now `aprsis_port` (appended for uifix; a record without them keeps these defaults) |
| roaming survey | `roam_beacon_s` (the repeaters' beacon interval, 0 = none, else 10..600, default 60), `roam_beacon_len_s` (1..10, default 4), `roam_survey` (0 off, 1 synced, 2 per site; default 1), `rpt_beacon` (bit 0: our repeater roles' idle beacon, default 0; bits 1-2: an SFR user's receive mode `sfr_rx`, 0 repeater only (default), 1 first copy, 2 dual-path RX, `sfr.md` 4; bits 3-5: the SFR repeater's CW ID interval `sfr_cwid`, 0 off (default), 1 5 min, 2 10 min, 3 15 min, 4 30 min, `sfr.md` 4; a record from before reads 0), `aprs_power` (APRS's RF power, `aprs.md` 11; 0 = the radio's current level, which a record from before it reads as), `aprs_cp_path` (what was last taken from the codeplug APRS record,
`aprs.md` 11: bits 0-2 the path into `aprs_path`, bits 3-7 the SSID into `aprs_ssid`, each + 1, 0 = none yet; through
`settings_aprs_cp_path` / `_ssid`, so a record from before the SSID reads none), `volume` (the speaker volume of a radio without a knob; in the old `reserved6`, so a record from before it reads as the default. 0 = the default step; bit 7 set = `0x80 | (step + 1)` for step 0..16 (0 = mute); 1..11 = step + 1 of the first version's 0..10 range, read as step 0..10 scaled to 0..16 (round(step * 16 / 10): the default 5 is 8, 10 is 16) and rewritten in the new form at the next save; `radio-features.md` 4.2) |
| APRS | `aprs_flags aprs_flags2` (TX, SmartBeaconing, compressed, after PTT, boot, KISS, narrow, RX; iGate, iGate TX, messaging, UDT, to APRS-IS, DMR GPS Info, mute) `aprs_region aprs_ssid aprs_freq_hz aprs_lat aprs_lon aprs_dmr_dst aprs_alt_m aprs_interval_s aprs_distance_m aprs_symbol[2] aprs_path aprs_pos_src aprs_level aprs_txdelay aprs_misc aprs_stored` and a reserved u16, now `snmp_rf_gap_s`; taken when `aprs_stored` is 1 (appended for aprs, `aprs.md` 11) |
| position | In the APRS block, owned by the position service (`position.md` 5): `aprs_pos_src aprs_lat aprs_lon aprs_alt_m`, the DMR GPS Info flag in `aprs_flags2`, `aprs_misc` bits 4-6 (ambiguity). Taken when `aprs_misc` bit 7 (`SETTINGS_APRS_MISC_POSITION`, the position's own stored marker) is set or `aprs_stored` is 1. A record written before the marker has bit 7 clear and reads as before |
| APRS-IS client | `aprsis_ip` (the server's IPv4 address, 0 = off; appended for tcp), `aprsis_port` (0 = 14580; in the old `reserved5`), `aprsis_range_km` (the filter `m/<km>`, 0 = none; in the old `reserved3`) (`tcp-services.md` 7) |
| control station | `net_link_ip` (the radio's fixed link address, 0 = 192.168.10.1; in force with `net_flags` bit 2), 0 = off, the link by DMR ID (appended for dispatch after `aprsis_ip`, `usb-network.md` 5.4) |
| SNMP agent | `snmp_read_hash snmp_write_hash` (FNV-1a hashes of the communities, 0 = "public" / none; appended for snmp after `net_link_ip`), `snmp_rf_manager[3]` (the RF manager's DMR ID, little endian, 0 = none; in the old `reserved2[3]`), `snmp_flags` (in the old `reserved4`), `snmp_rf_gap_s` (0 = 60; in the APRS block's old `reserved7`); all zero = the defaults (`snmp.md` 6) |
| dual watch | `ui_flags` bit 7 (0x80) is talk back (Settings > Talk > Dual watch talk back; off = the bit clear, as in a record from before). Page 1: `dual_b`, the last B: 0 none, 1..1024 a codeplug channel number, 1025 the first VFO, 1026 the second. The UI looks the channel number up again at power-on, in A's zone first, then in the first zone that holds it; if it is gone, the picker opens ([dual-watch.md](../ux/dual-watch.md) 5). `dual_state` bit 0 (`SETTINGS_DUAL_ON`) says the watch was on: the UI starts it again at power-on, with `dual_b` as the channel that wasn't selected and bit 1 (`SETTINGS_DUAL_B_SEL`) set when B was the selected one |
| second VFO | Page 1: `vfo_index` (the VFO in use at power off, 0 or 1) and `vfo2_rx_hz` (0 = never set), `vfo2_tx_hz`, `vfo2_talk` (the DMR talk target: ID in bits 0-23, bit 31 private call), tone values and types, `vfo2_mode`, `vfo2_bw`, `vfo2_power` (`SETTINGS_UNSET8` = default), `vfo2_cc`, `vfo2_ts`: the first VFO's fields, unpacked. Dual watch gives each channel its own VFO ([radio-features.md](radio-features.md) 2.1). A record without page 1 reads as the first VFO in use and no second VFO frequency |
| dual watch wait | Page 1: `rs_dual_delay`, `radio_settings_t.dual_delay_ms` as n - 1 steps of 100 ms (1 = no wait, 11 = 1 s); 0 (never set, or no page 1) = the default, 1 s. Settings > Talk > Dual watch delay |
| broadcast step | Page 1: `rs_bcast_step`, `radio_settings_t.bcast_step` + 1 (1 = 200 kHz on the odd tenths, 2 = 100 kHz); 0 (never set, or no page 1) = the default, 200 kHz. Settings > Talk & listen > FM broadcast step ([airband-and-broadcast.md](airband-and-broadcast.md) 4.4) |
| zone journal | Page 1, appended for the zone editor: `zone_jrn_kind` (0 none, 1 add, 2 delete), `zone_jrn_index` (the index added at or deleted from), `zone_jrn_count` (the stored zone count before it, 0 with only the built-in zone). Written before a zone write that moves zone indexes and cleared in the record that moves `zone_index` and `roam_zones` to match; set at power on, the change is finished from the zone list (`zone_edit_settle`, [ui.md](ui.md) 2.2). 0 (no page 1, an older record) = none |
| language | Page 1, after the zone journal (was reserved): `lang`, the UI's language as its two ISO 639-1 letters (`es`), English `en`; both 0 = never picked, which reads as the build's default language (`CONFIG_LANG_DEFAULT_<CODE>`, else English). A language this image wasn't built with reads as English ([ui.md](ui.md) 4) |
| receive past the band table | Page 1: `cp_flags` bit 1 (`SETTINGS_CP_RX_EXT`), [radio-features.md](radio-features.md) 6.2. Cleared by a factory reset. |
| codeplug acceptance | Page 1, after `lang` (was reserved): `cp_flags` bit 0 (`SETTINGS_CP_ACCEPTED`), the user chose Use anyway for another firmware's codeplug ([codeplug-native.md](codeplug-native.md) 2). Cleared when a CPS or TFTP write of the codeplug commits and by a factory reset. 0 (an older record) = not accepted |
| zone sharing | Page 1: `zs_offers` (zone shares accepted: 0 the default and what an older record reads as, On; 1 On, as the old Ask; 2 Off. A record from before On became the default reads On whatever it stored), then `ref_trim_ppb` (an int16, the reference trim in ppb, -10000..+10000, `radio-features.md` 6.1; in the old `reserved8`, so an older record reads 0, no trim) and `ds_ip_gateway` (DMR data settings). `zs_import[4]` (appended, after the zone journal): the last four zone imports, newest first, each a key (an FNV-1a hash of the sender's ID and the zone ID; 0 unused), the 16-byte name the zone got (not terminated at 16), `sig` (the XOR of the written channels' hashes) and `count` (how many it wrote), then three reserved bytes. Settings > Connections > Zone offers ([zone-share.md](zone-share.md) 5.1, 5.4) |
| volume knob | Page 1, appended after `zs_import`: `sound_flags` (was `knob_flags`) bit 0 (`SETTINGS_KNOB_LOCK`) ignores the knob while the keys are locked, bits 1..3 reserved (0), bits 4..7 the Settings > Sound tones (below); then the VFO SFR bytes, `power_levels`, `t3_stun`, `t3_kill` and `satellite_index`. 0 (an older record) = the knob works. Settings > Key lock > Knob when locked, shown inverted (On = bit clear), on a radio whose MCU reads the knob |
| Settings > Sound tones | Page 1, `sound_flags` bits 4..7 (`settings_tones_off`): `radio_settings_t.tones_off`, the radio's own copy of the tone keys of the codeplug's settings area ([codeplug-settings.md](codeplug-settings.md) 3, 5.2). Bit 4 call end, 5 TOT warning, 6 battery, 7 TX denied: set = the tone is off. 0 (an older record) = every tone on. Firmware from before them changes only bit 0, so it keeps them |
| VFO SFR role | Page 1, after `sound_flags` in the old `reserved9[0..1]`: `vfo_sfr` (the first VFO) and `vfo2_sfr` (the second), each byte A of a codeplug channel's SFR bytes ([sfr.md](sfr.md) 4: role, framing, inbound slot, hang); role bits 0-2 0 = none, as an older record reads. No byte B: the VFO's outbound color code is its own. Written with the VFO, set by RREQ_VFO_SFR and Copy to VFO; then `power_levels` |
| power level count | Page 1, after `vfo2_sfr` (was `reserved9`): `power_levels`, the board's power level count (`board_t.power_count`) that `vfo_power`, `vfo2_power`, `master_power` and `aprs_power` count against. A level is an index into the board's levels, so when a board's levels change the same index names another power, possibly a higher one. A record whose count isn't the board's, an older record's 0 included, loads with those four back to their defaults (not set, and `aprs_power` 0, the current level) and the board's count; the next save stores it |
| Tier III stun | Page 1, after `power_levels` at offset 396: `t3_stun`, a stun kept across a restart ([dmr-trunking.md](dmr-trunking.md) 7.9). Bit 7 (`SETTINGS_T3_STUN`) set: stunned on the codeplug system in slot bits 4-6, whose network ID and model hash to bits 0-3. Written by the radio core when a stun is taken or revived. 0 (an older record) = not stunned. Cleared by a factory reset |
| Tier III kill | Page 1, offset 397: `t3_kill`, bit 0 (`SETTINGS_T3_KILLED`) a kill taken ([dmr-trunking.md](dmr-trunking.md) 7.9): no DMR at all, in any build, until a factory reset clears it; bits 1-7 reserved (0). 0 (an older record) = not killed |
| Satellite selection | Page 1, offset 398 (`reserved10[0]`): `satellite_index`, the installed TLE record index selected in Tools > Satellite; 0 selects the first record. Older records read as 0, not `SETTINGS_UNSET8`. The selection is saved when changed. TLE records and frequency pairs are stored separately ([satellite.md](satellite.md) 5) |
| signaling tones | Page 1, offsets 400-405: `sig_dmr[3]` and `sig_fm[3]`, where the start, end and idle tone sound in DMR and in FM (0 default, 1 off, 2 speaker, 3 on air, 4 speaker + air). Offset 399 remains reserved (`reserved10[1]`); offsets 406-407 are `reserved11[2]` (0). Older records read these as 0, the radio's own tone. The chains of steps are not here: each event's is its codeplug record (`CODEPLUG_KEY_SIG_BEGIN` + event, [codeplug-settings.md](codeplug-settings.md) 3.3), read at start and on every change, so `settings_t` copies on task stacks stay small. `radio_settings_t.sig_dmr`, `sig_fm`, `sig_hz`, `sig_ms` ([radio-features.md](radio-features.md) 4.3) |
| DMR AES MI reservation | Page 1, offsets 408-411, appended after `reserved11` (a record from before reads 0): `aes_mi_res`, u32, the end of the MI positions reserved for our AES overs ([dmr-voice.md](dmr-voice.md) 5.1.1). 0 = no position yet: the first AES channel with a key seeds one. Written between overs at first use after power-up and renewed as the reservation runs low. A restore while the radio runs can overwrite it; the radio writes back the in-memory end between overs. A restart before that save or a factory reset can still cause keystream reuse |
| crash report | packed into spare bits, so an older record reads as off with no target: the mode (0 off, 1 ask, 2 send at once) in `accent` bits 4-5; the target's codeplug contact number (0 none, else 1..1024, 11 bits) bits 0-5 in `theme` bits 2-7 and bits 6-10 in `updown_home` bits 2-6. `theme` and `updown_home` keep their value in bits 0-1; read and write them through `settings_theme` / `settings_updown_home` and `settings_crash_*` (`settings.h`). Firmware from before reads a `theme` or `updown_home` with these bits as out of range: the dark theme and Up/Down = channel ([crash-log.md](crash-log.md) 5.2) |

All of these are still layout 1: they were appended before any firmware with the settings store shipped. Page 0
is full: its later fields went into reserved bytes where 0 already meant
"as before" (`net_flags`, `snmp_rf_manager[3]`, `aprsis_range_km`, `snmp_flags`, `aprsis_port`, `snmp_rf_gap_s`, `aprs_power`, `aprs_cp_path`, `volume`, `vfo_talk_lo`), or the new defaults (`accent`: the filled status strip, a new look on purpose). No reserved byte is left. One-byte
fields that hold small numbers (`hotspot_allowed`, `usb_mode`) still have free upper bits (`theme` and `updown_home` hold the crash report's target, `updown_home` bit 7 the talk line order;
`accent` bits 6-7 are free); `vfo_power` does not (its "not set" is 0xFF). New settings are appended to page 1 as
plain fields; a page 2 follows when page 1 is full (2.1).

### 3.0 Who writes, and storage jobs

The settings are written through `settings_save` (debounced, written by `settings_poll` on the housekeeping task in
`app.c`). Other flash writes that a screen asks for (save to channel, the permanent talk target, factory reset) are
storage jobs: `storage_job_post(fn, ctx)` hands a function to the housekeeping task, which runs it from
`storage_job_poll` right after `settings_poll`. There is one slot: a second post while a job is pending is refused
(the UI says so), and `storage.job_busy` on the debug link shows a pending job. A job runs with no UI lock held, so
the UI keeps drawing during the flash write; the job reports back through `ui_notify`.

Factory reset (a job): `settings_defaults` into the live copy and save it at once, then `storage_native_erase`.
That erases the native area only when its first word is the native magic `STORAGE_NATIVE_MAGIC` (0x4E52444F, bytes 4F 44 52 4E)
or when it is blank already; anything else is reported as foreign and left alone. The open-firmware codeplug, the
calibration and the foreign-guarded regions (section 5) are never touched; a reset of those would need its own
second confirmation and is not offered.

### 3.1 Privacy key slots (the hook for DMR basic privacy)

The key store for `core/dmr/dmrPrivacy.c` (`dmr-voice.md` 5). System traffic keys use the codeplug settings area
([channel-modes.md](channel-modes.md) 5.4). Four slots, each a key value (`privacy_key[i]`,
u32: 16 bits are used today by Motorola and Anytone basic privacy, and the slot is wide enough for a 32-bit key
later) and a type (`privacy_type[i]`, `dmrPrivacyType_t`). `privacy_active` picks the slot in use radio-wide: 1 to
4, or 0 for privacy off. Turning privacy off keeps the key in its slot.

Today the radio core writes slot 1 when the key is set (the debug link's `RADIO` privacy op) and loads the
active slot at boot, checking it with `dmrPrivacyKeyIsValid`. A per-channel choice of key will be a slot number in
the native channel record (`codeplug-native.md` 3: 0 = the radio-wide choice, 1 to 4 = that slot). The keys
themselves stay in the settings and never go into a codeplug file, so a codeplug can be shared without its keys.

**Open: the keys are stored in plain text in the SPI NOR.** Anyone with the radio and a flash reader, or with the
CPS shim's area 1 reads, can see them. The settings area is outside what the open-firmware CPS reads by region, but area 1
reads any address. Options: refuse area 1 reads of the settings area, or obfuscate with a key from the MCU's UID.
Neither stops a determined reader, and basic privacy is weak anyway.

## 4. The sector journal

A codeplug edit, or a CPS commit, rewrites a whole 4 KB sector: erase, then program. A cut between the two would
leave the sector erased: channels, zones or contacts gone. The journal (`core/storage/storage.c`) makes the rewrite
repeatable. It uses two sectors at `journal_addr`:

| Sector | Content |
|---|---|
| J0 | header, 16 bytes: magic `0x4C4E4A4F` ("OJNL"), target address, CRC-32 of the copy, CRC-32 of the first 12 bytes; then a "done" byte at offset 16 (0xFF = pending, 0x00 = done) |
| J1 | the new content of the target sector |

`storage_sector_write(addr, data)`:

1. If the target already holds exactly `data`, nothing is done (`storage.sector_skips`).
2. Erase J1, program the copy, read it back and check its CRC.
3. Erase J0, program the header.
4. Erase the target, program `data`, read it back.
5. Program the done byte to 0.

`storage_init()` at boot, before the settings and the codeplug are read: a J0 with a valid header and a pending
done byte means a cut after step 3. If J1's CRC matches, steps 4 and 5 run again (`storage.replays`). A cut before
the header is complete leaves the target untouched, because the header is the commit point. A cut during the replay
is replayed again at the next boot.

`storage_write(addr, buf, len)` writes any range (the codeplug editor's records, names and bitmaps). Open-firmware
channel, zone and contact records don't divide 4 KB, so some cross a sector end; two separate sector rewrites would
leave such a record half old, half new after a cut between them (a channel with the new frequency and the old
timeslot). So a write of up to 4072 bytes, which touches one or two sectors, is one transaction:

| J0 offset | Content |
|---|---|
| 0 | magic `0x324E4A4F` ("OJN2"), then the address, the length and the CRC-32 of bytes 0 to 11 and the data |
| 16 | marks, 0xFF = not yet: first sector staged, first sector done, second sector staged, second sector done |
| 20 | all done |
| 24 | the data |

1. If the flash already holds exactly the data, nothing is done.
2. Erase J0, program the header and the data, read them back (the commit point).
3. For each sector: J1 erased and programmed with the sector merged with the data, "staged"; the sector erased and
   programmed from J1, read back, "done".
4. Program "all done".

Replay: a header with a good CRC and "all done" pending finishes the sectors not done. A sector not staged has not
been erased yet, so it is merged with the data from J0 again; a staged one is copied from J1. A longer write
(none of the core's callers makes one) goes sector by sector through `storage_sector_write`. The bytes are merged
in the shared 4 KB buffer, which is shared with the CPS shim's pending sector (`storage_buffer_acquire`), so an edit
while a CPS write is going on fails cleanly instead of waiting.

A flash that is copy on write underneath offers `hal_flash_t.replace` (a NAND flash, `nand-sectors.md` 5): the
sector rewrite then calls it and checks the result. There is no journal on such a flash (J0 and J1 stay blank), because
the replace is one atomic record, and the replay at boot finds nothing to finish. The patch transaction of
`storage_write` (up to 4 KB across one or two sectors) still uses the journal.

Wear: J0 and J1 are erased once per sector rewrite. A full qdmr write is about 46 sectors, so the journal sectors
take 46 erases per write. At 100,000 cycles, that is about 2,000 full codeplug writes. Spreading the journal over a
ring is possible later. The map has room after 0x01B000.

**Locks**: `storage_lock()` is a recursive FreeRTOS mutex around every flash sequence of the core: the codeplug
calls, a settings save, a CPS commit. `storage_quick_lock()` guards only RAM: the live settings copy. The radio task
takes only the quick lock on its own, so it never waits for an erase. A codeplug read from the radio task (a channel
change) does take `storage_lock` and may wait out a sector rewrite (about 100 ms) during a CPS write.

A run of writes (a zone import, a CPS commit) takes and gives `storage_lock` once per record, from a task above the
waiters. A give alone only makes a lower-priority waiter ready, and the writer takes the lock again before it runs, so
the waiter would wait out the whole run (6 s for a zone import on an SPI NOR radio). The last give therefore yields
one tick when a task is waiting: each waiter waits one record at most.

### 4.1 Factory ranges

A board whose maker keeps data of its own on the data flash (a factory calibration, records the maker's tools read)
lists those bytes in `hal_flash_map_t.factory` and builds with `-DSTORAGE_FACTORY_KEEP=1` (`MCU_DEFS` in its
`board.mk`, as `STORAGE_CLAIM_FIRST`; the default 0 is in `hal_flash.h`). A range may lie inside an open-firmware
image and share its sectors with codeplug bytes. Which boards list any: [feature-matrix.md](../feature-matrix.md) and
each board's hardware page.

Without the define the helpers `hal_flash_factory_unit()` and `hal_flash_factory_bytes()` are constant false and
nothing below is compiled: core/storage, the satellite store and the STM32 flash glue are the code they were before the
ranges existed. Only the data grew, by the map's `factory` field and the `hal_flash_t.factory_grant` pointer, each the
last field of its struct, so no other field moved. `storageJournalLogTest` (6) holds the journal's flash operations to
that.

| Term | Means |
|---|---|
| factory bytes | the bytes of the listed ranges |
| kept sector | a sector holding factory bytes (`hal_flash_factory_unit(map, STORAGE_SECTOR, ...)`) |
| whole kept sector | a kept sector of factory bytes only |

**The list's checks.** `storage_init()` checks the list before it reads anything. A list that fails leaves the
region none (`storage.region=none`): nothing of the core is written anywhere, because a wrong list says the map is
wrong. One log line says so.

| Rule | Why |
|---|---|
| the ranges of size other than 0 first, then entries of all zeros | size 0 ends the list |
| ascending and apart (at least one byte between two ranges) | adjacent ranges are one range; one form for every list |
| inside the chip | |
| every sector a range touches clear of the settings ring, the journal, the calibration window and the native, staging and satellite areas | those areas are erased whole |
| at most 4048 factory bytes (`STORAGE_SECTOR` - 48) in a kept sector that also holds other bytes; a whole kept sector has no limit | the journal's J0 carries a kept sector's factory bytes beside its header when that sector is rewritten |
| no `replace` op on the flash | a copy-on-write flash replaces a sector in one step from one buffer |

**The kept rewrite.** A kept sector stays writable: every byte of it that isn't a factory byte belongs to its
writer. Every rewrite of one carries the factory bytes over from the flash and takes the rest from the writer. The
writer's bytes for a range are dropped, the write succeeds and the drop is counted, so a codeplug tool that writes
whole sectors from another radio's image keeps working. A whole kept sector is never erased or programmed: a write to
it succeeds and changes nothing. The rewrite is a journaled sector write (4) whose entry carries the factory bytes
twice before the target is erased:

| Sector | Content |
|---|---|
| J0 | the sector header and its done byte, as in 4; then a kept block: at 32 magic `0x4B4E4A4F` ("OJNK"), at 36 the target, at 40 n (the sector's factory bytes in the map), at 44 the CRC-32 of bytes 32-43 and the n bytes; at 48 the sector's factory bytes in address order (the kept stream) |
| J1 | the merged sector: the writer's bytes with the factory bytes in place |

J1 is merged a 256-byte chunk at a time (the chunk from the writer, the factory spans of that chunk read over it from
the target), so no second sector buffer is needed: the writer's data is often the shared buffer itself.

| Step | A cut here leaves | The next start |
|---|---|---|
| 0 compare the target with the data outside the factory bytes: equal, or a whole kept sector, is a skip | nothing written | (true, `sector_skips`; `factory_dropped` when the factory bytes differed) |
| 1 erase J1, program the merged sector, check its CRC | J0 as it was (a done entry, or blank), J1 partial | nothing to replay; the target as it was |
| 2 erase J0 | J0 blank or partly erased, J1 whole | nothing to replay (no header); the target as it was |
| 3 program the kept stream at J0 + 48 (read from J1); read the target's factory bytes again and compare them with it; program the block's 16 bytes at J0 + 32, read back | a partial kept block, no header | nothing to replay; the target as it was |
| 4 program the header at J0 + 0: the commit point | a torn header | nothing to replay (header CRC); the target as it was |
| 5 open the grant for the target, erase it, copy J1 into it, read back, close the grant (on every exit) | the target erased or partial | the replay: the target := J1 with the kept stream over it (the same bytes), read back, done |
| 6 program the done byte | | the replay finds the target equal: done |

Three reads agree before any erase: the target read into J1, J1 read into the kept stream, the target read once more.
A difference in step 3 (a marginal read) stops the rewrite before the block's header is programmed, so J0 holds no
valid block and no commit point: false, `factory_refused`, the target untouched. The grant is open only between the
target's erase and its read back.

**Every writer.**

| Writer | On a kept sector |
|---|---|
| `storage_sector_write` | the kept rewrite; a skip when nothing outside the factory bytes changes, and on a whole kept sector |
| `storage_write`, a record of up to 4072 bytes | a record touching a kept sector, whether or not it reaches factory bytes (a DTMF or APRS record), takes the sector path: sector by sector through `storage_sector_write`, so the kept rewrite. The patch transaction (4) would erase the sector from one read of it, the factory bytes in J1 only. A record crossing a sector end into or out of a kept sector is then two sector writes, not one transaction: a board's map should keep its records clear of that (its hardware page says whether one crosses) |
| `storage_write`, the sector path and an erase (`buf` NULL) | per sector through `storage_sector_write`: an erase leaves the factory bytes |
| the replay at `storage_init` | the table below |
| settings ring, journal, claim, native erase | their areas are clear of every kept sector (the checks) |
| every erase and program of `core/storage` (`wr()`, `er()`) | refused in a kept sector but the one granted (`factory_refused`, also in `storage.errors`): a path that misses a rule above, or a read that failed between a check and a write, never reaches the maker's bytes |
| `storage_staging_size()`, the satellite store | an area touching a kept sector is none (each is also asked for apart from `storage_init`) |

The CPS shims, the fast codeplug protocol and the TFTP codeplug write reach the flash through `storage_sector_write`
(`codeplug_kv_commit`, `copy_sector`), so their commits and boot resumes keep the bytes too.

**The replay** at `storage_init`, of a pending entry in J0:

| J0 holds | Replay |
|---|---|
| no valid header, or the entry done | nothing (as in 4) |
| a sector entry whose target holds no factory bytes, or a patch entry whose sectors hold none | as in 4 |
| a sector entry whose J1 doesn't match the header's CRC (a partly erased J0 can keep an old header) | the done byte, nothing copied, nothing converted (as in 4). J1 is read a second time first: one marginal read must not give up an entry whose target may be erased |
| a sector entry for a whole kept sector | the done byte only: nothing of J1 lands |
| a sector entry and a valid kept block (its target and n those of the map) | the target := J1 with the kept stream over it, unless it holds that already; under the grant, read back, done |
| a sector entry, the kept area (J0 32 to 48 + n) blank: another firmware's entry, or one from before the rule | converted first: the kept block written from the target as it is now, compared with a second read, read back; then the row above. If that firmware had already erased or partly programmed the target, what it left is what is kept (the factory copy's restore brings the bytes back). The entry's bytes for the range in J1 never land |
| a sector entry, the kept area torn, or a valid block for another target | left pending, `factory_refused`, one log line, nothing written. Only a cut conversion (the target not erased since: the block comes before any erase) or a map changed between versions leads here. The next journaled write replaces the entry |
| a patch entry for a kept sector whose data misses every factory byte (only an earlier firmware makes one) | as in 4, the kept sector under the grant: an unstaged sector is intact and merged again, a staged J1 was merged from the intact sector |
| a patch entry whose data reaches factory bytes | left pending, `factory_refused`, one log line. No writer of this firmware makes one |

**The scan** (5) knows the new J0 shapes, on a map that lists ranges only: a kept block (valid, or torn by a cut while
it was programmed) after a valid sector header or after blank bytes 0-31, and a valid kept block after a header on its
way (a cut while the header was programmed). A kept block is never looked for in a patch entry (its data starts at 24).
So J0 never counts a foreign slot at any cut of a kept rewrite; J1 counts one only as for any journaled write (written
while J0 is blank, 5).

**The held journal.** A kept rewrite that fails after its commit point (a bus error after the target's erase, not a
power cut) leaves its entry pending, and the target may be erased: J0 and J1 then hold the only copies of the factory
bytes. Storage remembers it in RAM: every later write of that session (a sector write, a patch, either path of
`storage_write`) first runs the replay, and is refused while that fails (`factory_refused`, `storage.errors`; the
caller sees false, a CPS a refusal). A write that would be a skip is refused too: the held entry can be that very
sector's, whose erased factory bytes a compare would take as written, and whose replay would later undo a write skipped
against it. A held patch entry is not replayed while
the shared buffer is taken (its replay merges there). The next start replays the entry as after any cut. The same hold
follows a replay of an entry for a kept sector that fails once it may have erased the target. Only boards that keep
factory bytes hold the journal: elsewhere a failure past the commit point leaves the target torn until it is written
again, as it always has.

**A downgrade.** An earlier firmware sees a plain sector header and replays J1 as it is, and J1 is merged, so the
factory bytes survive it too. Its scan counts the kept block after the done byte as one foreign slot; the region stays
ours (its settings records).

| Counter (`storage_stats_t`) | Counts |
|---|---|
| `factory_rewrites` | kept rewrites done |
| `factory_dropped` | writes whose bytes for a range differed from the flash: dropped (the first of a start logged: `storage: kept 0x......-0x......, a write's bytes there dropped`) |
| `factory_replays` | entries for a kept sector finished at a start or by a held journal, converted ones included |
| `factory_refused` | entries left pending, kept rewrites stopped by a second read that differed or a read that failed, writes refused by a held journal, `wr()`/`er()` refusals |

The debug link shows them as `storage.factory=rewrites,dropped,replays,refused`, only on a board that keeps factory
bytes (`debuglink.md` 3.7).

**Driver backstop.** A flash driver may offer `hal_flash_t.factory_grant(unit)`: it then refuses every erase and
program in an erase unit holding factory bytes, nothing sent to the chip, except inside the one unit core/storage
granted for a rewrite that carries the bytes over (`UINT32_MAX`, or any address that doesn't start a unit, closes it).
Core/storage opens it for the target of a kept rewrite or a replay, from just before its erase to its read back,
under the storage lock; a whole kept sector is never granted.

| Driver | Backstop |
|---|---|
| STM32 SPI NOR glue (`platform/stm32/spi_flash`) | under `STORAGE_FACTORY_KEEP`: the units holding factory bytes of the board's map and of its small map (whichever the chip holds, or neither); with `SPI_NOR_ADDR4` also every address whose low 24 bits land there, which a slip to 3-byte addresses would reach, granted or not. Counted (`stm32_spi_flash_factory_refusals()`), the first one logged |
| HR_C7000 platform | its own write floor (`hrc_flash_cfg_t.write_floor`) |
| others | none |

**Reads.** The debug link's STORAGE READ also takes a span inside one factory range (`debuglink.md` 3.13), on a list
`storage_init()` accepted, in a foreign region too (before a claim), so a host can compare the maker's bytes with a
backup.

**Not protected:** a byte the maker keeps that the board doesn't list, and anything a tool writes past the firmware
(a direct chip write).

## 5. Foreign region guard

The flash of a radio that ran other firmware may hold that firmware's data where our settings ring and journal go.
Before anything is erased or programmed
there, `storage_init()` classifies the region:

| Part | Ours | Anything else |
|---|---|---|
| settings slot | all 0xFF; a valid record; our magic with a torn rest; a save cut inside the magic (the bytes so far on their way to "OSET", the rest 0xFF) | other bytes |
| J0 | all 0xFF; our header (magic "OJNL"), nothing after the done byte; our patch header ("OJN2"), nothing after its data; on a map with factory ranges also the kept rewrite's shapes (4.1) | other bytes |
| J1 | all 0xFF, or anything once J0 holds our header | non-blank while J0 is blank |

**Foreign** = some part holds other bytes **and** no valid record of ours (a settings record or a J0 header with
a good CRC) exists anywhere in the region. Then:

- `storage_init()` returns false, `storage_ready()` is false, nothing is replayed.
- Every erase and program in the region is refused. `storage_sector_write` and `storage_write` fail
  (`storage.foreign_refusals`), so codeplug edits and CPS commits fail too (the journal is the region). The CPS
  shim refuses `X 1` (`cps-protocol.md` 2.3).
- `settings_init()` returns `SETTINGS_FOREIGN` and the radio runs on the defaults in RAM; changes are kept in RAM
  and not saved.
- The debug link's view shows `settings.load=foreign`, `storage.region=foreign`, `storage.journal=foreign` and
  `storage.foreign_slots` (slots and journal sectors with other bytes; 0 on a blank flash that is claimed first,
  below).

Junk beside a valid record of ours (a torn erase of our own ring) is not foreign: the slots are skipped and
reused once their sector comes round.

**Ownership mark.** When the region is ours but holds no valid record (a blank flash), `storage_init()` programs
one valid settings slot of layout 0 and sequence 0 (no payload) in the first free slot. The settings load their
defaults past it (no settings layout is 0) and the first real save gets sequence 1. From then on a valid record
always exists, because saves never erase the sector with the newest record, so a power cut inside a journal
rewrite (J0 erased, J1 full) can't make the region look foreign at the next boot.

**Claim first.** The rule above takes a blank region at the first start, and with the mark every codeplug and CPS
write anywhere in the map is allowed. That is right when the other firmware's layout is known: its data is where
the map says. It is wrong for a flash that held another firmware's data in a layout nobody knows. There a blank
region says nothing about the megabytes around it, and the first start would open them to writes before a backup
exists. A board with such a flash builds with `-DSTORAGE_CLAIM_FIRST=1` (`MCU_DEFS` in its `board.mk`; the default
is 0), and its hardware page says so. A region with no valid record of ours is then foreign, blank or not:

| Region at start | Default | `STORAGE_CLAIM_FIRST` |
|---|---|---|
| a valid record of ours, anything beside it | ours | ours |
| other bytes, no valid record of ours | foreign | foreign |
| all blank | ours, the mark is programmed | foreign |
| a torn mark and nothing else (a power cut inside the claim) | ours, marked again | foreign, to be claimed again |

- Everything listed for a foreign region holds, on every unit, from the first start. Every writer in the core asks
  `storage_ready()` (settings, codeplug edits, CPS commits, TFTP writes), so the core erases and programs nothing
  anywhere on the chip. `tests/host/storageBootTest.c` (built as `storageBootTest` and `storageBootClaimFirstTest`)
  checks that the two start-up resumes, whose erases bypass `storage.c`, leave a foreign or unclaimed chip untouched. `storage_init()` never writes the mark. A board's own writes to the chip (a board record
  sector) don't pass through `storage_ready()`: on such a board they need their own rule.
- The claim (below) is the only way in. It erases the region and writes the mark, so the next start finds a valid
  record and the region ours. It opens the whole map to writes, so the backup to take first is of the whole flash.
  The firmware can't check that one exists. It tells the clients instead: `STORAGE INFO` carries a claim-first flag
  (`debuglink.md` 3.13), and each client claims on such a board only after its own backup of the whole chip, read
  completely, or not at all (below).
- Once claimed, the region stays ours for the reason the mark gives: a valid record always exists. A region that
  lost its last record would be foreign again and the radio would save nothing until a second claim.
- A valid record proves one claim, not what happened since. A flash that other firmware wrote to after the claim is
  still ours at the next start if a record of ours survived in the region.

The switch is a define and not a field of the flash map, so a board that doesn't set it carries no code or data
for it: with 0 the condition is the default one.

**Claim.** Only through the debug link, behind the chip UID token like REBOOT (`debuglink.md` 3.13). Two clients
send it:

- The web radio programming utility, right after a backup of everything, when `STORAGE INFO` reports the region
  foreign and the backup holds the whole SPI flash (a read that stopped early claims nothing). The Codeplug Editor
  checks before every write and, on a foreign region, explains and claims only when the user confirms. Both poll
  INFO until the region is ours (20 s limit).
- `rfoundry_debug.py storage claim --out DIR`, as listed below.

On a claim-first board (INFO's flag) the Codeplug Editor never claims: it tells the user to take the utility's
backup of everything, which claims after it. The utility's restore of channels from a backup bundle doesn't claim
either (a bundle is an older state, maybe of another radio), and says the same. The other clients claim only where
the other firmware's layout is known: the utility's channel step after it recognized a known codeplug in the chip
(`prepareStorage` with no whole-chip backup), and the migration tools for the radios they serve (`migrate.js`
`writeConverted`). On a claim-first board they claim nothing, write nothing, and point to the backup of everything
(`flash.storage.claim_first`).

1. `rfoundry_debug.py storage claim` reads the settings ring, the journal and the calibration copy through the debug
   link's read-only SPI flash window (twice, compared) into `--out DIR` (refused inside any git
   work tree), then sends the claim with the token. On a claim-first board it first reads the whole data flash
   through the CPS shim (`cps-protocol.md` 2.2, area 1; the size from the CPS radio info), each 32 KB block twice and
   compared, into one `dataflash_*.bin` in the same directory, and stops before the claim when a block is refused,
   short or read differently (no partial file is left). `--skip-full-backup` leaves that read out.
2. The firmware notes the request (`storage.claim_pending`). The housekeeping task (`settings_poll`) erases every
   settings sector and both journal sectors, checks they read blank, marks the region ours and writes the
   ownership mark, then saves the settings the radio is running with.
3. The tool polls until `storage.region=ours`.

`rfoundry_debug.py storage dump` saves the same files without claiming; `storage info` shows the state and addresses.

## 6. Testing

| Test | What |
|---|---|
| `tests/host/settingsTest.c` | load, save, debounce, ring wrap, layout change, older and newer payload sizes; pages (a page 0 alone from an older firmware, a page newer than page 0, a save that passes over a sector holding an old page); power levels stored against another level count reset; a power cut at every flash operation of a save (the previous or the new set is in force, never neither, and never pages of two saves) |
| the journal part of `settingsTest.c` | a power cut at every one of the 701 flash operations of a sequence of sector rewrites, then a boot: every sector holds either its old or its new content, and the replayed ones the new |
| `tests/host/sim/sim_flash.c` | the simulated NOR: program only clears bits (violations counted), erase per 4 KB, a power cut after N operations (a torn program or erase leaves random bits) |
| `tests/host/radioCoreTest.c` `test_persist` | the radio core saves the zone, the channel and the VFO and comes back to them after a reboot |
| `tests/hil/test_cps.py` `test_settings_saved` | on the radio: a channel change is saved within 2 s (the debug link's `storage` view section, `debuglink.md` 3.7) |
| `settingsTest.c` `test_foreign` | another firmware's bytes in the ring / J0 / J1: foreign, no flash operation at all, writes refused, RAM defaults, the read window's limits; claim erases and saves; junk beside our record, our J0 header beside junk, a cut inside a journal rewrite after the mark, a cut inside the first mark: ours |
| `cpsTest.c` `test_foreign` | a foreign region: reads answered, `X 1` and codeplug edits refused, flash unchanged; after a claim, CPS writes work |
| `tests/host/storageClaimFirstTest.c` | the store built with `STORAGE_CLAIM_FIRST=1`: a blank flash, a blank region with other data around it and a region with other bytes are all foreign, with no program or erase on any sector, every write refused and counted, RAM defaults; the claim touches the region only and leaves the mark, and the next start is ours; our journal header alone is ours and nothing is marked; a power cut at every flash operation of the claim and the save after it (ours once the mark is whole, else foreign with nothing written at that start, and a second claim works); once claimed, a cut at every operation of the saves that erase a ring sector (two laps of a two-sector ring) and inside both kinds of journal rewrite never makes the region foreign |
| `tests/host/storageJournalLogTest.c` | the journal's flash operations pinned: a scripted run on a map with no factory ranges (sector writes and the skip, patches inside a sector, across a sector end and of the largest size, the sector path, an erase, the native area's erase, a foreign region and its claim, `storage_init` on every J0 and J1 shape of the format, a power cut at the start, one unit in, the middle and the end of every program and erase of each, then a start and one more write), every operation, result, stats and the flash hashed against a golden value taken from the storage.c before the factory ranges. Built as every board builds storage.c and again with `STORAGE_FACTORY_KEEP=1` (`storageJournalLogKeepTest`): one hash |
| `tests/host/storageFactoryTest.c` | `STORAGE_FACTORY_KEEP=1`, a map with a range inside a sector, two whole kept sectors and a range across a sector end, on the simulated NOR counting erases and programs per sector and refusing those in a kept sector outside the grant: every rule of the list's checks (region none, not one read, program or erase, one log line) and lists that pass at each limit; `replace` with a list refused; `hal_flash_factory_unit` and `_bytes` at the edges; `storage_sector_write` with other bytes, with only factory bytes differing (a skip, dropped), with the radio's own factory bytes, on a whole kept sector (nothing touched), J0's kept block and the merged J1; `storage_write` of a record in a kept sector (the sector path, under the grant), reaching factory bytes, across into a whole kept sector, an erase, the sector path and an erase of everything; a power cut at the first, second, middle and last unit of every erase and program of a kept rewrite (the journal blank before, and holding another kept entry; the range as filled, all 0xFF and all 0x00), then a start: the factory bytes the stored ones, the rest of the sector all old or all new, the region ours, J0 never a foreign slot, the journal usable at once; a misread in the merge and in the second read (stopped before the commit point, nothing to replay); the held journal (a program failure after the target's erase, then every write refused, a skip too, the journal unchanged, until the failure is lifted; the held sector itself written; across a start; with the shared buffer taken; an earlier firmware's patch entry held at the start, not replayed while the shared buffer is taken); the replay of every J0 shape (this firmware's entry, also with a misread of J1, an earlier firmware's verbatim replay of it, another firmware's entry with the target intact and torn, a cut conversion, a block for another target or length, a whole kept sector, stale headers, a patch entry clear of factory bytes unstaged and staged, one reaching them); a power cut at every operation of the replays that write, for the three fills; a claim; the debug read inside a range; the staging and satellite areas. Then the STM32 glue on a fake SPI NOR driver: the backstop with and without a grant, a grant off a unit's start, both maps; built again with `SPI_NOR_ADDR4` (`storageFactoryAddr4Test`): the address 16 MB up, a grant of it included |
| `tests/hil/test_cps.py` `test_storage_region` | on the radio: the region state matches the view, reads inside the window work and outside it are refused (skips with instructions when foreign) |
| `tests/hil/test_cps.py` `test_settings_survive_restart` | opt-in `--allow-reboot`: the channel position survives a restart |
