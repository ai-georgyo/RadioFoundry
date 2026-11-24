<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# Test voice set

Speech for testing every digital voice mode, in the frame formats each mode carries. The speech is synthesized
(text to speech), not a recording of a person. The HIL voice tests send these files by default
([tests/hil/README.md](../../hil/README.md)).

Two phrases:

| Set | Speech | Duration | 20 ms frames |
|-----|--------|----------|--------------|
| `test_transmission` | a short spoken test transmission | 2.08 s | 104 |
| `ten_second_countdown` | a test transmission with a countdown, for longer overs | 10.8 s | 540 |

## Files

Each set has the same six committed files, and four Codec 2 files made from them on demand. Every file holds whole
20 ms frames (a 1600 frame is two), so the frame counts above hold for each format.

| File | Format | Size (short / countdown) | Modes |
|------|--------|--------------------------|-------|
| `<set>.pcm` | 16-bit signed little-endian, 8 kHz, mono, no header | 33,280 / 172,800 B | the source; mic injection, any encoder |
| `<set>.wav` | the same PCM in a WAV file | 33,324 / 172,844 B | listening |
| `<set>.ambe` | AMBE+2 (3600 x 2450 bit/s), 9-byte frames with FEC (72 bits), back to back, no header | 936 / 4,860 B | DMR, NXDN |
| `<set>_decoded.wav` | the `.ambe` frames decoded back to audio | 33,324 / 172,844 B | reference: what a DMR or NXDN radio should play |
| `<set>.imbe` | full rate (7200 x 4400 bit/s), 11-byte frames of the 88 information bits, first bit first, no FEC, back to back, no header | 1,144 / 5,940 B | P25 Phase 1 |
| `<set>_imbe_decoded.wav` | the `.imbe` frames decoded back to audio by `core/mbe` | 33,324 / 172,844 B | reference: what a P25 radio should play |
| `<set>.c2_3200` | Codec 2 at 3200 bit/s, 8-byte frames (64 bits, first bit first), one per 20 ms, back to back, no header. Generated, not committed | 832 / 4,320 B | M17 voice (`M17_DATA_VOICE`: two frames a stream frame) |
| `<set>_c2_3200_decoded.wav` | the `.c2_3200` frames decoded back to audio by `core/codec2`. Generated, not committed | 33,324 / 172,844 B | reference: what an M17 radio should play |
| `<set>.c2_1600` | Codec 2 at 1600 bit/s, 8-byte frames, one per 40 ms, back to back, no header. Generated, not committed | 416 / 2,160 B | M17 voice and data (`M17_DATA_VOICE_DATA`: a frame and 8 data bytes a stream frame) |
| `<set>_c2_1600_decoded.wav` | the `.c2_1600` frames decoded back to audio. Generated, not committed | 33,324 / 172,844 B | reference |

The `.ambe` 9-byte frame is the one the HIL tests' `--ambe` option and the SDR bench's voice jobs take. NXDN carries
the same AMBE+2 frames.

The `.imbe` frame is `IMBE_BYTES` in [core/mbe/imbe.h](../../../core/mbe/imbe.h) (`P25_IMBE_BYTES` in the P25 code):
the frame the firmware's P25 voice path hands the call layer before the FEC. `tools/bench/p25tool.py gen --voice-file`
takes it.

## Regenerating the IMBE files

The `.imbe` files and their decoded WAVs come from the `.pcm` through the firmware's own full-rate encoder and decoder
(`core/mbe`, fixed point, the same output on every host):

```
make -C tests/host voice-data
```

The generator is [tests/host/imbeVoiceGen.c](../../host/imbeVoiceGen.c). `make -C tests/host` runs it with `--check`,
which fails when a committed `.imbe` no longer matches the encoder's output. After a change to the encoder, regenerate
and commit both files of each set.

## The Codec 2 files

The Codec 2 files come from the `.pcm` through the firmware's own encoder and decoder (`core/codec2`, fixed point, the
same output on every host), as the M17 voice path runs them: one 20 ms block a call, each frame decoded once, then its
blocks. They are generated, not committed:

```
make -C tests/host c2-voice-data                       # into tests/host/out/voice
make -C tests/host c2-voice-data C2VOICE_OUT=<dir>     # anywhere else
```

The generator is [tests/host/c2VoiceGen.c](../../host/c2VoiceGen.c). `make -C tests/host` runs it on
`test_transmission` into `tests/host/out/voice` and fails when the speech decodes to near silence; `m17VoiceTest`
sends the same speech through the voice path both ways and checks it against the codec run directly
([docs/spec/m17.md](../../../docs/spec/m17.md), the voice path).

## Git

The repository ignores `*.ambe` and `*.wav` so that no codec material or recording is committed by accident. These
files are tracked by name (`git add -f`). Git still shows changes to them.
