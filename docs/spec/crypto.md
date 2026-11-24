<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Symmetric crypto

The generic ciphers in `core/crypto`: AES with counter mode, HMAC-SHA-256 and the HMAC_DRBG generator. Any mode may
use them: M17 ([m17.md](m17.md) sections 6 and 7), and DMR AES its AES ([dmr-voice.md](dmr-voice.md) 5.1). P-256 signatures, which run on this HMAC, are in
[p256.md](p256.md). Written from the public standards in section 1 only: no library's code was read or copied.
Which radios carry the modes that use them: [feature-matrix.md](../feature-matrix.md).

| Section | Topic |
|---|---|
| 1 | Sources |
| 2 | AES and counter mode |
| 3 | HMAC-SHA-256 |
| 4 | HMAC_DRBG |
| 5 | Secrets |
| 6 | Switches and cost |
| 7 | Tests |
| 8 | The entropy pool |

## 1. Sources

| Standard | Taken from it |
|---|---|
| FIPS 197 (2001) 5.1, 5.2, appendices A to C | The AES cipher, the key expansion, the S-box, the known answers |
| NIST SP 800-38A (2001) 6.5, B.1, F.5 | Counter mode, the standard incrementing function, the CTR known answers |
| FIPS 180-4, FIPS 180-2 appendix B | SHA-256 (`core/support/sha256.c`) and its known answers |
| FIPS 198-1, RFC 2104 | HMAC |
| RFC 4231 section 4 | HMAC-SHA-256 known answers, test cases 1 to 7 |
| NIST SP 800-90A Rev. 1 10.1.2 | HMAC_DRBG: instantiate, reseed, generate, update |
| NIST CAVP `HMAC_DRBG.rsp` | A no-reseed known answer for HMAC_DRBG with SHA-256 |

## 2. AES and counter mode

`core/crypto/aes.h`, `aes.c`. The FIPS 197 forward cipher only, for 128-, 192- and 256-bit keys. Counter mode never
runs the inverse cipher, so there is none. A later mode that decrypts with AES (CBC, ECB) would add it.

| Call | Does |
|---|---|
| `aes_init(ctx, key, len)` | Expands a 16-, 24- or 32-byte key into `ctx` (10, 12 or 14 rounds, 176 / 208 / 240 bytes of round keys). Any other length, or no key: false, and `ctx` is wiped |
| `aes_encrypt(ctx, in, out)` | One block. `in` and `out` may be the same |
| `aes_ctr(ctx, ctr, data, n)` | Counter mode over `n` bytes in place (encrypting and decrypting are the same) |
| `aes_wipe(ctx)` | Zeroes the context |

Counter mode (SP 800-38A 6.5):

- Block j of key stream is AES(counter + j). `data` is XORed with it.
- The counter is the whole 128-bit block, big-endian: the standard incrementing function of B.1 with m = 128. It
  wraps from all ones to all zeros.
- `ctr` is left at the next unused value. A partial last block uses up its counter value: the next call starts on a
  fresh block, not inside the old one. A caller that cuts a byte stream keeps its pieces to whole blocks.
- A mode whose counter has its own layout builds each counter block and calls `aes_encrypt`. M17 does this: the
  nonce in META, then the frame number ([m17.md](m17.md) 6.2). DMR AES runs OFB the same way, each block the
  encryption of the one before ([dmr-voice.md](dmr-voice.md) 5.1).
- CTR gives no integrity. A flipped ciphertext bit flips the same plaintext bit.

Without a key (`aes_init` returned false, or after `aes_wipe`) the calls fail closed: `aes_encrypt` writes a zero
block and `aes_ctr` zeroes the data. A caller that missed the false sends zeros, never its plaintext. The caller still
checks `aes_init`'s result: a mode refuses an encrypted transmission without a key.

### 2.1 One table: the S-box

The only table is the 256-byte S-box (FIPS 197 figure 7), in flash. The rounds compute MixColumns with `xtime`
(multiplication by x in GF(2⁸)) instead of the 4 KB of T-tables a fast software AES keeps, or the 1 KB of one
rotated table. Flash is the scarce resource, and a mode needs few blocks: M17 asks one per 40 ms frame, DMR AES at
most 9 per 60 ms voice burst. The key
expansion computes Rcon with `xtime` too, so it needs no table of its own.

