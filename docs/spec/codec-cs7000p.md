<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# CS7000 M17 PLUS: hosting the AMBE+2 codec

Status: format description of one stock file, against the reference build ([codec-placement.md](codec-placement.md),
"STM32 radios"). **Untested on a radio**: every result below is from static analysis of that file or from a host
instruction set emulator (2026-11-22). The hosting is written and untested on a radio: [Hosting](#hosting).

How a fact here is known:

| word | means |
|---|---|
| measured, static | read from the stock file: bytes, hashes, addresses, what the code at an address references |
| emulated | a result of running the carved code on a host emulator ([Verification on a host](#verification-on-a-host)) |
| inferred | a name or a purpose given to something by how it is used. Not a measurement |

Each section says which it is. Anything about the radio itself comes from the linked hardware and platform pages or
from the ARMv7-M architecture, not from a run.

Rule: the radio runs exactly the codec its own Connect Systems firmware shipped. The user's machine extracts it from a
firmware file the user supplies. No codec bytes go in this repository or in released binaries.

The HR_C6000 has no vocoder of its own ([modem-hrc6000.md](modem-hrc6000.md) 1): it carries the 72-bit frames, and the
MCU runs the codec in software. On this radio that is the codec inside the maker's DMR program.

## Firmware files

The maker publishes its files at `https://www.connectsystems.com/amateur-files/software/CS7000%20M17%20PLUS/`. The
"HOST" file is the maker's DMR program, linked at 0x08020000 behind the maker's bootloader.

| version | file | size | sha256 | codec, range A | state |
|---|---|---|---|---|---|
| **V9.00.96** | `M17_PLUS_HOST_V9.00.96_20260303_1.bin` | 1758526 (0x1AD53E) | `1a4bdf38fed52dc50c738a85f8ce54f6384c070c6a08af21ca919a547b7e5a59` | 0x080A4F24 | this page |
| V9.00.95 | `M17_PLUS_HOST_V9.00.95_20260226.bin` | 1750688 | `690392a8930a19145db66b813b3dd75e69cd729da23c302442c7a6d4d8a7392c` | 0x080A4188 | not analyzed |
| V9.00.09 | `M17_PLUS_HOST_V9.00.09_20250110.bin` | 1763250 | `347293b12682eb017a82a03278232d68d608a42d02ce78e2d4c36f9d3a49eda0` | 0x080AE684 | not analyzed |
| V9.00.05 | `M17PLUS_HOST_V9.00.05_20241029.bin` | 1763124 | `e16bf6f4eccde779a77b10300b7da2d40e4fa996b70962bf7380691a444c7a1e` | 0x080AE284 | not analyzed |

* Only V9.00.96 is a codec source. The older files use the same container and key (measured: header fields, the two
  copies, the version text). Each holds the same codec code: compared outside the reference sites, range A and both
  islands are equal to this build's in all three (but for one of the maker's constants inside A), and range C in
  V9.00.95.
* The older files do not have this build's ranges. In all three, range B is in two parts, split at this build's
  0x080AF140 and 28 bytes further apart. In V9.00.09 and V9.00.05, range C is in two parts around the maker's piece
  at 0x080B9274, 3968 bytes further apart. So this build's lengths, hashes and site lists do not apply to them. Their
  table windows are not known: each needs its own analysis.
* The image carries its version as the text `V9.00.96` at 0x080F5138.
* A tool recognizes the file by its SHA-256 and refuses any other.

## Container and encryption

| file offset | contents |
|---|---|
| 0x00..0x7F | header, not encrypted. u32 LE at 0x14 = 0x001AD4BC, the payload length. u32 LE at 0x44 = 0x80, the payload offset. The other nonzero bytes are [0x02] = 0x0D, [0x6D] = 0x01 and [0x6F] = 0x18; their meaning is not known |
| 0x80..0x1AD53B | payload, 0x1AD4BC bytes, XORed with a 256-byte key |
| 0x1AD53C..0x1AD53D | two bytes (0x22, 0x7B) outside the header's length; role not known |

```
plain[i] = file[0x80 + i] ^ key[i & 0xFF]
```

The plain payload:

| payload offset | contents |
|---|---|
| 0 | u32 LE N = 0x000D6A56 (879190), the image length |
| 4 | the image, N bytes |
| 4 + N | u32 LE N again |
| 8 + N | the same N bytes again |
| 8 + 2N | two zero words |

* 2N + 16 is the header's payload length.
* The image is linked at 0x08020000 and ends at 0x080F6A56. Its SHA-256 is
  `9bcd6e63d85cea5c7dda9f41466902bd54d097e5b3ecdbeacd8f49dbf91573d1`. Its vector table holds SP 0x2403B238 and reset
  vector 0x080F50C9.
* The two copies are byte-identical. Why there are two is not known.
* For an image address A: file offset = 0x84 + (A - 0x08020000) in the first copy, 0xD6ADE + (A - 0x08020000) in the
  second. Key index = (file offset - 0x80) & 0xFF. The second copy's offset is the first's plus 0xD6A5A.

### Key recovery

The key is 256 bytes, SHA-256 `72315db3eff46e6856a1ca214236828b04cea9ee0f60bbd837dbfde71b0177c0`, first bytes
2D 33 42 45. It is never stored in the repository. A tool derives it from the stock file and checks it by that hash.

Two ways from the file alone, both checked:

1. **Column vote.** For each key column c, take the most common byte among file[0x80 + c + 256 m]. That is the key
   byte in 253 of the 256 columns. In columns 69, 185 and 195 the key byte is the second most common. No column has a
   tie for first place.
2. **The two copies.** The same plain byte is in the file twice, 0xD6A5A bytes apart, so under two key columns that are
   0x5A apart. For every i below N, file[0x84 + i] XOR file[0xD6ADE + i] = key[(4 + i) & 0xFF] XOR
   key[(0x5E + i) & 0xFF]. That gives key[j] XOR key[(j + 0x5A) & 0xFF] for every column j, without error. 0x5A is
   even, so the even columns form one chain of 128 and the odd columns another, and one byte fixes each chain. Take
   for each chain the byte whose 128 derived key bytes have the highest total count in their columns. The result is
   the key.

Two checks beyond the hash:

* The decrypted image holds the key itself, once, at 0x080F128C (file offset 0xD1310 in the first copy).
* The maker publishes its bootloader B7.06 twice: plain, as a DfuSe file (`M17PLUS_Bootloader_B7.06_20241214.dfu`, one
  element at 0x08000000, 0xD9FC bytes), and in this container (`M17PLUS_Bootloader_B7.06_20241214.boot`, the payload
  at 0x80). The XOR of the two is the key repeated over all 55804 bytes. That file's header has its length at 0x18
  and its offset at 0x48, not at 0x14 and 0x44: a tool must not take it for a HOST file.

## Codec in the V9.00.96 file

The codec is not in one code window and one table window, as the reference build's is. The maker's linker spread it:
five code ranges and 20 table windows between 0x080A4F24 and 0x080F59B3, with the maker's own code and data between
them. Every address in this section is where the file holds the codec.

This section is measured from the file (static), except where a line says a run read or did not read something:
those are emulated. The names of the pieces ("API layer", "Golay routines", "byte fill", "exit stub", "shift
helpers") are inferred from how the code uses them.

Code, 58404 bytes. S = 0x080A4F24, the start of range A, is the code start that the entry offsets count from.

| range | flash | length | file offset | key phase | first word | sha256 (decrypted) | CRC-32 |
|---|---|---|---|---|---|---|---|
| A, the API layer | 0x080A4F24..0x080A5F50 | 0x102C (4140) | 0x84FA8 | 0x28 | 0x4FF0E92D | `463d78c02a2df849673298196e289f07320409292a172b28db1504cd9d8af721` | 0xAB040EFA |
| B | 0x080AC100..0x080AF964 | 0x3864 (14436) | 0x8C184 | 0x04 | 0xB4F02A00 | `40b3504f6983755d29f7b779e1be28c9f608c3dab9fb06e0e0aa93d2501e3da0` | 0x5178B85B |
| C | 0x080B1980..0x080BABBC | 0x923C (37436) | 0x91A04 | 0x84 | 0xF04FB510 | `7afa06ee8611fef6dc99dc4967de0a70e97fe2a2caea1173753560c08d0a5c03` | 0xB3D8AC6F |
| island 1 | 0x080D9A10..0x080DA284 | 0x874 (2164) | 0xB9A94 | 0x14 | 0x0FF0E92D | `739982dc4456bea1a8ce808981dfc2bd164d0f31bc06e7de000e2fc464da6f09` | 0x441AECCF |
| island 2, the Golay routines | 0x080F1768..0x080F184C | 0xE4 (228) | 0xD17EC | 0x6C | 0x03F0E92D | `731a14b3839ad491b6477b88605cb76f8c3e983cd87606254777ef3ed4d528a3` | 0xE7772689 |

Tables, 13483 bytes in 20 windows:

| window | flash | length | file offset | key phase | sha256 (decrypted) | CRC-32 | sites | addresses referenced (offsets in the window) |
|---|---|---|---|---|---|---|---|---|
| T1 | 0x080C469C..0x080C4710 | 0x74 (116) | 0xA4720 | 0xA0 | `20aff10a1ab504491ad4f24ddfe6509b1392e12d1858f664daca436166333f8e` | 0x37C09F0A | 2 | 0x0 |
| T2 | 0x080CE9E0..0x080CFC80 | 0x12A0 (4768) | 0xAEA64 | 0xE4 | `b1b5830c22bc96bdf789ba3e31d5f04a0ba0e9f69fe483276fcb807dd41a3118` | 0xC0986844 | 7 | 0x0, 0x40, 0xC0, 0x140, 0x240, 0x640, 0x1240 |
| T3 | 0x080D0D28..0x080D1D5C | 0x1034 (4148) | 0xB0DAC | 0x2C | `a0b2e233da9493eee3c52eb11f8f9dd170e521456f8bad8c53e46fb277ffbb60` | 0xA4C2D0F5 | 14 + 5 | 0x0, 0xC00, 0x1004, 0x1014, 0x101C, 0x1028 |
| T4 | 0x080DF228..0x080DF688 | 0x460 (1120) | 0xBF2AC | 0x2C | `b1ad3ce2792698bc6dc02aa406645f2c396c798b5aa946e1091970cd04d5735e` | 0x876760C7 | 2 | 0x0, 0x50 |
| T5 | 0x080E0770..0x080E0B30 | 0x3C0 (960) | 0xC07F4 | 0x74 | `11d823bd776976cad9b1ef621fdf2e996199dd2a909c9b08e95282dd807eaec3` | 0xF570C0E9 | 1 | 0x0 |
| T6 | 0x080E9ECC..0x080EA0CC | 0x200 (512) | 0xC9F50 | 0xD0 | `d721117ac5b980a5784d209d11df9b049c832ed7b300500ac8a246ddb95c1a71` | 0xAD039873 | 2 | 0x0 |
| T7 | 0x080ED098..0x080ED260 | 0x1C8 (456) | 0xCD11C | 0x9C | `49ee00e5cbe72c87e92bceff45a69e65acafc305fc9b9fa56cecc9c2bd8fb297` | 0xE8729DE7 | 2 | 0x0, 0xC8 |
| T8 | 0x080F0974..0x080F0A8A | 0x116 (278) | 0xD09F8 | 0x78 | `1c95cee56c2cbb35f2babaa39f13627f4aa10c0cdbc3ddc0ae431e7917a35c35` | 0x31E94EC2 | 2 | 0x0, 0xE4 |
| T9 | 0x080F148C..0x080F158C | 0x100 (256) | 0xD1510 | 0x90 | `df7b43d5d94331aa558573f3c2532efb13d889f35db49fd0330b91e1618ebdb8` | 0x12BAF84E | 7 | 0x0 |
| T10 | 0x080F1924..0x080F19FC | 0xD8 (216) | 0xD19A8 | 0x28 | `6f758b1089bc524dea3b5eea17814d9d27eb4ea0dafd83f3fd7750dc5f253232` | 0x1F5C5FE3 | 3 | 0x0, 0x48, 0x90 |
| T11 | 0x080F1E84..0x080F1F26 | 0xA2 (162) | 0xD1F08 | 0x88 | `a861d8a6c3cef15d7af5ffbb41054844e37cad763b0232e491cf532e432f7707` | 0x64DFF1E9 | 1 | 0x0 |
| T12 | 0x080F21DC..0x080F2244 | 0x68 (104) | 0xD2260 | 0xE0 | `39a89b07c20513d1e1acddd368a535c088df2aad0878d77c1a98a0b0e63297fe` | 0xAB927A87 | 3 | 0x0, 0x3C, 0x48 |
| T13 | 0x080F22A8..0x080F230C | 0x64 (100) | 0xD232C | 0xAC | `16b9306bfbbefa54b84599bdb59972079c7a06dd2e6fb648bf0bbb37e33bca27` | 0xDED2357C | 2 | 0x0, 0x14 |
| T14 | 0x080F2814..0x080F285C | 0x48 (72) | 0xD2898 | 0x18 | `163b99fc4146ab87620d3563e8560ab960c9f6c8c948c9279e5ab9e60e8da65d` | 0x7BB34BBB | 12 | 0x0, 0x8, 0xC, 0x14, 0x18, 0x20, 0x24, 0x2C, 0x30, 0x38 |
| T15 | 0x080F2B64..0x080F2BA4 | 0x40 (64) | 0xD2BE8 | 0x68 | `48c753062c1dec3ea8e84e977d6cc5ff90e556e28da221ca5d9b4cb9c5f0613d` | 0xC52B1734 | 1 | 0x0 |
| T16 | 0x080F3638..0x080F3664 | 0x2C (44) | 0xD36BC | 0x3C | `d31f87f8b0f589ac3bc5dc52e1ecd628c0eeb1cd4c38ee8a4e63321e3446190f` | 0x8DB58A40 | 1 | 0x0 |
| T17 | 0x080F4674..0x080F4694 | 0x20 (32) | 0xD46F8 | 0x78 | `124bb7ae360bd4e6ab8226cc7024804223fb6aa10c3981ea288af423fcc790fb` | 0x091306AB | 4 | -0x2, 0xC, 0x18 |
| T18 | 0x080F4B5C..0x080F4B74 | 0x18 (24) | 0xD4BE0 | 0x60 | `6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375` | 0x33E82E70 | 1 | 0x0 |
| T19 | 0x080F4E04..0x080F4E18 | 0x14 (20) | 0xD4E88 | 0x08 | `0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017` | 0xFFF0E930 | 1 | 0x0 |
| T20 | 0x080F5994..0x080F59B3 | 0x1F (31) | 0xD5A18 | 0x98 | `cadd4de2615a7d73588a2a450d1765dda3d655123ed61cab2c099af019024154` | 0xE14AB240 | 6 | 0x0, 0x8, 0x18 |

* Code and tables together are 71887 bytes in 25 windows. Nothing else of the image is needed.
* File offsets are the first copy's. Key phase is (file offset - 0x80) & 0xFF.
* "sites" counts the places in the code that hold an address inside the window: in code the entries reach, plus in
  code they don't reach. [Reference sites](#reference-sites) lists every one.
* All 25 windows carve from both copies of the stock file to the hashes above.
* Range A's first word is 0x4FF0E92D (`push.w {r4-r11, lr}`), the signature word of the STM32 stock codecs.

What the windows hold:

* **Ranges A, B and C.** The codec library. A holds the entry points. The maker's own code lies between them:
  0x080A5F50..0x080AC100 (25008 bytes) and 0x080AF964..0x080B1980 (8220 bytes). The codec never enters it. A to the
  end of C as one block is 0x080A4F24..0x080BABBC, 0x15C98 (89240) bytes, SHA-256
  `a5efac225921575c7482c0e077cd321221730178b66eebe65b2913760b88feee`, CRC-32 0xF267FAF7.
* **Islands.** Two pieces of codec code that the linker placed among the tables. Island 1 is reached through a code
  pointer and calls back into B. Island 2 is reached only from FEC decode, through a code pointer: by that use it
  holds the Golay routines of the codec's own FEC.
* **Shift helpers.** Range C ends with the compiler's two 64-bit shift helpers (0x080BAB74..0x080BAB96 and
  0x080BAB98..0x080BABBA). The codec calls them; nothing else does.
* **Not the codec's, inside the ranges.** Two pieces of the maker's code sit in holes: a byte fill at
  0x080A583C..0x080A5876, which the maker's voice code calls for its own buffers, and an exit stub at 0x080B9274 (its
  hole runs to 0x080B92C0). The codec calls neither. They are carried only to keep the ranges contiguous.
* **Three two-byte constants of the maker's**, which its linker put in the padding after a function: at 0x080A5876
  (after the byte fill), 0x080BAB96 (after the first shift helper) and 0x080BABBA (after the second: the last two
  bytes of range C). A maker's routine (0x080989E4) holds their addresses in the literal words at 0x08098B24,
  0x08098B54 and 0x08098B58, among 43 such constants all over the image. The traced run neither read nor executed
  them. The one at 0x080A5876 has another value in V9.00.09 and V9.00.05. They are carried so that the ranges stay
  contiguous and C keeps a length that is a multiple of 4; the hashes above include them.
* **Not reached.** Of the 58404 bytes, the ten entries and the three code pointers reach 55506 bytes of code (192
  functions) and 292 bytes of literal pools. The other 2606 bytes are padding (NOPs), unused library functions that
  call only into the ranges, the two functions only stock calls (104 bytes), the two maker's pieces and the three
  constants.
* **No constants in the code.** Inside the ranges the code reads only its literal pool words and one 4-byte branch
  table (0x080ADD7A, relative: it needs no rewrite).
* **Tables.** The code reaches its constants only through absolute addresses, 52 distinct ones. No table holds an
  address: no word in a table points into any of the 25 windows (static), and tables moved without a change inside
  them gave the same output (emulated). No reference points into the middle of an object.

How each bound is known:

* **Starts.** The lowest address the code holds for the window. No run read below a start. T17 is the one exception
  in form: the words at 0x080A54E8 and 0x080A5BC0 hold 0x080F4672, two bytes below the window, because their readers
  step before they load. The two bytes there are not the codec's and are never read.
* **Ends, by what follows.** The maker's code or data starts right at the end of T1, T2, T4, T5, T6, T7, T9, T10, T12,
  T13, T14, T16, T17, T18 and T19, two bytes after T15 and one byte after T20. Four zero bytes follow T3.
* **Ends, by the reading code.** T8's two objects each carry their own entry count: the second ends at 0x080F0A8A,
  and two zero bytes follow. T11's last entry ends at 0x080F1F26, and two zero bytes follow. T16's reader loads bytes
  40 to 43 on every call: 44 bytes, not 40. T20's last object is indexed 0 to 6: 7 bytes. T10 is three objects of 72
  bytes.
* **T3.** Its first object is 3072 bytes with one regular structure to its end. The runs read only part of it.
* **T4.** Its second object is 1040 bytes and ends at 0x080DF688, where a maker's function starts.
* **T5.** The 64 bytes before it look like the same kind of data. Nothing addresses them, and T5's reader only walks
  forward from the start. They are not the codec's.
* **Not the codec's, inside the windows, and never read:** 0x080F2220..0x080F2223 (T12), 0x080F22BA..0x080F22BB
  (T13), 0x080F59A4..0x080F59AB (T20).
* A window may be rounded up to a multiple of 4 bytes: T8 and T11 then take two zero bytes each, T20 one byte of the
  maker's. The hashes above are for the exact lengths.
* Read by no run: the first object of T10 (72 bytes), T1 beyond its first 36 bytes, T4's second object beyond 640
  bytes. Their bounds rest on what follows them.

## Entry points and calling convention

All AAPCS, Thumb (call with bit 0 set), arguments beyond four on the stack in order, results in r0. The offsets are
from S = 0x080A4F24.

The addresses and argument lists are read from the stock image's own calls (static), and stock's init routine was
run in the emulator with the arguments logged at each entry. Return values, argument meanings, sizes and flags are
emulated. All of it is untested on a radio. Where an argument below is a number that stock keeps in a variable, the
number is the one stock's init routine stores there ([What stock passes](#what-stock-passes)).

| call | address | offset | arguments (r0, r1, r2, r3; then the stack words in order) | r0 on return |
|---|---|---|---|---|
| encode | 0x080A5070 | +0x14C | 49 bits out, 0, PCM in, 80; flags, half, 0x2000, E | half 0: 0; half 1: a status word |
| encoder set | 0x080A5104 | +0x1E0 | E, 128, -20 | 0x4000; stock ignores it |
| encoder init | 0x080A51B8 | +0x294 | E, 1, 1 | 0 |
| FEC init | 0x080A5420 | +0x4FC | F, 1 | 23 |
| encode ECC | 0x080A5438 | +0x514 | 72 air bits out, 49 bits in, 0, F of the encoder side | 72 |
| decode | 0x080A5528 | +0x604 | PCM out, 80, 49 bits in, 0; flags, half, D | half 0: a status word; half 1: 0 |
| decoder set | 0x080A55D0 | +0x6AC | D, 128, -20 | 0x4000; stock ignores it |
| decoder init | 0x080A5614 | +0x6F0 | D, 1 | 0 |
| encoder parameters | 0x080A5778 | +0x854 | E, -22, 20, -10; -40, 0x4000, 3276, 3276, 1310, 10, 20 | none |
| FEC decode | 0x080A5A04 | +0xAE0 | 49 bits out, 0, 72 air bits in, 1; flags, F of the decoder side | 0, 4, 8 or 12 |

Two more that only stock calls, after every FEC decode:

| call | address | offset | arguments | does |
|---|---|---|---|---|
| FEC result copy | 0x080A5F20 | +0xFFC | record out (0x34 bytes), F | copies the first 0x34 bytes of an FEC state; returns 0 |
| FEC result to decoder | 0x080A54EC | +0x5C8 | D, record | copies the record to D+0x618..0x64C |

* E is the encoder state block, D the decoder state block, F an FEC state block
  ([State sizes](#state-sizes-measured-in-an-emulator)).
* The library is the one of the other STM32 stock codecs. The boards that build `platform/stm32/hrc/vocoder.c` use
  two sets of entry offsets; this build has the set with encode at +0x14C. The eight entries that host calls (encode,
  decode, the three inits, encoder parameters, the two sets) are at exactly those offsets, with the same argument
  lists and state sizes. FEC decode, which that host does not call, is at +0xAE0 here.
* The only calls into the five ranges from the rest of the image go to these twelve entries, the byte fill and the
  exit stub (static: every BL and B in the image). No data word outside the ranges holds a code address inside them.
  The only words that point into them are the three that name the maker's two-byte constants
  ([above](#codec-in-the-v90096-file)).

### Frames

* PCM: signed 16-bit, 8 kHz, 160 samples per 20 ms frame, in two calls of 80 (half 0, then half 1). Encode only reads
  its PCM.
* Codec frame: 49 bits, one per 16-bit halfword, each exactly 0 or 1, without FEC. Other nonzero values are not
  taken as 1.
* Air frame: 72 bits, one per halfword, in transmit order: bit 7 of byte 0 first when packed into 9 bytes.
* The codec's own FEC is the standard AMBE+2 3600x2450 one. Encode ECC gave the same 72 bits as an FEC written from
  the description in `core/dmr/ambe/ambeFec.h` on every random frame tried (300 in one run, thousands in another).
  So a host does the FEC in `core/dmr/ambe` and needs only encode and decode.
* The AMBE silence frame (B9 E8 81 52 61 73 00 2A 6B) goes through FEC decode with result 0, re-encodes to the same
  9 bytes and decodes to silence.

### Arguments

| argument | meaning | a host passes |
|---|---|---|
| sample count | 80 at every stock call. Other counts are not established | 80 |
| half | 0 for the first 80 samples, 1 for the second. Encode writes the 49 bits in the half 1 call. Decode reads them in the half 0 call and rewrites its bit buffer | 0, then 1 |
| the 0 in encode r1, decode r3, encode ECC r2, FEC decode r1 | the number of frame bits given up. With k = 1..6 the same k air positions change in every frame and FEC decode returns the first 49 - k bits. 7 faults. In stock each is a variable ([below](#what-stock-passes)) | 0 |
| encode flags (stack word 0) | [Encode flags](#encode-flags). One value on both calls of a frame | below |
| encode stack word 2 | the noise suppressor's strength, used only with flag 0x40. 0 suppresses most, 0x4000 and above least. Stock passes 0x2000 | 0x2000 |
| decode flags (stack word 0) | 0 for a good frame. 4 or 8: the decoder ignores the bits (status 0x20). Stock passes the FEC decode result ORed with a variable of its own ([below](#what-stock-passes)) | 0 for a frame its own FEC accepted |
| FEC decode r3 | bits per input symbol: 1 for hard bits. 2 to 4 take soft values | 1 |
| FEC decode flags (stack word 0) | with 4 or 8 set nothing is decoded and 4 is returned | 0 |

* FEC decode returns 0 for a good frame and 4 when C0 could not be corrected. 8 and 12 appear only under heavy
  errors; their meaning is not established. 8 depends on the FEC state, not only on the frame: after 50 or more
  frames with 20 bit errors each, the next four or five clean frames still returned 8.
* Status words seen: encode half 1: 0, 0x0002, 0x0402, 0x8002, 0x8402 (0x8000 with a tone frame or the test mode);
  decode half 0: 0, 0x0002, 0x0020, 0x8002. Stock uses neither for the audio. The bit meanings are not established.
* Encode ECC and FEC decode work in place when both pointers name one 72-halfword buffer.
* Nothing checks that a state was initialized. FEC decode on an FEC state that FEC init has not written runs off
  the end of memory.
* The two extra calls hand the FEC result to the decoder. Without them the fields at D+0x618 stay at the zeros that
  decoder init wrote. The decoded PCM is the same with and without them at 0 to 5 random bit errors per frame and
  differs at 6 and more in one run; another run differed from 3. It matters only on a bad channel. A host that does
  its own FEC cannot make these calls.

### What stock passes

Static, from the maker's code around its calls. It says what stock does, not what a host must do.

| argument | in stock |
|---|---|
| encode r1, decode r3 | one variable each. Stock's init routine stores 0. The only other writer found is a branch of the routine below (it would store 0..4) that none of that routine's four callers takes |
| encode ECC r2, FEC decode r1 | one variable each. Stock's init routine stores 0. A maker's routine (0x0804E756) stores one value in both, limited to 0..6. Its four callers pass 4 or 0. So stock gives up four frame bits in one of its modes. Which mode was not established |
| encode flags | the constant 0x800 at five of the eight calls, a variable or its working copy at the other three. Stock's init routine stores 0x1840 in that variable. That the 0x800 calls flush the end of a transmission is inferred, not established |
| encode stack word 2 | a variable. Stock's init routine stores 0x2000 |
| decode flags | the constant 8 at the init call. At three calls the FEC decode result that stock saved for the frame, ORed with a variable of its own; at the other three that variable alone. Stock's init routine does not write the variable |
| sample count | the constant 80 at every call |

A host that does the FEC itself never passes the frame bit count to the codec's FEC. A frame that stock sent with
four bits given up differs from a normal one only in air bits 59, 63, 67 and 71 (emulated: the positions that change
with k = 4).

### Encode flags

The flags word is a set of feature bits, one value on every call of a transmission. Measured on this build in the
emulator:

| Bit | Effect |
|---|---|
| 0x0040 | Noise suppression. Noise alone: 1084 -> 241 RMS after decoding. Its strength is encode's stack word 2 |
| 0x0800 | Noise gate. Noise alone comes out lower, by an amount that changed from run to run: 1084 -> 39 RMS in one, 1051 -> 795, 1113 -> 426 and 1057 -> 226 in others |
| 0x1000 | Tone detection: a steady tone or a DTMF pair is sent as a tone frame (status bit 0x8000) and decodes at its frequency. 1000, 1500 and 2500 Hz come back at 1000.0, 1500.0 and 2499.9 Hz, 440 Hz at 437.5 Hz, the tone frame grid |
| 0x2000 | Input AGC: the level of speech-like input changes (856 -> 579 RMS). Not characterized further |
| 0x4000 | Test mode: one fixed frame whatever the input |
| others | Not characterized on this build |

* Stock passes 0x1840 or 0x800 ([What stock passes](#what-stock-passes)).
* These are the bits `drivers/vocoder/ambe2_flags.h` maps the encode options to ([dmr-voice.md](dmr-voice.md) 6).
* The strength word, with flags 0x40: noise of sigma 500 (530 RMS decoded with no flag) decodes at about 58, 80, 120,
  238 and 238 RMS for 0, 0x1000, 0x2000, 0x4000 and 0x7FFF. The numbers move by a few RMS with the noise; the order
  does not. 0x4000 and 0x7FFF give the same bits. With 0x40 clear the encoded bits are the same for every value.

### Init

Stock's init routine (0x08085E7E) makes these calls in this order:

| # | call | arguments | returns |
|---|---|---|---|
| 1 | encoder init | E, 1, 1 | 0 |
| 2 | FEC init | the encoder side's FEC state, 1 | 23 |
| 3 | decoder init | D, 1 | 0 |
| 4 | FEC init | the decoder side's FEC state, 1 | 23 |
| 5 | encoder parameters | E, -22, 20, -10; -40, 0x4000, 3276, 3276, 1310, 10, 20 | |
| 6 | encoder set | E, 128, -20 | 0x4000 |
| 7 | decoder set | D, 128, -20 | 0x4000 |
| 8 | decode | PCM, 80, a bit buffer, 0; 8, 0, D | 0x20 |

* The order, the arguments and the block addresses are static and were logged from a run of stock's init routine in
  the emulator. The return values and the bullets below are emulated.
* Call 8 is a first decode with flags 8: the decoder ignores the bits (status 0x20). The emulator runs gave it 72
  zero halfwords.
* Stock keeps E at 0x24012A84, the encoder side's FEC state at E+0x16E4, D at E+0x1A40 and the decoder side's FEC
  state at D+0x7F8. Nothing in the codec depends on those addresses.
* Each call stays in its own block: encoder init E; FEC init F; decoder init D; encoder parameters
  E[0x16A2..0x16B8); encoder set E[0x1372..0x1376); decoder set D[0x7A0..0x7A4). So the encoder's half (1, 2, 5, 6)
  and the decoder's half (3, 4, 7, 8) are independent, and one block can hold the encoder's state or the decoder's
  ([codec-placement.md](codec-placement.md), "STM32 radios", State).
* The init calls write every byte of a block before anything reads it. Blocks filled with 0x00, 0x5A, 0xA5 or 0xFF
  before init give the same output and the same final state.

### State sizes (measured in an emulator)

Each block was placed so that it ends at an unmapped page.

| block | size | notes |
|---|---|---|
| encoder state E | 0x16E0 (5856) bytes | 0x16DC faults (encoder init) |
| decoder state D | 0x7EC (2028) bytes | 0x7E8 faults (decoder init) |
| FEC state | 0x44 (68) bytes each | 0x40 faults; only for the codec's own FEC routines |
| FEC result record | 0x34 bytes | only for the two extra calls |
| PCM buffer | 80 samples per call | encode reads 160 bytes, decode writes 160 |
| bit buffer | 49 halfwords; 72 for the FEC calls | |

Stack below SP at the entry (stack arguments lie above it):

| entry | SP low-water mark | lowest byte touched | stack arguments |
|---|---|---|---|
| encode | 4048 | 4046 | 16 bytes |
| decode | 2192 | 2188 | 12 bytes |
| FEC decode | 1008 | 1004 | 8 bytes |
| encode ECC | 192 | 192 | none |
| encoder init, decoder init, FEC init | 48, 44, 28 | 48, 44, 28 | none |
| the set, parameter and extra calls | 12 to 32 | 12 to 32 | 28 bytes for encoder parameters |

* Encode with 4048 bytes below its entry SP runs; with 4040 it faults.
* The codec has no static RAM. With every RAM access checked (init and 16 frames), nothing outside the state blocks,
  the buffers and the stack was touched.
* The blocks hold no address, flash or RAM: after the same 40 frames their bytes are identical with the code at its
  stock address, in four moved layouts and at three RAM bases. They can live anywhere.

### Processor state

What the code contains is static (every instruction the twelve entries reach). Counts and "in the trace" are
emulated.

| item | fact |
|---|---|
| Registers | r4-r11 and SP come back unchanged from every call. The Q flag can come back set (after about half the decode calls and a few encode calls in one run); nothing in the codec clears it |
| FPU | not used: no FPU or coprocessor instruction and no soft-float call. A hard-float caller's FPU registers are untouched |
| System | no MSR, MRS, CPS, SVC, BKPT, WFI, barrier or exclusive access in reached code. Interrupts are never masked |
| Instruction set | Thumb-2 with the DSP extension and the hardware divider. This radio's Cortex-M7 has both |
| Division | four SDIV (0x080AF16C, 0x080AF1B2, 0x080AF22C, 0x080AF3F4), none with a test for a zero divisor. 6375 executions in one trace, no zero divisor. `CCR.DIV_0_TRP` must stay 0, its reset value |
| Unaligned loads | happen with every block 4-aligned: at 0x080B9944 and 0x080B9978 from T3, and at 0x080B21D6 from D and the stack. `CCR.UNALIGN_TRP` must stay 0, its reset value |
| Multi-word loads | the LDRD, LDM and STM with a base other than SP address T3, T5 and the stack only, always at a multiple of 4 in the trace. None addresses a state block or a caller's buffer |
| Stack alignment | SP at 4 modulo 8 at the entry gives the same output |
| Reentrancy | no static data. Encoder calls touch only E, its FEC state and their buffers; decoder calls only D, its FEC state and theirs |
| Outside calls | none. No call or branch leaves the five ranges; no global variable, peripheral, interrupt, RTOS or C library call |

## Position dependence

The code is not position independent. The site lists are static: a recursive descent from the twelve entries and
three code pointers, and a sweep of every word, MOVW/MOVT pair and branch in the five ranges for what the descent
does not reach. A second sweep written from this page alone found the same sites and no other. That the lists are
enough is emulated: the moved layouts of [Verification on a host](#verification-on-a-host).

Everything in the five ranges that holds an address:

| kind | sites | rewritten when |
|---|---|---|
| literal word holding a table address | 71 in reached code (A 9, B 15, C 44, island 1 2, island 2 1), 2 more in C that no reached code loads | the target's window moves |
| MOVW/MOVT pair building a table address | 3 in reached code, 3 in code not reached, all in C | the target's window moves |
| literal word holding a code address | 1: 0x080ADBFC in B holds 0x080D9B09, island 1 + 0xF8 with the Thumb bit | island 1 moves |
| MOVW/MOVT pair building a code address | 4, all in FEC decode: two build 0x080AF849 (in B), two 0x080F17E1 (island 2 + 0x78 with the Thumb bit) | B, or island 2, moves |
| PC-relative BL or B between two ranges | 292: A to B 70, B to C 58, C to B 152, island 1 to B 12 | the distance between the two ranges changes |

* [Reference sites](#reference-sites) lists every site: 74 words, 10 pairs, 292 branches.
* Of the 292 branches, 280 are in code the ten host entries reach (A to B 59, B to C 58, C to B 151, island 1 to B
  12), 4 in the two functions only stock calls, 8 in code not reached. 281 are BL and 11 are unconditional B.W; none
  is conditional.
* A never branches to C. Island 2 branches nowhere outside itself.
* Island 1 is reached only through the word at 0x080ADBFC. The encoder copies it into a block on its own stack and
  calls through it; it is not kept in the encoder state. FEC decode passes island 2 and 0x080AF848 as function
  pointers on its stack. These are the only indirect calls.
* Nothing else is absolute: no RAM or peripheral address in any literal or pair, no absolute jump table, no reference
  to anything outside the 25 windows.

What a move must respect:

* **Code.** A range moves by a multiple of 4: its literal loads are relative to the PC rounded down to 4. A move by 2
  faults (emulated).
* **A, B and C at their stock distances** (one block of 0x15C98 bytes): none of their 280 mutual branches changes.
  Only the 12 calls from island 1 into B are re-encoded. The two holes in that block need not hold the maker's bytes.
* **Tables.** Each window moves by its own distance, a multiple of 4. T3 and T5 are read with LDRD, which on this
  core faults at an address that is not a multiple of 4 whatever `CCR` says (ARMv7-M Architecture Reference Manual,
  alignment behavior). The emulator has no alignment faults, so this rests on the architecture, not on a run.
* **Inside a window** nothing is repacked: its objects keep their order and distances.
* **The two T17 words** hold the window start minus 2 and keep that distance.

## Hosting

Implemented as the second layout below, untested on a radio: the merge tools place the codec in sector 7, the
shared STM32 host (`platform/stm32/hrc/vocoder.c`) runs it, `boards/cs7000p/vocoder_cs7000p.c` pins it.

The SoC's program flash is two banks of eight 128 KB sectors ([hardware-cs7000p.md](hardware-cs7000p.md) 2.1). The
image is linked at 0x08000000 and uses bank 1 only (0x08000000..0x08100000), and of it sectors 0 to 6
(`boards/cs7000p/memory.ld`). Bank 2 is never written ([hardware-cs7000p.md](hardware-cs7000p.md) 2.3).

Two layouts ran in the emulator with the same output as the whole stock image
([Verification on a host](#verification-on-a-host)).

**Layout 1: stock addresses, no rewrite.**

| sector | range | holds |
|---|---|---|
| 5 | 0x080A0000..0x080BFFFF | A, B, C (56012 bytes) |
| 6 | 0x080C0000..0x080DFFFF | T1, T2, T3, island 1, T4 (12316 bytes) |
| 7 | 0x080E0000..0x080FFFFF | T5 to T20, island 2 (3559 bytes) |

The image must stay out of all 25 windows. Kept apart by sector it ends by 0x080A0000: 640 KB, and three sectors for
72 KB of codec. This is the cost [codec-placement.md](codec-placement.md) names for stock addresses.

**Layout 2: moved into one sector, with the rewrites of [Position dependence](#position-dependence).** This follows
the rule of [codec-placement.md](codec-placement.md).

| variant | size | rewrites | checked as |
|---|---|---|---|
| every window packed in the order of the tables above, each at its stock address modulo 16 | 72031 bytes | 74 words, 10 pairs, 292 branches | packed from 0x080E0000: 0x080E0004..0x080F1963 |
| A to C as one block at stock distances, then the islands and tables the same way | 105247 bytes | 74 words, 10 pairs, 12 branches | 0x080E0004..0x080F9B23, the two holes undefined. Also with the islands and tables 0x1C further on: 105279 bytes, ending at 0x080F9B43 |

* Either fits sector 7 (131072 bytes) and leaves sectors 0 to 6, 896 KB in one piece, to the image.
* 0x080FFF80, where the STM32 descriptor lives ([codec-placement.md](codec-placement.md), "STM32 radios"), is the end
  of sector 7 on this SoC too.
* That descriptor names one code window and up to five table windows. This build has five code ranges and 20 table
  windows. The second variant has one code window whose first word is the signature word and whose only re-encoded
  branches are BL from an island, as the existing codec maps have. The first variant also re-encodes 11 B. The
  layout in use (below) names the islands and tables as one table window.
* The rewrite list is fixed by this build. A site must hold its stock value before it is rewritten.

**The layout in use.** A to C as one code window at stock distances, then island 1, island 2 and T1 to T20 as one
block, each window at the lowest address after the one before it that has its stock address modulo 16. It has the
ends of the second variant above: 0x080E0004..0x080F9B23, 105247 bytes.

| placed | holds | descriptor |
|---|---|---|
| 0x080E0004..0x080F5C9C | the code window: A, B, C | code, 0x15C98 bytes |
| 0x080F5CA0..0x080F9B23 | island 1, island 2, T1 to T20, 0xFF between | table window 0, 0x3E83 bytes |
| 0x080FFF80..0x08100000 | the descriptor, version 1, tag 0x00090096 | |

Four bytes 0xFF lie between the code window and the block, in neither. 0x080F9B23..0x080FFF80 is free: 25693 bytes
0xFF.

| window | stock | length | placed | moved by | 0xFF before it |
|---|---|---|---|---|---|
| code (A, B, C) | 0x080A4F24 | 0x15C98 (89240) | 0x080E0004..0x080F5C9C | +0x3B0E0 | |
| island 1 | 0x080D9A10 | 0x874 (2164) | 0x080F5CA0..0x080F6514 | +0x1C290 | (the block's start) |
| island 2 | 0x080F1768 | 0xE4 (228) | 0x080F6518..0x080F65FC | +0x4DB0 | 4 |
| T1 | 0x080C469C | 0x74 (116) | 0x080F65FC..0x080F6670 | +0x31F60 | 0 |
| T2 | 0x080CE9E0 | 0x12A0 (4768) | 0x080F6670..0x080F7910 | +0x27C90 | 0 |
| T3 | 0x080D0D28 | 0x1034 (4148) | 0x080F7918..0x080F894C | +0x26BF0 | 8 |
| T4 | 0x080DF228 | 0x460 (1120) | 0x080F8958..0x080F8DB8 | +0x19730 | 12 |
| T5 | 0x080E0770 | 0x3C0 (960) | 0x080F8DC0..0x080F9180 | +0x18650 | 8 |
| T6 | 0x080E9ECC | 0x200 (512) | 0x080F918C..0x080F938C | +0xF2C0 | 12 |
| T7 | 0x080ED098 | 0x1C8 (456) | 0x080F9398..0x080F9560 | +0xC300 | 12 |
| T8 | 0x080F0974 | 0x116 (278) | 0x080F9564..0x080F967A | +0x8BF0 | 4 |
| T9 | 0x080F148C | 0x100 (256) | 0x080F967C..0x080F977C | +0x81F0 | 2 |
| T10 | 0x080F1924 | 0xD8 (216) | 0x080F9784..0x080F985C | +0x7E60 | 8 |
| T11 | 0x080F1E84 | 0xA2 (162) | 0x080F9864..0x080F9906 | +0x79E0 | 8 |
| T12 | 0x080F21DC | 0x68 (104) | 0x080F990C..0x080F9974 | +0x7730 | 6 |
| T13 | 0x080F22A8 | 0x64 (100) | 0x080F9978..0x080F99DC | +0x76D0 | 4 |
| T14 | 0x080F2814 | 0x48 (72) | 0x080F99E4..0x080F9A2C | +0x71D0 | 8 |
| T15 | 0x080F2B64 | 0x40 (64) | 0x080F9A34..0x080F9A74 | +0x6ED0 | 8 |
| T16 | 0x080F3638 | 0x2C (44) | 0x080F9A78..0x080F9AA4 | +0x6440 | 4 |
| T17 | 0x080F4674 | 0x20 (32) | 0x080F9AA4..0x080F9AC4 | +0x5430 | 0 |
| T18 | 0x080F4B5C | 0x18 (24) | 0x080F9ACC..0x080F9AE4 | +0x4F70 | 8 |
| T19 | 0x080F4E04 | 0x14 (20) | 0x080F9AE4..0x080F9AF8 | +0x4CE0 | 0 |
| T20 | 0x080F5994 | 0x1F (31) | 0x080F9B04..0x080F9B23 | +0x4170 | 12 |

The block is 16003 bytes: 15875 bytes of windows and 128 bytes 0xFF between them. Every window moves by a multiple
of 16.

* Not carried: everything in the windows that is not the codec's. In the code window the maker's code between A
  and B and between B and C, the byte fill, the exit stub's hole and the three two-byte constants (seven spans,
  33368 bytes); in T12, T13 and T20 the three spans named above (14 bytes). The tool checks each carved window by
  its SHA-256, rewrites the sites, takes the result back to stock once, and then writes 0xFF over the ten spans.
  71733 bytes of the stock file are in the merged image.

  | span | stock | length | placed |
  |---|---|---|---|
  | the byte fill, in a hole of A | 0x080A583C..0x080A5876 | 58 | 0x080E091C..0x080E0956 |
  | the constant after it | 0x080A5876..0x080A5878 | 2 | 0x080E0956..0x080E0958 |
  | the maker's code between A and B | 0x080A5F50..0x080AC100 | 25008 | 0x080E1030..0x080E71E0 |
  | the maker's code between B and C | 0x080AF964..0x080B1980 | 8220 | 0x080EAA44..0x080ECA60 |
  | the exit stub and its hole, in C | 0x080B9274..0x080B92C0 | 76 | 0x080F4354..0x080F43A0 |
  | the constant after the first shift helper | 0x080BAB96..0x080BAB98 | 2 | 0x080F5C76..0x080F5C78 |
  | the constant at the end of C | 0x080BABBA..0x080BABBC | 2 | 0x080F5C9A..0x080F5C9C |
  | in T12 | 0x080F2220..0x080F2224 | 4 | 0x080F9950..0x080F9954 |
  | in T13 | 0x080F22BA..0x080F22BC | 2 | 0x080F998A..0x080F998C |
  | in T20 | 0x080F59A4..0x080F59AC | 8 | 0x080F9B14..0x080F9B1C |

  No site, no entry, no branch target and no address a site holds lies in a span (the codec map's loader checks it).
  The emulator runs had the two large spans undefined and every other span as stock: no run has had the eight small
  spans blanked.
* Rewritten: 74 words, 10 pairs, the 12 BL of island 1. The two T17 words keep their distance to T17.
* CRC-32s that follow from the CRC-32s above, the site list and the layout, before the spans inside the ranges
  and the tables are blanked: the code window 0xE24A92CF; the same with only A, B and C (0xFF outside them)
  0xB4A36974; the block 0x7BC061E9. The tools check their bytes against them.
* CRC-32, as placed and pinned: code 0xA6BAF942, block 0x1DD4537C. They need the bytes of the blanked spans and are
  the tool's: its run on the stock file printed them after the three values above matched.
* The firmware reports `cs7000p-v90096-s7` when the descriptor, both CRC-32s and the first word of A match.
* `CCR.UNALIGN_TRP` and `CCR.DIV_0_TRP`: the STM32 start-up clears both (`platform/stm32/main.c`, every family:
  the image can be entered by the ROM bootloader's jump, without a reset), and the host clears them once more before
  its first codec call if either is set.
* A word of this flash that a power cut left half written can read as a bus fault
  ([hardware-cs7000p.md](hardware-cs7000p.md) 2.3). The installers therefore never start an image over a sector 7
  that an unfinished install touched ([release.md](../dev/release.md#the-codec-on-this-route)). The boot check
  first reads every flash word of sector 7 under a guard (a bus error there is a refusal, not a fault), so a sector 7
  that another program left torn costs DMR voice, not the start-up, until an install through the ROM bootloader
  writes or erases the sector. Untested on a radio.
* The image has sectors 0 to 6 (`boards/cs7000p/memory.ld`). An update that writes only those keeps the codec
  (`[flash] keep_codec = true`).

**RAM** (anywhere; no address is fixed): E 0x16E0 and D 0x7EC, 4-aligned as stock has them, or one block of 0x16E0
for either. An FEC state of 0x44 per side and the 0x34 record only if the codec's own FEC and the extra calls are
used. A PCM buffer of 80 samples per call and a bit buffer of 49 halfwords (72 for the FEC calls). The blocks need
not be cleared. CPU-only RAM is enough: no DMA touches them.

**Stack:** 4048 bytes below SP at encode's entry plus 16 bytes of stack arguments; 2192 plus 12 for decode. The call
veneer and the caller come on top. The audio task that makes the calls has 5120 bytes
(`platform/stm32/hrc/i2s_pcm.c`).

**Processor state:** Thumb; `CCR.UNALIGN_TRP` = 0 and `CCR.DIV_0_TRP` = 0, their reset values, and the port sets
neither bit today; FPU on or off; any interrupt state. The instruction cache should make no difference, since the
codec runs from flash and writes no code: that is reasoning, not a measurement.

**Calls:**

1. Once per state, before its first frame: its half of [Init](#init).
2. Per transmitted frame: encode (bits, 0, PCM, 80; flags, 0, 0x2000, E), then encode (bits, 0, PCM + 80 samples, 80;
   flags, 1, 0x2000, E), the same flags word on both. Then the FEC in `core/dmr/ambe`.
3. Per received frame: the FEC in `core/dmr/ambe`. Then decode (PCM, 80, bits, 0; 0, 0, D) and decode (PCM + 80
   samples, 80, bits, 0; 0, 1, D). The first stack word is the decode flags: 0 for a good frame.
4. Bits are halfwords of exactly 0 or 1. The sample count is always 80.

**Where the bytes come from.** The radio is installed through the SoC's ROM bootloader after a Read Unprotect that
erases the whole program flash ([hardware-cs7000p.md](hardware-cs7000p.md) 2.5). After that the radio no longer holds
the maker's image, and before it a read-protected chip gives nothing. So the codec can only come from the maker's
file, on the user's machine. No codec byte of layout 2 shares a sector with the image, so an update that writes only
the image's sectors can keep the codec (`[flash] keep_codec` in `board.toml`, true).

## Extraction recipe (V9.00.96)

1. Check the file: size 0x1AD53E, SHA-256 `1a4bdf38...`, the header's payload length 0x001AD4BC at 0x14 and payload
   offset 0x80 at 0x44. Refuse any other file.
2. Get the key from the file ([Key recovery](#key-recovery)) and check its SHA-256 (`72315db3...`).
3. Check the payload: N = 0x000D6A56 at payload offsets 0 and 4 + N, two zero words at 8 + 2N.
4. Decrypt each of the 25 windows: file offset = 0x84 + (A - 0x08020000), key index (file offset - 0x80) & 0xFF.
   Check each window's SHA-256 and each code range's first word. The second copy (file offset plus 0xD6A5A) must
   give the same bytes.
5. Place every window where the board's layout puts it, each moved by a multiple of 4, its bytes in order. Layout 1
   places them at their stock addresses and needs no step 6.
6. Rewrite every site of [Reference sites](#reference-sites) for the moves. First check that the site holds its stock
   value.

   | site | new value |
   |---|---|
   | word | the old value plus the move of the window it points into. The two T17 words point 2 below T17 and move with T17. The code pointer word moves with island 1 and keeps bit 0 set |
   | MOVW/MOVT pair | the same for the 32-bit address the pair builds: the low half in the MOVW (encoding T3), the high half in the MOVT (encoding T1), each in its imm4:i:imm3:imm8 fields. A code address keeps bit 0 set |
   | branch | BL (encoding T1) or B (encoding T4): the old offset plus the target range's move minus the site range's move. Nothing to do when the two ranges move together |

7. Write the board's descriptor for the placed windows. 0xFF fills the rest after the image.
8. Check the result: undo the rewrites and compare each window with its stock SHA-256.

The merge happens on the user's machine, never in a release. The placed CRC-32s depend on the layout: the one in use
and its CRC-32s are in [Hosting](#hosting) ([codec-placement.md](codec-placement.md), "STM32 radios").

## Verification on a host

Everything here is an emulator result (Unicorn 2.1.4, Cortex-M7 model). **None of it has run on a radio.**

Setup: flash holds only the listed windows. Every other byte of the pages they touch is an undefined instruction, and
every other page is unmapped. One RAM block, filled with junk before init. Nothing else of the maker's image.

Signal checks, the same from the whole image, from the 25 windows alone, and with every flash read and every executed
block checked against the exact window bounds:

| in | out |
|---|---|
| 440 Hz tone, flags 0x1840 | 437.5 Hz |
| 1000, 1500, 2500 Hz, flags 0x1840 | 1000.0, 1500.0, 2499.9 Hz |
| 1000 Hz, flags 0x800 | 1000.0 Hz, coded as voice, at a lower level |
| silence | silence (RMS 0.03, peak 1) |
| the AMBE silence frame | FEC result 0, the same 9 bytes back, silence |

Layouts, each compared by one SHA-256 over every bit, air bit, FEC result, status word and PCM sample:

| layout | rewrites | result |
|---|---|---|
| whole image at 0x08020000 | none | reference |
| 25 windows at stock addresses | none | same |
| all 25 packed from 0x080E0000, stock address modulo 16 (0x080E0004..0x080F1963, 72031 bytes) | 74 words, 10 pairs, 292 branches | same |
| all 25 packed in reverse order from 0x08060004, stock address modulo 4 only | the same | same |
| A, B, C at stock distances from 0x080E0004, the holes undefined; islands and tables packed from 0x080F5CB8 (105279 bytes) | 74 words, 10 pairs, 12 branches | same |
| the same with the islands and tables packed from 0x080F5C9C, right after the block (105247 bytes) | 74 words, 10 pairs, 12 branches | same (the 60-frame run with exact bounds, the 300 and 400 frame mixed runs, the wide run) |
| all 25 packed from 0x08120000 | 74 words, 10 pairs, 292 branches | same |

* Each window's stock bytes were carved from the stock file by the [recipe](#extraction-recipe-v90096) alone and
  checked by the hashes above before every run. The runs were repeated by a second, separately written carver and
  rewriter working only from this page and the stock file; they gave the same hashes.
* Mixed workload, 120, 300, 400 and 400 frames on four seeds: speech-like signal, tone, silence, noise, full-scale
  square, a DTMF pair, a chirp; eight flag words; 1 to 12 injected bit errors; random frames; forced decode flags 4
  and 8.
* Wide workload, 338 frames: the test mode with every first argument of encoder set and decoder set from 0 to 255,
  k = 0 to 6 with 4-bit soft symbols, twelve flag words, heavy errors, the two extra calls.
* Every layout gave the reference hash on every workload. No window of a moved layout sits at its stock address, and
  nothing is mapped at the stock addresses.
* With exact bounds checked there was no violation in any of the layouts (60 mixed frames), nor in the wide
  workload at stock addresses and packed from 0x080E0000.
* Negative runs, each faulting at once: tables not rewritten; code pointers not rewritten; branches not re-encoded;
  A to C as one block without the 12 island calls re-encoded; windows moved by 2 modulo 4.
* RAM: output and final state are the same at three RAM bases and with four fill values.

Limits:

* 49358 bytes of code ran, of the 55610 that the twelve entries and three code pointers reach. The other 6252 bytes
  never ran: the sites in them rest on static analysis.
* The emulator has no alignment faults, no `CCR` and no timing.
* RadioFoundry's own host code (`platform/stm32/hrc/vocoder.c`, the call veneer) has not been run against this
  codec.

## Only a radio can check

* Audio: transmit and receive voice quality and levels through this radio's audio path.
* Timing: encode and decode within 20 ms at the port's clock. The emulator gives instruction counts only, and they
  differ by counting method: about 0.35 to 0.7 million per encoded frame, up to 0.2 million per decoded frame.
* The audio task's stack under 4048 bytes of codec plus the veneer and the caller.
* Alignment on the real core: that no access faults with the blocks, buffers and tables where the host puts them.
* That `CCR.UNALIGN_TRP` and `CCR.DIV_0_TRP` are still 0 at the first codec call, and that no SDIV sees a zero
  divisor in real use.
* The codec run from its placed flash address after an install through the ROM bootloader, and kept across an update.
* The voice path against the HR_C6000 with real frames ([modem-hrc6000.md](modem-hrc6000.md) 5.2, 6.3): bit order on
  air, the FEC results, the silence frame.
* The encode flags on air (0x800 against 0x1840, and AGC 0x2000), and the noise suppressor's strength word.
* Whether stock's hand-over of the FEC result to the decoder is audible on a bad channel.

## Open items

* The hosting is written ([Hosting](#hosting)) and has not run on a radio: the merged image itself has run in no
  emulator yet.
* Read by no run: the first object of T10, T1 beyond 36 bytes, T4's second object beyond 640 bytes; 6252 bytes of
  reached code.
* Whether an SDIV divisor can be zero.
* The status word bits, the meaning of FEC decode's 8 and 12, and what the decoder does with the FEC result fields.
* Sample counts other than 80.
* The other HOST versions: each needs its own windows and site list.
* The mode in which stock gives up four frame bits in its FEC calls, and what it puts in them
  ([What stock passes](#what-stock-passes)).
* The container's two trailing bytes and three flag bytes, and why the image is there twice. They matter only for
  packing an image for the maker's own tool, which nothing here does.

## Reference sites

Every place in the five code ranges that holds an address, at its stock address. This is the content of this build's
codec map ([codec-placement.md](codec-placement.md), "Codec maps"): addresses only, no codec bytes.

Marks: (u) in code that no entry reaches; (s) in the two functions only stock calls; (b) an unconditional B, not a BL.

### Words that hold a table address

Each site is a 32-bit little-endian word in a literal pool. It holds the target window's stock start plus the offset
shown. 73 words: 71 in code the entries reach, 2 marked (u).

| target | words | site: offset |
|---|---|---|
| T1 | 2 | 0x080BA644: +0x0, 0x080BAA38: +0x0 |
| T2 | 7 | 0x080B6C48: +0x1240, 0x080B6C4C: +0x640, 0x080B6C50: +0x240, 0x080B6C54: +0xC0, 0x080B6C58: +0x40, 0x080B6C5C: +0x140, 0x080B6C60: +0x0 |
| T3 | 16 | 0x080AF53C: +0x1028, 0x080AF604: +0x101C, 0x080AF6F4: +0x1014, 0x080B1C24: +0x0, 0x080B1E54: +0xC00 (u), 0x080B1E58: +0x0, 0x080B810C: +0xC00, 0x080B8298: +0xC00, 0x080B9270: +0xC00, 0x080B9570: +0x1014, 0x080B98FC: +0x1028, 0x080B99A4: +0xC00, 0x080B9AD4: +0x1004, 0x080BA4C0: +0x1004 (u), 0x080BA648: +0xC00, 0x080BAA3C: +0xC00 |
| T4 | 2 | 0x080AD36C: +0x0, 0x080B4FEC: +0x50 |
| T5 | 1 | 0x080B93DC: +0x0 |
| T6 | 2 | 0x080B6464: +0x0, 0x080B7218: +0x0 |
| T7 | 2 | 0x080A506C: +0x0, 0x080ACB0C: +0xC8 |
| T8 | 2 | 0x080B1A10: +0x0, 0x080B1A14: +0xE4 |
| T9 | 7 | 0x080A596C: +0x0, 0x080A5F18: +0x0, 0x080AD0A4: +0x0, 0x080AE7D8: +0x0, 0x080B7804: +0x0, 0x080B79A0: +0x0, 0x080F17DC: +0x0 |
| T10 | 3 | 0x080AE300: +0x48, 0x080AE5E0: +0x90, 0x080AE5E4: +0x0 |
| T11 | 1 | 0x080B7C9C: +0x0 |
| T12 | 2 | 0x080AC6C8: +0x0, 0x080B94B4: +0x3C |
| T13 | 2 | 0x080D9B04: +0x0, 0x080DA110: +0x14 |
| T14 | 10 | 0x080B57EC: +0x20, 0x080B57F0: +0x30, 0x080B57F4: +0x24, 0x080B57F8: +0x8, 0x080B57FC: +0x18, 0x080B5800: +0xC, 0x080B5804: +0x0, 0x080B5808: +0x2C, 0x080B580C: +0x14, 0x080B5BE4: +0x0 |
| T15 | 1 | 0x080BAB3C: +0x0 |
| T16 | 1 | 0x080AE3CC: +0x0 |
| T17 | 4 | 0x080A54E8: -0x2, 0x080A59FC: +0xC, 0x080A5A00: +0x18, 0x080A5BC0: -0x2 |
| T18 | 1 | 0x080AE7D4: +0x0 |
| T19 | 1 | 0x080B8E10: +0x0 |
| T20 | 6 | 0x080A54E4: +0x0, 0x080A5F1C: +0x0, 0x080ADF20: +0x0, 0x080AE1EC: +0x0, 0x080B1CE4: +0x18, 0x080BA400: +0x8 |

### Instruction pairs that build an address

A MOVW with the low half and a MOVT with the high half, each a 32-bit Thumb-2 instruction at the address shown.

| MOVW at | MOVT at | address built | points to |
|---|---|---|---|
| 0x080A5C48 | 0x080A5C4C | 0x080AF849 | code: B + 0x3748, Thumb bit set |
| 0x080A5C52 | 0x080A5C5A | 0x080F17E1 | code: island 2 + 0x78, Thumb bit set |
| 0x080A5CF4 | 0x080A5CF8 | 0x080AF849 | code: B + 0x3748, Thumb bit set |
| 0x080A5CFE | 0x080A5D02 | 0x080F17E1 | code: island 2 + 0x78, Thumb bit set |
| 0x080B2606 | 0x080B2612 | 0x080F2224 | T12 +0x48 |
| 0x080B5554 | 0x080B555C | 0x080F284C | T14 +0x38 |
| 0x080B55FA | 0x080B5602 | 0x080F284C | T14 +0x38 |
| 0x080B9EF8 | 0x080B9EFC | 0x080D1928 | T3 +0xC00 (u) |
| 0x080B9F12 | 0x080B9F18 | 0x080D1928 | T3 +0xC00 (u) |
| 0x080B9F46 | 0x080B9F4E | 0x080D1928 | T3 +0xC00 (u) |

### The word that holds a code address

0x080ADBFC (in B) holds 0x080D9B09: island 1 + 0xF8, Thumb bit set.

### Branches between two ranges

Each site is a 32-bit BL, or a 32-bit unconditional B where marked (b). The target is the site's address plus 4
plus the instruction's signed 25-bit offset. 292 sites: 281 BL and 11 B.

A to B: 70 sites, 4 of them (s), 7 (u), 29 targets.

| target | sites |
|---|---|
| 0x080AC100 | 0x080A4F5C, 0x080A4F74, 0x080A4F86, 0x080A4F9E, 0x080A54DC, 0x080A54FA (s), 0x080A5514 (s), 0x080A5F28 (s), 0x080A5F3E (s) |
| 0x080AC164 | 0x080A51DE, 0x080A51EE, 0x080A5254, 0x080A5266, 0x080A528E, 0x080A52D8, 0x080A52E4, 0x080A52F2, 0x080A52FE, 0x080A530C, 0x080A5350, 0x080A535E, 0x080A542A, 0x080A5574, 0x080A5620, 0x080A5A2A, 0x080A5A34 |
| 0x080AC290 | 0x080A4FDC |
| 0x080AC3C8 | 0x080A5018 |
| 0x080AD1E0 | 0x080A5032 |
| 0x080AD7AC | 0x080A504E |
| 0x080AD974 | 0x080A5066 |
| 0x080AD9D8 | 0x080A50BA |
| 0x080ADD28 | 0x080A5138 (u), 0x080A55C8 (u) |
| 0x080ADD38 | 0x080A50F4 (b) (u), 0x080A55B4 (b) (u) |
| 0x080ADDB4 | 0x080A51D4 (b), 0x080A568E (b) |
| 0x080AE284 | 0x080A510E, 0x080A55DA |
| 0x080AE640 | 0x080A5126, 0x080A55F4 |
| 0x080AE6D0 | 0x080A518C (u), 0x080A51A8 (u) |
| 0x080AE72C | 0x080A5460, 0x080A547C |
| 0x080AE748 | 0x080A5470, 0x080A548A |
| 0x080AE774 | 0x080A59CE, 0x080A59EC, 0x080A5CC8, 0x080A5D5E |
| 0x080AE7A0 | 0x080A5468, 0x080A5482 |
| 0x080AE7DC | 0x080A549A, 0x080A5CBE |
| 0x080AE818 | 0x080A54D2 |
| 0x080AE904 | 0x080A5646, 0x080A5650 |
| 0x080AE93C | 0x080A555C, 0x080A55A0 |
| 0x080AEDEC | 0x080A5590 |
| 0x080AF0A0 | 0x080A560C (u) |
| 0x080AF140 | 0x080A5720, 0x080A597A |
| 0x080AF3C0 | 0x080A5B72 |
| 0x080AF680 | 0x080A5B7A |
| 0x080AF90C | 0x080A58E0, 0x080A594C |
| 0x080AF940 | 0x080A5E94 |

B to C: 58 sites, 46 targets.

| target | sites |
|---|---|
| 0x080B1F10 | 0x080AC270 |
| 0x080B1FA0 | 0x080AC44A |
| 0x080B2038 | 0x080AC390, 0x080AC39C |
| 0x080B225C | 0x080AC284 |
| 0x080B231C | 0x080AC2B4 |
| 0x080B2384 | 0x080AEA9A |
| 0x080B2400 | 0x080AC7EE, 0x080AC826 |
| 0x080B2434 | 0x080AC60E, 0x080AC630 |
| 0x080B24BC | 0x080AC3F8 |
| 0x080B26C8 | 0x080AC412 |
| 0x080B3570 | 0x080ADABC |
| 0x080B3590 | 0x080ADBD2 |
| 0x080B3818 | 0x080AC8E6 |
| 0x080B42AC | 0x080ACADC |
| 0x080B4508 | 0x080AC90A |
| 0x080B476C | 0x080AD21C |
| 0x080B4CD0 | 0x080AD336, 0x080AD35E |
| 0x080B54D0 | 0x080AD7FE |
| 0x080B5BE8 | 0x080ADB90 |
| 0x080B5C34 | 0x080ADC6C |
| 0x080B5CB4 | 0x080ADC54 |
| 0x080B5EAC | 0x080ADC80 |
| 0x080B5FD4 | 0x080ADCD4 |
| 0x080B6228 | 0x080ADE5A, 0x080AE08A |
| 0x080B622C | 0x080ADE94 |
| 0x080B657C | 0x080ADEBC, 0x080AE12A |
| 0x080B6710 | 0x080ADEDC, 0x080ADEE8 |
| 0x080B6724 | 0x080ADF06 |
| 0x080B67DC | 0x080ADF18 |
| 0x080B6878 | 0x080ADF62 (b), 0x080AE17A |
| 0x080B68E4 | 0x080AEE68, 0x080AEEC6 |
| 0x080B6D40 | 0x080ADF46, 0x080AE150 |
| 0x080B6D98 | 0x080ADF50, 0x080AE0D6 |
| 0x080B71D4 | 0x080AE0E0, 0x080AE0F2 |
| 0x080B71EC | 0x080AE10A |
| 0x080B721C | 0x080AE160 |
| 0x080B7580 | 0x080AE16E |
| 0x080B7718 | 0x080AE1B0 |
| 0x080B7808 | 0x080AE1E6 |
| 0x080B7854 | 0x080AE990 |
| 0x080B78D8 | 0x080AE864 (b) |
| 0x080B795C | 0x080AEDA0 |
| 0x080B79A4 | 0x080AEA62 |
| 0x080B8684 | 0x080AEA7E |
| 0x080B8D20 | 0x080AED7C |
| 0x080B8E14 | 0x080AEDC6 |

C to B: 152 sites, 1 (u), 19 targets.

| target | sites |
|---|---|
| 0x080AC100 | 0x080B227E (b), 0x080B22C0 (b) (u), 0x080B2522, 0x080B252C, 0x080B26B6, 0x080B270A, 0x080B2720, 0x080B4526, 0x080B4538, 0x080B47A0, 0x080B47D6, 0x080B5D0E, 0x080B68C4, 0x080B7F7A, 0x080B87AE, 0x080B8B68, 0x080B8D36, 0x080BA54C, 0x080BAB6E (b) |
| 0x080AC164 | 0x080B2590, 0x080B484A, 0x080B4916, 0x080B68D8, 0x080B6CD4, 0x080B773E, 0x080B78D2 (b), 0x080B7B8E, 0x080B7C70, 0x080B7D1C, 0x080B7D5E, 0x080B7D74, 0x080B7F8A, 0x080B7FBA, 0x080B7FCA, 0x080B816C, 0x080B8290, 0x080B87B8, 0x080B888A, 0x080B8E00, 0x080B9C22 |
| 0x080AC1AC | 0x080B261A |
| 0x080AE3D0 | 0x080B77DA |
| 0x080AE72C | 0x080B71DC |
| 0x080AE774 | 0x080B671C |
| 0x080AF140 | 0x080B28EA, 0x080B6918, 0x080B69AE, 0x080B6BA8, 0x080B74B0, 0x080B74CA, 0x080B7EFA, 0x080B8FA2, 0x080B9B50 |
| 0x080AF180 | 0x080B380A |
| 0x080AF200 | 0x080B1F9A (b), 0x080B36FC, 0x080B66B2, 0x080B676E, 0x080B678E, 0x080B7836, 0x080BA6AA, 0x080BA6D2 |
| 0x080AF240 | 0x080B299C, 0x080B2A1A, 0x080B2A50, 0x080B2A9C, 0x080B2BC6, 0x080B2C06, 0x080B2C6C, 0x080B2C9E, 0x080B2E80, 0x080B2FA6, 0x080B3078, 0x080B30DA, 0x080B30F8, 0x080B3174, 0x080B321A, 0x080B3380, 0x080B3408, 0x080B3D92, 0x080B3DCA, 0x080B8300, 0x080B85C4, 0x080B860E, 0x080BA26A |
| 0x080AF2C0 | 0x080B2B22, 0x080B2B44, 0x080B2C1C, 0x080B2CC0, 0x080B3040, 0x080B3162, 0x080B8CA0, 0x080BA27E |
| 0x080AF3C0 | 0x080B2784, 0x080B27EA, 0x080B2ACC, 0x080B2BD8, 0x080B2D7E, 0x080B2DDE, 0x080B3690, 0x080B3D1C, 0x080B3E1E, 0x080B4278, 0x080B4366, 0x080B49F0, 0x080B4BB0, 0x080B64A8, 0x080B6520, 0x080B7268, 0x080B7344, 0x080B7A1A, 0x080B82F0, 0x080B861C, 0x080B86F4, 0x080B891A, 0x080B8A08, 0x080B914C, 0x080BA05A, 0x080BA290, 0x080BA2E0, 0x080BA3A8 |
| 0x080AF440 | 0x080B46E2, 0x080B5AEA, 0x080BA03E, 0x080BA1F2 |
| 0x080AF480 | 0x080B2FC4, 0x080B3182, 0x080B31B8, 0x080B3238, 0x080B3264 |
| 0x080AF4C0 | 0x080B3002, 0x080B341C, 0x080B5E64, 0x080B674A |
| 0x080AF540 | 0x080B35BE, 0x080B529C, 0x080B5C62, 0x080B6126, 0x080B6B7E, 0x080BAAA6 |
| 0x080AF640 | 0x080B330E, 0x080B78AE |
| 0x080AF680 | 0x080B49DE, 0x080B7C34, 0x080B7E8E, 0x080B7F0A, 0x080B85CC, 0x080B90D2, 0x080B90E8, 0x080B9154 |
| 0x080AF700 | 0x080B7272, 0x080B7710 |

Island 1 to B: 12 sites, 6 targets.

| target | sites |
|---|---|
| 0x080AF240 | 0x080D9C66, 0x080DA1B4, 0x080DA254 |
| 0x080AF2C0 | 0x080D9ED4, 0x080D9FF6, 0x080DA008, 0x080DA230 |
| 0x080AF3C0 | 0x080DA0D8 |
| 0x080AF440 | 0x080DA17C |
| 0x080AF4C0 | 0x080D9EF0, 0x080D9FB6 |
| 0x080AF540 | 0x080DA0C0 |