### 2.2 Timing

AES has no branch on secret data here: `xtime` reduces with a mask, the counter increment touches all 16 bytes
whatever the carry, and the round loop depends only on the key length. What varies is the S-box lookup. SubBytes
indexes the table with bytes of the state, which depend on the key, and the time of a load can depend on its
address.

| Memory under the table | Lookup time | AES time |
|---|---|---|
| No cache, the same wait states for every address | the same for every index | independent of key and data |
| A data cache (or a flash accelerator's data cache) in front of it | depends on whether the entry's line is cached | can depend on key and data |

The tree's targets have no cache on SRAM, but the S-box is a constant in flash, and both platforms enable a data
cache in front of flash: the STM32F4's flash accelerator with its data cache (`DATA_CACHE_ENABLE`), the HR_C7000's
instruction and data cache over its execute-in-place flash (`platform/hrc7000/clock.c`). So on the radios the lookup
time can depend on the index in principle. A host computer (the tests) has deep caches: there AES is not constant
time either. Cache timing attacks on table-driven AES are known on computers with shared caches.

What this means for a radio: the timing has to be measured on the radio itself, at a resolution of cycles, by code
running on it or a probe on it. That access reaches the key in the codeplug anyway (the threat model of
[p256.md](p256.md) section 10). Nothing an over-the-air observer sees depends on it: frames go out on a fixed
schedule. Two ways to close it are not built:

| Option | Cost |
|---|---|
| The S-box copied to SRAM (uncached on both platforms) | 256 bytes of RAM |
| A bitsliced S-box: logic gates, no table | more code and more cycles per block |

## 3. HMAC-SHA-256

`core/crypto/hmac_sha256.h`, `hmac_sha256.c`. FIPS 198-1 on the tree's SHA-256 (`core/support/sha256.h`, block 64
bytes, output 32).

| Call | Does |
|---|---|
| `hmac_sha256_init(h, key, len)` | Any key length: a key longer than 64 bytes is hashed first (FIPS 198-1 step 2), a shorter one zero padded. Keeps the inner state after K0 ⊕ ipad and the outer after K0 ⊕ opad |
| `hmac_sha256_update(h, data, len)` | Streams message bytes into the inner state, in pieces of any size |
| `hmac_sha256_final(h, mac)` | The 32-byte MAC. `h` is spent and wiped |
| `hmac_sha256(key, klen, data, len, mac)` | One-shot, on a context it wipes before returning |
| `hmac_sha256_wipe(h)` | Zeroes a context |

A keyed context after `hmac_sha256_init` holds the two states, not the key. A caller that MACs many messages under
one key copies the keyed context for each (a plain struct copy) instead of hashing the key blocks again: RFC 6979's
loop ([p256.md](p256.md) section 7) and the generator of section 4 can. A truncated MAC (RFC 4231 case 5) is the
caller's: it compares the leading bytes, with `crypto_equal` when the MAC guards a secret.

## 4. HMAC_DRBG

The generator of SP 800-90A 10.1.2 on HMAC-SHA-256, in `hmac_sha256.c`. No prediction resistance, no derivation
function, no additional input. Its seed is the entropy pool's (section 8).

| Call | SP 800-90A | Does |
|---|---|---|
| `hmac_drbg_seed(g, seed, len)` | 10.1.2.3 | K = 32 × `00`, V = 32 × `01`, then Update(seed). The seed is entropy input ‖ nonce ‖ personalization as one string |
| `hmac_drbg_reseed(g, seed, len)` | 10.1.2.4 | Update(seed) |
| `hmac_drbg_generate(g, out, n)` | 10.1.2.5 | V = HMAC(K, V) per 32 bytes until n are out, then Update with no input |
| `hmac_drbg_wipe(g)` | 9.4 | Zeroes K, V and the count |

Update (10.1.2.2): K = HMAC(K, V ‖ `00` ‖ data), V = HMAC(K, V); with data, again with `01`.

- `reseeds` counts the generate calls since the last seed or reseed (0 after either). SP 800-90A's reseed counter
  is this plus 1. The caller decides when to reseed and how often; the standard's limit is 2⁴⁸ requests.
- A request takes any n, including 0 (then only the update runs). SP 800-90A limits one request to 2¹⁶ bytes; the
  callers ask for a few.
- A short request is the start of a longer one from the same state: the output is V's bytes in order.
- The state is 68 bytes: K, V and the count. It is a secret (section 5).

## 5. Secrets

A key, a round key schedule, a keyed HMAC context, the DRBG's K and V, a key stream block and every intermediate of
the cipher or the MAC are secrets. Ciphertext, counters and MACs are not.

- **Where they live.** A context holding a key (`aes_ctx_t`, `hmac_sha256_t` after init, `hmac_drbg_t`) is static
  or lent memory, never a task stack: the crash record saves words at the stack pointer. The calls themselves keep
  only their locals on the stack (a state block, a key stream block, the key block, the inner hash) and wipe them
  before returning. The one-shot `hmac_sha256` and the DRBG's internal MACs use a context on the stack the same way:
  wiped before they return.
- **Wiping.** `crypto_wipe` (`crypto_wipe.h`) writes zeros through a `volatile` pointer, so the compiler keeps the
  stores. Every wipe call zeroes its whole context. `hmac_sha256_final` wipes the spent context. A refused key
  leaves an empty AES context, and a shorter key wipes the old schedule's tail. A key longer than a block is
  hashed in the inner context, which is wiped before the keyed state is set: SHA-256 keeps the key's last bytes in
  its block buffer.
- **SHA-256's schedule.** In a build with HMAC, `core/support/sha256.c` wipes its 16-word message schedule after
  each block: the schedule runs backward to the block it came from, which for HMAC is the key block.
- **Registers.** What the compiler keeps in registers or spills to the stack inside a call is not wiped. Code that
  holds a secret outside its wiped context marks the work as secret (`core/crash/crash_secret.h`): while it runs,
  the crash capture and the fault trace zero every saved general register and stack word ([p256.md](p256.md)
  section 10).
- **Never on the debug link.** No view key, log, crash record or support line carries a secret. A key is shown as
  set or not set ([channel-modes.md](channel-modes.md) section 8).
- **Comparing.** A MAC or tag checked against a computed one is compared with `crypto_equal`, whose time does not
  depend on where the bytes differ.
- **No RTOS, no heap.** The calls are plain functions over caller memory: any task may run them, one context per
  task at a time.

## 6. Switches and cost

| Switch | Builds | Turned on by |
|---|---|---|
| `CONFIG_CRYPTO_AES` | `aes.c` | an AES traffic mode (`CONFIG_M17_AES` or `CONFIG_DMR_AES`) |
| `CONFIG_CRYPTO_HMAC` | `hmac_sha256.c` and the SHA-256 schedule wipe | P-256 signatures or the entropy pool (`CONFIG_CRYPTO_P256`, `CONFIG_ENTROPY`) |
| `CONFIG_ENTROPY` | `entropy.c`, `entropy_boot.c`, the platform's hardware generator op (section 8) | M17's AES nonces (`CONFIG_M17_AES`); DMR AES walks its own MI sequence instead ([dmr-voice.md](dmr-voice.md) 5.1) |

All are derived in `config/features.mk` and `core/config.h`, never set by hand ([build-config.md](../build-config.md#switches)).
Each file is one `#if` on its own switch: off, it compiles to nothing.

Size at `-Os` (the objects alone, with the build's flags):

| Object | Cortex-M4 (Thumb-2) | CK803S |
|---|---:|---:|
| `aes.o` (256 B of it the S-box) | 1,146 B | 1,280 B |
| `hmac_sha256.o` (HMAC and the DRBG) | 628 B | 656 B |
| `sha256.o`: the schedule wipe | +16 B | +20 B |

No RAM of their own: the contexts are the caller's (`aes_ctx_t` 241 bytes, `hmac_sha256_t` two SHA-256 states, 224
bytes on Cortex-M4, `hmac_drbg_t` 68). Stack, Cortex-M4 `-Os`: `aes_init` 64 bytes, `aes_ctr` 40 + `aes_encrypt` 56;
`hmac_sha256` about 500 bytes deep and a DRBG generate under 600 (an HMAC context, the key block and SHA-256's block
function).

Time on a host computer (one core of a 3.6 GHz desktop, `-Os`): an AES-128 block about 0.6 µs, AES-256 about
0.8 µs, an HMAC-SHA-256 of 32 bytes under a 32-byte key about 6 µs (four SHA-256 blocks). Not measured on the radios
yet; a Cortex-M4 at 72 MHz is about two orders of magnitude slower, well inside one block per 40 ms frame.

## 7. Tests

`tests/host/cryptoTest.c` (`make -C tests/host`):

| Set | Source | Checks |
|---|---|---|
| AES-128, -192, -256 blocks | FIPS 197 appendix B, C.1-C.3; the last schedule word of A.1-A.3 | the cipher and the key expansion; in place |
| CTR-AES128, -192, -256 encrypt | SP 800-38A F.5.1, F.5.3, F.5.5 | one call; block by block; every length 0 to 64; calls split at odd byte counts; the counter left after each |
| Counter edges | computed | wrap from all ones; a carry through the low 64 bits |
| M17's counter layout | generated from the layout of [m17.md](m17.md) 6.2, the AES blocks with the OpenSSL CLI (M17 publishes no AES vector, m17.md 11) | FN `0000`-`0002` encrypted and the key stream of FN `7FFF`, each key length |
| Key handling | the header's contract | wrong lengths refused, the context left empty; no key fails closed; wipe |
| SHA-256 | FIPS 180-2 appendix B, the empty message | the tree's SHA-256 as HMAC links it |
| HMAC-SHA-256 | RFC 4231 test cases 1-7 (case 5 truncated to 128 bits); key edges from Python's `hmac` | one-shot, byte by byte, in growing pieces; a copied keyed context; empty key and message; a 64- and a 65-byte key; no key bytes left in a keyed context |
| HMAC_DRBG | CAVP `HMAC_DRBG.rsp`, SHA-256, no reseed, COUNT 0; an independent implementation of 10.1.2 in Python | outputs and K, V after seed and reseed; the count; determinism; a reseed changes the stream; zero-length requests; wipe |

## 8. The entropy pool

`core/crypto/entropy.h`, `entropy.c`; the boot count in `entropy_boot.h`, `entropy_boot.c`. Random bytes for values
that must never repeat, such as an AES-CTR nonce's random part ([m17.md](m17.md) 6.2). It is not used for signing
nonces: P-256 signs with RFC 6979 ([p256.md](p256.md) section 7). Built with `CONFIG_ENTROPY` (derived from
`CONFIG_M17_AES`, section 6). The radio core's task only: no lock, no RTOS call.

### 8.1 Sources and credits

Everything goes into one SHA-256 context, the pool. Only some inputs are credited: counted toward
`ENTROPY_READY_BITS` (256).

| Input | When | Credit |
|---|---|---|
| A personalization string (SP 800-90A 8.7.1) | `entropy_init` | 0 |
| The chip's unique ID (`hal_chip_t.uid`) | `entropy_init` | 0: it tells two radios apart, but anyone with the radio can read it. A platform that builds it from a part's ID reports none for an ID of one repeated byte (all 0x00, all 0xFF), which every radio with such a part would share: the HR_C7000's data flash ID (`platform/hrc7000/chip_uid.h`). Then the weak pool's no-chip-ID rule of 8.3 applies |
| The boot count and whether it was saved | `entropy_init`, `entropy_rebase` | 0 |
| 32 bytes of the SoC's hardware generator (`hal_system_t.entropy`) | `entropy_init` and every `entropy_get` | 256 for a whole read; 0 for a short one |
| The time since reset and the start-up settings write's duration (`hal_micros`) | `entropy_boot_start` | 0: they vary by some microseconds between start-ups, by an amount nobody can vouch for |
| The microsecond of each radio core tick (`hal_micros`), which interrupts and the other tasks move | `entropy_boot_poll`, every tick | 0 |
| The caller's stirring (`entropy_mix`: a time stamp, an RSSI, a block of received soft values) | when the caller has it | what the caller vouches for, at most 8 bits a byte given |

The callers' stirs today:

| Stir | Where | Credit |
|---|---|---|
| The key-up time in microseconds since start-up, before each AES nonce | `radio_m17.c`, the transmission's prepare | 2 |
| A count of the M17 receiver's soft values, when it changed | `radio_m17.c`, the radio core's tick | 1 |
| The receiver's level and noise as the status poll read them (`radio_view_t.rssi_dbm`, `noise`), XORed with the microsecond after the poll, in one word | `radio_task.c`, after every status poll (25 ms) | 0: dB steps nobody can vouch for, but they follow the air, which no other start-up hears the same |

A credited stir is also what lets a weak pool give a nonce (8.3). The level and noise are the radio core's readings,
already at hand after its poll: no hal op was added for them. The ADCs are not stirred: no `hal_system_t` op gives
their raw readings.

`entropy_source()` says what the pool has:

| Source | Meaning | `entropy_get` |
|---|---|---|
| `ENTROPY_SRC_NONE` | not started | starts the pool from nothing, fills, returns false |
| `ENTROPY_SRC_WEAK` | started, less than 256 bits credited | fills, returns false |
| `ENTROPY_SRC_POOL` | 256 bits credited, no hardware generator | fills, returns true |
| `ENTROPY_SRC_TRNG` | one whole read of the hardware generator | fills, returns true |

`entropy_credited()` counts the bits credited since start-up and saturates. The debug link shows both, never the
pool or the generator state.

### 8.2 The generator

The HMAC_DRBG of section 4, with the pool as its only seed:

- `entropy_init` hashes the inputs above, then instantiates the generator from the pool's digest.
- `entropy_get` reads the hardware generator into the pool first, where there is one. If the pool took anything since
  the last seed, its digest reseeds the generator. Then the generator fills the output.
- After each digest the pool context is wiped and started again. SHA-256 keeps the last input bytes in its block
  buffer after the final step, so a plain restart would leave them in RAM.
- The digest and the hardware generator's words pass through the stack and are wiped there. The pool and the
  generator's K and V are static, wiped by the next `entropy_init`. The STM32F4 driver keeps one word between reads
  for the continuous test (8.4), and that word never went into the pool.

### 8.3 The nonce counter

A nonce must never repeat under one key. A pool stirred for a few seconds after start-up, on a radio without a
hardware generator, can't promise that alone. So `entropy_nonce(out, n)` (n ≥ 8) ends every nonce with a 32-bit
counter that never repeats on this radio:

| Nonce bytes | Content |
|---|---|
| 0 to n - 5 | from the generator (`entropy_get`) |
| n - 4 to n - 1 | the counter, big endian: the boot count in bits 31-12, the nonces drawn since it in bits 11-0 |

Two radios that share a key differ in the random bytes: the chip's unique ID seeds each pool differently. A weak pool
with no chip ID is refused (below), since then only the counter would differ, and two radios on the same boot count
and draw would have the same one.

The boot count is kept in the settings store (`settings_boot_count`, [settings-store.md](settings-store.md) 3):

1. At start-up, after `settings_init` and before the radio core starts, `entropy_boot_start` reads it, adds 1 and
   saves it at once (`settings_flush`, in the start task, which is the one that writes the settings). Then it calls
   `entropy_init` with the board's system services, the chip ID, the count and whether the save worked.
2. A store that can't write yet (no flash, or another firmware's settings area until it is claimed) or a failed
   write leaves the count unsaved. `entropy_boot_poll`, on the radio core's tick, sees when the store has nothing
   unsaved any more and calls `entropy_rebase(count, true)`.
3. At draw 3,072 (the last quarter) `entropy_nonce_want_bump` turns true. `entropy_boot_poll` writes count + 1 into
   the settings; the settings task saves it at its next save. Once the store has it, the poll calls
   `entropy_rebase(count + 1, true)` and the draws start at 0 again. No flash write happens in the radio task.
4. A factory reset keeps the count. It stops at 2²⁴ - 1.
5. A load keeps it too. `settings_init` takes the highest count of any valid page 1 record in the ring, whatever its
   layout: the count's place (offsets 399 and 406) is the same in every layout ([settings-store.md](settings-store.md)
   3). A layout change loads the defaults with that count, and the sector of the record it came from is kept until a
   save stores the count again, so a cut save can't erase its only copy. A save cut after its page 1 leaves that
   page's higher count, which the next load takes.
6. A storage claim erases only a region that holds no record of ours (foreign,
   [settings-store.md](settings-store.md) 5), so no stored count is lost to it. The settings it saves after the
   erase are the ones the radio runs with, the count included.

`entropy_nonce` refuses (returns false, the output zeroed) when it can't promise a new nonce:

| Case | Why |
|---|---|
| 4,096 nonces drawn on this boot count | the counter's low bits are spent until the next count is stored |
| The boot count is above 2²⁰ - 1 | its top bits can't be told apart from a lower count's |
| The boot count is not saved and the pool is weak (`ENTROPY_SRC_WEAK` or `NONE`) | the next start-up may read the same count and stir the pool the same way |
| The pool is weak and no chip ID went in (`hal_chip_t.uid` unread) | two radios sharing a key differ only in the counter, which can be equal |
| The pool is weak and no credited stir came in since `entropy_init` | the random part is a fixed function of the chip ID, the boot count and the draw: a boot count that went back repeats it whole |
| Fewer than 8 bytes, or no buffer | no room for the counter |

A refused draw doesn't use up a counter value. A nonce from a weak pool with a saved boot count, a chip ID and a
credited stir is given: the counter keeps it unique on this radio while the count only goes up. An encrypting
transmitter refuses on false (`TXDENY_ENC_NOT_READY`). It stirs the key-up time (2 bits) before every draw, so the
stir rule holds from the first key-up on; the rule makes a caller that forgets it fail closed.

`entropy_rebase` never goes back: a lower count is ignored, and the same count only changes whether it is saved.

**The guarantee.** A counter value given on a saved boot count is never given again on this radio, at any later
start-up, whatever the pool. Each start-up's count is above every count saved before it, and a start-up draws each
value once. A nonce on an unsaved count comes only from a seeded pool (the table above), and its random part keeps it
apart. `settingsTest` (the layout change, a cut save, the claim) and `entropyStoreTest` (8.5) check it.

It holds while only this firmware writes the settings ring. What still breaks it:

| Event | Why the count goes back |
|---|---|
| A full chip erase, or a write of the settings area, by an external tool (a programmer, a vendor's flashing tool, an old flash image written back) | the ring loses its records, or gets older ones back |
| Another firmware run in between that overwrote the ring | no record of ours is left: the region is foreign, and after the claim the count starts again from 1 |
| A firmware whose board's flash map moved the settings area | the load doesn't find the old ring |
| Wear or damage that leaves no valid page 1 record | nothing to read the count from |

A later layout can't move the count: `settings.c` asserts its offsets, and `settingsTest` checks them. After one of
these events the count starts lower again, and only the random part keeps the nonces apart. How well it does:

| Radio | The random part after a count went back |
|---|---|
| With a hardware generator (`ENTROPY_SRC_TRNG`) | 256 fresh bits at every draw: a repeat is as likely as a 48-bit random collision |
| Without one, weak pool | Set by everything stirred since start-up. A repeat of an earlier start-up's nonce needs every input to match: the start-up times, the microsecond of every radio core tick, the receiver's level and noise at every status poll, every M17 receiver count and the microsecond of every AES key-up so far. The key-up times alone are a person's timing at microsecond resolution; once one input differs, the generator's chain differs for every later draw |

So after such an event, without a hardware generator, the guarantee is probabilistic: two start-ups on one boot
count give the same nonce only if they ran the same inputs to the microsecond. A key-up is quantized to the input
task's scan of the keys, so only 2 bits are credited for it.

### 8.4 The hardware generator

`hal_system_t.entropy` (optional, NULL: none) returns up to n bytes, 0 on a seed or clock fault, and never blocks
longer than the generator's few microseconds a word. The words are never used raw: they go into the pool. The
STM32F4 platform gives one for every board on it (`platform/stm32f4/rng.c`, RM0090 section 24):

- The RNG runs on PLL48CLK, the 48 MHz clock USB runs on, which every board on the platform sets up.
- A seed error (SECS) or a clock error (CECS) gives 0 bytes. A seed error restarts the generator, as RM0090 24.3.2
  says.
- The continuous test of RM0090 24.3.1: the first word after a start only primes the comparison, and a word equal to
  the one before it is a fault. Each read ends with one more word that is only compared, never output, so the word
  kept in static RAM between reads is never one the pool took. A fault on that word voids the read too.
- A word that isn't ready after 240 polls of the status register, about 20 µs (no PLL48CLK), is a fault.

The HR_C7000 platform has no documented generator: there the pool runs on stirring alone (8.1), and the weak pool's
rules of 8.3 apply. Which radios carry the modes that use the pool: [feature-matrix.md](../feature-matrix.md).

### 8.5 Cost and tests

| Object, `-Os` | Cortex-M4 | CK803S | RAM |
|---|---:|---:|---:|
| `entropy.o` | 780 B | 856 B | 200 B (196 on CK803S): the pool, the generator, the counter |
| `entropy_boot.o` | 328 B | 348 B | 12 B |
| `rng.o` (STM32F4) | 248 B | | 6 B |

The generator also links `hmac_sha256.o` and `sha256.o` (section 6). An `entropy_get` costs one SHA-256 final and
about 30 SHA-256 blocks of HMAC (the reseed and the generate); the tick's stir adds 4 bytes to the pool every radio
core tick, one SHA-256 block every 16 ticks, and the status poll's another 4 bytes every 25 ms. Not measured on the
radios yet.

`tests/host/entropyTest.c` (`make -C tests/host run-entropy-utc`), with a fake hardware generator and a fake
settings store:

| Set | Checks |
|---|---|
| Unstarted | no source, stirring ignored, a get fills from nothing and says so, a nonce refused |
| Credits | the ID and the boot count not credited; the cap of 8 bits a byte; ready at 256; saturation |
| Hardware generator | seeded at start-up, a read on every get; a fault at start-up, part way, an overrun |
| Inputs | the ID, the boot count, the saved flag, the generator and a stir each change the output |
| No repeats | 6,000 outputs through reseeds, with and without the generator |
| Nonce | the counter's layout, the bump at 3,072, the 4,096th draw and the refusal after it, rebase rules, sizes, a boot count past 20 bits, an unsaved count with a weak and with a seeded pool |
| Rollback | four start-ups on the same count and chip ID: refused until a credited stir; key-up times a microsecond apart give different nonces; identical start-ups give the identical nonce (the limit); one tick a microsecond late gives a different one. No chip ID: refused while weak, given seeded or with the generator. An uncredited stir doesn't count; the flag is per start-up |
| Wipes | the pool empty after each digest, K and V replaced, a second start replaces everything |
| Boot count | bumped and saved at start-up; six start-ups with bumps and a slow store, no counter value twice; a store that can't write, then is claimed; the same unsaved count twice; the end of the count |

`tests/host/entropyStoreTest.c` runs the boot count on the real settings store and a simulated flash, where the
saved state is read from the store's stats: the start-up save, the bump through the settings task's delayed save, a
worn ring that heals, a power cut at every point of the start-up save, a settings layout change and back again (the
count comes over both ways), and a foreign region refused at three start-ups, then claimed. No counter value is given
twice, and a count that gave a nonce is never read again at a later start-up.

`tests/host/stm32RngTest.c` (`make -C tests/host run-fix-core`) runs the STM32F4 driver on a scripted RNG block: a
repeated word, a seed error part way and on the spare word, and the word kept between reads never one it gave out.

Built with `CONFIG_ENTROPY=0`, `entropy.c` and `entropy_boot.c` define no symbol (the same run checks it).
