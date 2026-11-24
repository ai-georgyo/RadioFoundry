<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codec 2 vocoder, modes 3200 and 1600

What an encoder and decoder must do to be bit compatible with Codec 2 at 3200 bit/s and 1600 bit/s, the two rates M17
carries ([m17.md](m17.md)). Enough to write the codec without any other source: the bit stream, every analysis and
synthesis step, every constant, and the tables (inline when small, otherwise in `tables.json` with a checksum).

Status: specification, 2026-11-22. The firmware's decoder is section 17, its encoder section 18 (`core/codec2`).

| Section | Topic |
|---|---|
| 1 | Provenance |
| 2 | Overview, frames and timing |
| 3 | Conventions and constants |
| 4 | Bit stream: field order, Gray code, packing |
| 5 | Encoder: input buffer, window, DFT |
| 6 | Encoder: pitch estimator |
| 7 | Encoder: pitch refinement, harmonic amplitudes, voicing |
| 8 | Encoder: LPC analysis, LSPs, energy |
| 9 | Encoder: quantizers and frame assembly |
| 10 | Decoder: dequantization and interpolation |
| 11 | Decoder: harmonic amplitudes from the LPC model, post filter |
| 12 | Decoder: phase synthesis and the random generator |
| 13 | Decoder: sinusoidal synthesis and output |
| 14 | Tables |
| 15 | Numerical precision, fixed point and test targets |
| 16 | What this firmware does: tables and bits |
| 17 | What this firmware does: the decoder |
| 18 | What this firmware does: the encoder |

## 1. Provenance

- The algorithm description is written from the Codec 2 design document (`doc/codec2.tex`, "Codec 2", D. Rowe et
  al.) and from the measured behavior of the Codec 2 reference implementation (stage-by-stage dumps of its internal
  values on synthetic inputs). It is prose and equations, not a translation of that implementation's code.
- The numeric table values (LSP quantizer levels, the pitch estimator's FIR coefficients) are taken from the Codec 2
  reference implementation, LGPL 2.1, by decision of the RadioFoundry maintainer (2026-11-22), not under a separate
  grant from the Codec 2 authors. Tables that the reference computes at start (windows, dequantizer outputs) are
  given as derived values with their formula.
- No code, comments or identifiers from the reference implementation are part of this page or may enter the tree.
  Clean-room rules: [docs/CLEANROOM.md](../CLEANROOM.md).
- `tables.json` and the reference test harness (`HARNESS.md`, the synthetic inputs, the reference outputs and the
  comparison script) are kept outside the repository, delivered with this page to the implementer.

## 2. Overview, frames and timing

Codec 2 is a harmonic sinusoidal vocoder. Speech is modeled every 10 ms as a sum of harmonics of a fundamental
frequency ω0, with amplitudes taken from a 10th order LPC envelope, a single voiced/unvoiced flag and phases synthesized
at the decoder. Both modes share all analysis and synthesis; they differ in how many 10 ms analysis frames make one
codec frame and in the LSP quantizer.

| Item | 3200 | 1600 |
|---|---|---|
| Sample rate | 8000 Hz, mono, 16-bit signed | same |
| Analysis/synthesis frame N | 80 samples (10 ms) | same |
| Codec frame | 160 samples (20 ms) = 2 analysis frames | 320 samples (40 ms) = 4 analysis frames |
| Bits per codec frame | 64 (8 bytes) | 64 (8 bytes) |
| Voicing | 1 bit per 10 ms (2 bits) | 1 bit per 10 ms (4 bits) |
| ω0 | 7 bits, once (frame 2) | 7 bits, twice (frames 2 and 4) |
| LPC energy | 5 bits, once (frame 2) | 5 bits, twice (frames 2 and 4) |
| Spectral envelope | 10 LSP differences, 5 bits each (50 bits), once | 10 LSPs, scalar, 36 bits, once (frame 4) |
| Decoder interpolates | ω0, energy, LSPs for frame 1 | ω0 and energy for frames 1 and 3; LSPs for frames 1, 2, 3 |
| M17 stream payload (128 bits per 40 ms) | 2 consecutive codec frames, earlier first | 1 codec frame + 64 data bits |

"Frame k" below counts the 10 ms analysis frames inside one codec frame from 1. The encoder input of one codec frame is
the next 160 (3200) or 320 (1600) samples; the decoder output of one codec frame is 160 or 320 samples.

Delay: each analysis frame describes the speech centered 159 samples (about 20 ms) before the newest input sample
(B[160] against B[319], the middle of a 40 ms buffer, section 5.1). The decoder adds no look-ahead: it outputs a codec
frame's samples as soon as the frame's bits arrive. Its overlap-add places each 10 ms frame's waveform with its time origin at the last output
sample of that frame (section 13.2).

## 3. Conventions and constants

### 3.1 Notation

| Notation | Meaning |
|---|---|
| trunc(x) | round toward zero (conversion of a real value to an integer) |
| ⌊x⌋, ⌈x⌉ | floor, ceiling |
| round(x) | ⌊x + 0.5⌋ unless stated otherwise |
| f32(x) | x rounded to IEEE 754 single precision |
| a[i] | element i of a sequence, 0-based unless stated |
| DFT | X[k] = Σ_{n=0}^{N−1} x[n] e^{−j2πkn/N}, no scaling |
| IDFT | x[n] = Σ_{k=0}^{N−1} X[k] e^{+j2πkn/N}, no scaling (no 1/N) |

The reference computes in single precision (float32) with the exceptions noted where they change a result: several
index and harmonic-count computations are formed in double precision and then truncated, and the constants π and 2π
are the double values below, not the exact ones. An implementation that has to reproduce the reference's integer
decisions (bins, L, quantizer indexes) must follow those notes. See section 15 for what can and cannot be exact.

### 3.2 Constants

| Name | Value | Use |
|---|---|---|
| π_c | 3.141592654 (double) | every use of π in the parameter math below |
| 2π_c | 6.283185307 (double) | every use of 2π in the parameter math below |
| Fs | 8000 Hz | sample rate |
| N | 80 | samples per analysis/synthesis frame |
| M | 320 | analysis buffer length (40 ms) |
| Analysis window | 279-point Hann, 278 points used (n = 21..298 of the buffer) | section 5.2 |
| Ndft | 512 | DFT size for analysis, LPC spectra and synthesis |
| P_min, P_max | 20, 160 samples | pitch period range (400 Hz to 50 Hz) |
| ω0_min | f32(2π_c / 160) = 0.03926991 (0x3D20D97C) | lowest ω0, rad/sample |
| ω0_max | f32(2π_c / 20) = 0.31415927 (0x3EA0D97C) | highest ω0 |
| p | 10 | LPC order |
| Voicing threshold | 6.0 dB | section 7.3 |
| E_min, E_max | −10 dB, 40 dB | energy quantizer range |
| ω0 and energy bits | 7, 5 | scalar quantizer sizes |
| LSP bandwidth expansion | g^i on a_i, g = f32(0.994) | encoder, before LSP conversion (8.4) |
| Post filter β, γ | 0.2, 0.5 | decoder LPC post filter, always on |
| Bass boost | ×1.4·1.4 (double, 1.9599999999999997) on power below 1 kHz | decoder, always on |
| Low-pitch correction | A_1 ×= 0.032 when ω0 < π_c·150/4000 | decoder |
| Background estimator | threshold 40 dB, β 0.1, margin 6 dB | decoder phase post filter |
| Output limiter set point | 30000 | decoder |
| Max harmonics | 79 | L never exceeds 79 in the encoder (7.2) or the decoder (10.2), since ω0_min is slightly above 2π/160; size amplitude and phase arrays for 80. The refinement count L_r (7.1) stores nothing and reaches 82 |

## 4. Bit stream

### 4.1 Packing

Each codec frame is 64 bits sent as 8 bytes. Fields are written in the order below into one 64-bit string, most
significant bit of each field first. Bit 0 of the string is the most significant bit (0x80) of byte 0, bit 7 is the
least significant bit of byte 0, bit 8 is the most significant bit of byte 1, and so on. Fields cross byte boundaries
freely; there is no padding and no spare bit.

Every field, in both modes, is Gray coded before packing: the transmitted value is g = i XOR (i >> 1) for the index
i. The decoder inverts it: i = g XOR (g >> 1) XOR (g >> 2) XOR … (every shift up to the field width). A 1-bit field
is unchanged. There is no option to send natural binary in these two modes.

### 4.2 Mode 3200 (20 ms, 64 bits)

| Bits | Width | Field |
|---|---|---|
| 0 | 1 | voicing, analysis frame 1 |
| 1 | 1 | voicing, analysis frame 2 |
| 2–8 | 7 | ω0 index, frame 2 (section 9.1) |
| 9–13 | 5 | energy index, frame 2 (section 9.2) |
| 14–18 | 5 | LSP difference index 1 (LSP 1 itself) |
| 19–23 | 5 | LSP difference index 2 |
| 24–28 | 5 | index 3 |
| 29–33 | 5 | index 4 |
| 34–38 | 5 | index 5 |
| 39–43 | 5 | index 6 |
| 44–48 | 5 | index 7 |
| 49–53 | 5 | index 8 |
| 54–58 | 5 | index 9 |
| 59–63 | 5 | index 10 |

### 4.3 Mode 1600 (40 ms, 64 bits)

| Bits | Width | Field |
|---|---|---|
| 0 | 1 | voicing, frame 1 |
| 1 | 1 | voicing, frame 2 |
| 2–8 | 7 | ω0 index, frame 2 |
| 9–13 | 5 | energy index, frame 2 |
| 14 | 1 | voicing, frame 3 |
| 15 | 1 | voicing, frame 4 |
| 16–22 | 7 | ω0 index, frame 4 |
| 23–27 | 5 | energy index, frame 4 |
| 28–31 | 4 | LSP 1 index |
| 32–35 | 4 | LSP 2 |
| 36–39 | 4 | LSP 3 |
| 40–43 | 4 | LSP 4 |
| 44–47 | 4 | LSP 5 |
| 48–51 | 4 | LSP 6 |
| 52–55 | 4 | LSP 7 |
| 56–58 | 3 | LSP 8 |
| 59–61 | 3 | LSP 9 |
| 62–63 | 2 | LSP 10 |

The LSPs are those of frame 4.

## 5. Encoder: input buffer, window, DFT

### 5.1 Analysis buffer

The encoder keeps a buffer B[0..319] of the most recent 320 input samples, as real numbers equal to the 16-bit sample
values (no scaling, no DC removal, no high-pass or pre-emphasis filter; the only DC handling is inside the pitch
estimator, section 6.2). At creation every B[n] is 1.0, not 0. For each 10 ms analysis frame the buffer shifts left by
80 (B[n] ← B[n+80] for n = 0..239) and the 80 new samples go into B[240..319]. All analysis of that frame then works on
this buffer; its center sample B[160] is the time origin of the frame.

### 5.2 Analysis window

w[n], n = 0..319, is a 279-point Hann window centered on n = 160, of which the 278 points n = 21..298 are used (the
first of them is 0), and zero elsewhere:

- w[n] = 0.5 − 0.5 cos(2π_c (n − 21) / 278) for n = 21..298, computed with the cosine in single precision of the
  double argument;
- w[n] = 0 for n < 21 and n ≥ 299 (w[21] is 0 by the formula, w[160] = 1 before scaling);
- then every w[n] is multiplied by 1 / sqrt(512 · Σ w[n]²), the sum taken over the unscaled window, so that
  Σ w[n]² = 1/512 after scaling.

The scaled window is table `analysis_window` (320 values). The same window serves the harmonic analysis (5.3) and the
LPC analysis (8.1).

### 5.3 Speech DFT

The windowed buffer is arranged with its center at DFT index 0 and transformed with a 512-point complex DFT (real
input, zero imaginary part):

- x[k] = B[160 + k] · w[160 + k] for k = 0..138;
- x[373 + k] = B[21 + k] · w[21 + k] for k = 0..138 (that is, x[512 + d] = B[160 + d] · w[160 + d] for d = −139..−1);
- x[k] = 0 for k = 139..372.

S_w(k) = DFT_512(x)(k), k = 0..511. Bins above 256 are the conjugates of the bins below; the analysis reads bins
0..341 at most (pitch refinement can index beyond 256, section 7.1, and must read the true DFT value there, which is
the mirror image |S_w(512 − k)|).

### 5.4 Window spectrum

W(k), k = 0..511, is the real part of the 512-point DFT of the scaled window arranged the same way (w[160 + d] at index
d mod 512 for d = −139..138), rotated by half the size so that index 256 holds DC: W(256 + d) = Re DFT(d mod 512). The
arranged window is even, so the imaginary part is zero. Table `analysis_window_dft` holds the 512 values. It is used
only by the voicing estimator (7.3).

## 6. Encoder: pitch estimator

The pitch estimator works on the unwindowed buffer B and produces a fundamental f0 in Hz on a 3.125 Hz grid between
50 and 400 Hz. It has its own state: a 320-value history q[0..319] (all 0 at creation), the notch filter memories
(input and output, 0 at creation), a 48-sample FIR delay line (0 at creation) and the previous estimate f0_prev
(50.0 Hz at creation).

### 6.1 Squaring

For the 80 new samples, n = 240..319: q[n] = B[n]². (q[0..239] hold the filtered values of earlier frames.)

### 6.2 DC notch

Each new value passes through the notch H(z) = (1 − z⁻¹)/(1 − 0.95 z⁻¹), in time order, with memories carried from
frame to frame: y = x − x_prev + 0.95 · y_prev, where x_prev is the previous notch input and y_prev the previous notch
output. Precision: y = f32(f32(x − x_prev) + 0.95 · y_prev), the difference rounded to single precision first and
the rest in double (0.95 is the double constant). Doing it all in single precision drifts the history enough to flip
a sub-multiple choice on a sweep. The value stored back into q[n] is y + 1.0. The constant 1.0 is added after the
recursion; the recursion memory keeps y without it.

### 6.3 Low-pass FIR

Each of the 80 notched values, in time order, enters a 48-tap FIR low-pass filter (cut-off about 600 Hz) with delay line
memory carried between frames; q[n] is replaced by the filter output Σ_{j=0}^{47} h[j] · u[n − 47 + j], where u is the
FIR's input sequence (the notched values, the oldest of the 48 at j = 0). The taps h are table `nlp_lpf_fir`, listed in
14.3; they are symmetric, so the orientation does not matter. Accumulate in increasing j.

### 6.4 Decimation, window and DFT

Take every fifth value of the whole history, d[i] = q[5i] for i = 0..63 (the decimated 1.6 kHz signal spanning the full
40 ms), multiply by the window g[i] = 0.5 − 0.5 cos(2π_c i / 63) (table `nlp_dft_window`, 64 values), place the 64
products at the start of a 512-point complex input with zero padding, and compute F(k) = |DFT_512(k)|², the power
spectrum. Bin k is k · 1600/512 = k · 3.125 Hz.

### 6.5 Global peak

Search k = 16..128 inclusive (50 Hz to 400 Hz; 16 = 2560/160 and 128 = 2560/20 in integer division) for the largest
F(k). Ties keep the lowest bin; the search starts with a running maximum of 0 at bin 16, so a frame where every F(k) is
0 returns bin 16. Call the result k_g and its value F_g.

### 6.6 Sub-multiple search

The global peak may be a multiple of the true fundamental. Starting from the candidate k_c = k_g, for each divisor
m = 2, 3, 4, … while trunc(k_g / m) ≥ 16 (integer division):

1. b = trunc(k_g / m) (integer division); search interval b_lo = trunc(0.8 · b), b_hi = trunc(1.2 · b), then raise b_lo
   to 16 if lower.
2. Previous-pitch bias: let k_p = trunc(f32(f0_prev · 2560) / 8000) (single precision product and quotient, then
   truncate). If b_lo < k_p < b_hi (both strict) the threshold is T = 0.15 · F_g, else T = 0.3 · F_g.
3. Find the largest F(k) for k = b_lo..b_hi inclusive (running maximum from 0 with the bin preset to b_lo, ties keep
   the lowest bin): value F_l at bin k_l.
4. If F_l > T and F_l > F(k_l − 1) and F_l > F(k_l + 1) (a strict local maximum), set k_c = k_l.

All divisors are tried; the candidate from the last divisor that passes wins (so the lowest qualifying sub-multiple is
chosen). The thresholds always refer to the global peak F_g.

f0 = f32(k_c · 8000 / 2560) = k_c · 3.125 Hz. The pitch period is P = f32(8000 / f0) samples. f0_prev ← f0.

### 6.7 Shift

After the estimate, the history shifts left by 80: q[n] ← q[n + 80] for n = 0..239.

## 7. Encoder: pitch refinement, harmonic amplitudes, voicing

### 7.1 Pitch refinement

The initial fundamental is ω0 = f32(2π_c / P). Refinement maximizes the harmonic sum

  E(ω) = Σ_{m=1}^{L_r} |S_w(b_m)|²

over candidate pitch periods, in two passes, where for a candidate ω the bins are formed by repeated single-precision
addition: c = f32(ω · f32(1/f32(2π_c/512))), c_1 = c, c_{m+1} = f32(c_m + c), and b_m = trunc(c_m + 0.5).

Each pass, given the current ω0:

1. L_r = trunc(π_c / ω0), fixed for the pass.
2. Candidate periods run from p_lo = f32(2π_c/ω0 − Δ) to p_hi = f32(2π_c/ω0 + Δ) in steps of s, by repeated
   single-precision addition starting at p_lo, while the candidate is ≤ p_hi.
3. For each candidate period q, the candidate is ω = f32(2π_c / q). The candidate with the largest E(ω) wins; ties keep
   the earlier (shorter period) candidate; a candidate must beat a running maximum that starts at 0, and if none does,
   ω0 is unchanged.

| Pass | Δ | s | Candidates |
|---|---|---|---|
| Coarse | 5 samples | 1.0 | 11 |
| Fine | 1 sample | 0.25 | 9 |

The fine pass starts from the coarse result. Afterwards ω0 is clamped to [ω0_min, ω0_max] (2π_c/160 and 2π_c/20).

### 7.2 Harmonic count and amplitudes

L = ⌊π_c / ω0⌋ (quotient in double, floor of its single-precision value). If f32(ω0 · L) ≥ 0.95 π_c, L is reduced by 1.
So a harmonic within 5% of the Nyquist frequency is dropped.

For each harmonic m = 1..L, with r = f32(1 / f32(2π_c/512)) (bins per radian):

- a_m = trunc((m − 0.5) · ω0 · r + 0.5), b_m = trunc((m + 0.5) · ω0 · r + 0.5), formed in double precision;
- A_m = sqrt(Σ_{k=a_m}^{b_m−1} |S_w(k)|²).

With the window scaling of 5.2, a steady sinusoid of amplitude a gives A ≈ a/2.

### 7.3 Voicing

The voicing estimator fits, in each harmonic band below 1 kHz, a single sinusoid shaped by the window spectrum and
measures the fit:

- m_1k = trunc(L · 1000/4000) (= trunc(L/4)).
- Signal: S = 10⁻⁴ + Σ_{m=1}^{m_1k} A_m².
- For each m = 1..m_1k:
  - band edges a = ⌈f32((m − 0.5) · ω0 · 512 / 2π_c)⌉ and b = ⌈f32((m + 0.5) · ω0 · 512 / 2π_c)⌉ (products in
    double, rounded to single precision, then ceiling); note the ceiling here differs from the rounding of 7.2;
  - offset o = trunc(256 − f32(f32(m · ω0) · 512) / 2π_c + 0.5) (the division and the rest in double), so that
    W(o + k) is the window spectrum centered on the harmonic's nearest bin;
  - complex amplitude B_m = Σ_{k=a}^{b−1} S_w(k) W(o + k) / Σ_{k=a}^{b−1} W(o + k)², real and imaginary parts
    separately;
  - error contribution Σ_{k=a}^{b−1} |S_w(k) − B_m W(o + k)|².
- Error: D = 10⁻⁴ + the sum of all contributions.
- SNR = 10 log10(S / D). The frame is voiced (v = 1) if SNR > 6.0 dB, else unvoiced.

Post-processing on the band energy ratio, with m_2k = trunc(L · 2000/4000) and m_4k = trunc(L · 4000/4000) = L:

- E_lo = 10⁻⁴ + Σ_{m=1}^{m_2k} A_m², E_hi = 10⁻⁴ + Σ_{m=m_2k}^{m_4k} A_m² (harmonic m_2k counts in both);
- R = 10 log10(E_lo / E_hi).
- If v = 0 and R > 10 dB, set v = 1.
- Then, if v = 1: set v = 0 if R < −10 dB; also set v = 0 if R < −4 dB and ω0 ≤ f32(60 · 2π_c / 8000) (60 Hz).

## 8. Encoder: LPC analysis, LSPs, energy

LPC analysis runs only in the analysis frames that send energy and LSPs (frame 2 for 3200; frames 2 and 4 for 1600),
on the same buffer B as that frame's harmonic analysis.

### 8.1 Autocorrelation

x[n] = B[n] · w[n], n = 0..319 (the analysis window of 5.2, so only n = 21..298 are non-zero). If Σ x[n]² is exactly
0, skip 8.1–8.4: the energy is E = 0 and the LSPs are ω_i = (π_c/10) · i for i = 0..9 (the first is 0; these feed
only the LSP quantizer). Otherwise R[j] = Σ_{n=0}^{319−j} x[n] x[n+j] for j = 0..10.

Accumulate each R[j] in single precision, in increasing n, each product rounded to single precision before the add
(no fused multiply-add, no pairwise or double-precision summation). This matters: for steady tones and DC the
prediction error E is a small difference of large terms, and the energy and LSP indexes then depend on the exact
rounding of R. A model that sums R in double precision disagrees with the reference on about 99% of those frames; the
sequential single-precision sum agrees on all of them.

### 8.2 Levinson-Durbin

A(z) = 1 + Σ_{i=1}^{10} a_i z^{−i}, the prediction error filter (note the plus sign). With e = R[0], for i = 1..10:

- k_i = −(R[i] + Σ_{j=1}^{i−1} a_j^{(i−1)} R[i − j]) / e;
- if |k_i| > 1, k_i = 0;
- a_i^{(i)} = k_i, a_j^{(i)} = a_j^{(i−1)} + k_i a_{i−j}^{(i−1)} for j = 1..i−1;
- e ← e (1 − k_i²).

a_0 = 1, a_i = a_i^{(10)}.

### 8.3 Energy

E = Σ_{i=0}^{10} a_i R[i], summed in single precision in increasing i, from the coefficients before bandwidth
expansion. The Levinson sums of 8.2 are likewise single precision in increasing j. This is the transmitted LPC
energy (prediction error power, in the units of the scaled window). It is not clamped; a negative or zero value
quantizes to index 0 (section 9.2).

### 8.4 Bandwidth expansion

a_i ← f32(a_i · c_i), i = 0..10 (about 15 Hz of bandwidth expansion), after the energy and before the LSP
conversion. The factor is c_i = f32(g^i) with g = f32(0.994) = 0.994000018 (0x3F7E76C9), the single-precision constant
raised to the power i and then rounded. It is not f32(0.994^i): for i ≥ 2 the two differ by 1 to 3 ulp, and that
moves an LSP index on a few frames of a vibrato vowel or a sweep. The factors:

| i | c_i | bits | i | c_i | bits |
|---|---|---|---|---|---|
| 0 | 1 | 0x3F800000 | 6 | 0.964535773 | 0x3F76EBD1 |
| 1 | 0.994000018 | 0x3F7E76C9 | 7 | 0.958748579 | 0x3F75708C |
| 2 | 0.988036036 | 0x3F7CEFEE | 8 | 0.952996135 | 0x3F73F78E |
| 3 | 0.982107818 | 0x3F7B6B6B | 9 | 0.947278142 | 0x3F7280D2 |
| 4 | 0.976215184 | 0x3F79E93D | 10 | 0.941594541 | 0x3F710C57 |
| 5 | 0.970357955 | 0x3F786961 | | | |

### 8.5 LPC to LSP

The LSPs are the roots, on the unit circle, of the symmetric and antisymmetric polynomials of A(z), found with a
Chebyshev grid search. With the expanded a_0..a_10:

- reduced polynomial coefficients, P′_0 = Q′_0 = 1 and for i = 1..5:
  P′_i = a_i + a_{11−i} − P′_{i−1}, Q′_i = a_i − a_{11−i} + Q′_{i−1}
  (these are P(z)/(1 + z⁻¹) and Q(z)/(1 − z⁻¹), where P(z) = A(z) + z⁻¹¹A(z⁻¹) and Q(z) = A(z) − z⁻¹¹A(z⁻¹));
- coefficients c_0..c_4 are doubled, c_5 is not: c = (2P′_0, 2P′_1, 2P′_2, 2P′_3, 2P′_4, P′_5), likewise for Q′;
- evaluation at x = cos ω: C(x) = c_5 + c_4 T_1(x) + c_3 T_2(x) + c_2 T_3(x) + c_1 T_4(x) + c_0 T_5(x), with the
  Chebyshev polynomials T_0 = 1, T_1 = x, T_i = 2x T_{i−1} − T_{i−2}, summed from the T_0 term up.

Root search, ten roots in descending x (ascending ω), alternating P′ for roots 1, 3, 5, 7, 9 and Q′ for roots 2, 4,
6, 8, 10:

1. Start with the left point x_l = 1.0 for root 1; for every later root, x_l is the previous root.
2. Step a right point x_r = f32(x_l − f32(0.01)). If C(x_r) · C(x_l) < 0, or C(x_r) = 0, a root lies in
   [x_r, x_l]: bisect it six times (midpoint x_m = (x_l + x_r)/2; if C(x_m) · C(x_l) > 0 the midpoint replaces x_l,
   otherwise it replaces x_r). The root is the last midpoint x_m. Otherwise x_l ← x_r and step again.
3. Stepping for a root continues only while the most recent x_r is ≥ −1 (the test comes before each step, so the last
   grid point tried may lie just below −1). Once a polynomial runs off the end without a sign change, no later root is
   found either.

ω_i = acos(x_i). If fewer than 10 roots were found, the LSPs are replaced by ω_i = (π_c/10) · (i − 1), i = 1..10.
On the test inputs (section 15) every non-zero frame finds all 10 roots.

## 9. Encoder: quantizers and frame assembly

### 9.1 ω0

Uniform in ω0 over [ω0_min, ω0_max) with 128 levels: n = f32((ω0 − ω0_min)/(ω0_max − ω0_min)),
index = ⌊f32(f32(128 · n) + 0.5)⌋, clamped to 0..127. The decoder level for index i is ω0_min + i · s with
s = f32((ω0_max − ω0_min)/128) = 0.002147573 (0x3B0CBE4C), computed in single precision; table `wo_dequant`. The levels
are the quantizer's reproduction points (rounding, not truncation, in the encoder).

### 9.2 Energy

Uniform in dB over [−10, 40) with 32 levels of 1.5625 dB: e_dB = f32(10 · f32(log10 E)), n = f32((e_dB + 10)/50),
index = ⌊f32(f32(32 · n) + 0.5)⌋, clamped to 0..31. If E ≤ 0 (log undefined) the index is 0. The decoder level is
E = 10^((−10 + 1.5625 i)/10); table `energy_dequant` (index 0 is 0.1, index 31 is 6978.3).

### 9.3 LSPs, mode 1600

Each LSP is converted to Hz, f_i = f32((4000/π_c) · ω_i), and quantized on its own to the nearest level of its table
(`lsp1600_q1` … `lsp1600_q10`, section 14.1): the level minimizing (level − f_i)², ties keeping the lower index. No
weighting, no ordering constraint.

### 9.4 LSP differences, mode 3200

The LSPs in Hz (as 9.3) are coded as differences from the previous quantized LSP, closing the loop through the
quantized values:

- d_1 = f_1; index_1 = the nearest level of `lspd3200_q1`; f̂_1 = that level.
- For i = 2..10: d_i = f_i − f̂_{i−1}; index_i = the nearest level of `lspd3200_qi`; f̂_i = f̂_{i−1} + that level.

"Nearest" is the minimum squared error, ties to the lower index. All levels are positive (25 Hz and up), so the decoded
LSPs are strictly increasing; a negative difference takes the smallest level.

### 9.5 Frame assembly

Every 10 ms analysis frame runs sections 5–7 (buffer shift, DFT, pitch, refinement, amplitudes, voicing). The fields
come from:

| Mode | Frame | Sends |
|---|---|---|
| 3200 | 1 | voicing |
| 3200 | 2 | voicing; ω0 (9.1); LPC analysis (8) → energy (9.2) and LSP differences (9.4) |
| 1600 | 1 | voicing |
| 1600 | 2 | voicing; ω0; LPC analysis → energy (its LSPs are not sent) |
| 1600 | 3 | voicing |
| 1600 | 4 | voicing; ω0; LPC analysis → energy and LSPs (9.3) |

ω0 is sent whatever the voicing. Fields are packed in the order of section 4.

## 10. Decoder: dequantization and interpolation

### 10.1 Decoder state

| State | Initial value |
|---|---|
| Previous frame ω0, voicing | ω0_min, unvoiced |
| Previous LPC energy | 1.0 |
| Previous LSPs | ω_i = f32(π_c · (i − 1) / 11), i = 1..10 |
| Excitation phase φ_ex | 0 |
| Background noise estimate | 0 dB |
| Overlap-add memory | 79 zeros (section 13.2) |
| Random generator | seed 1 (section 12.3) |

"Previous" means the last analysis frame of the previous codec frame (frame 2 for 3200, frame 4 for 1600).

### 10.2 Dequantization

- Voicing bits as received.
- ω0 for the frames that carry it: ω0 = ω0_min + i · s (9.1, table `wo_dequant`).
- Harmonic count for any frame in the decoder: L = trunc(π_c / ω0), quotient in double. This differs from the
  encoder's L (no 5% Nyquist rule). For ω0 = ω0_min it gives 79 (ω0_min is slightly above 2π/160); table
  `wo_dequant_L` lists L for every index.
- Energy: table `energy_dequant`.
- LSPs, 3200: f̂_1 = level of index 1, f̂_i = f̂_{i−1} + level of index i (single precision, increasing i),
  ω_i = f32((π_c/4000) · f̂_i). No reordering or spacing rule. Random bits can push the upper LSPs past π (the sum of
  the largest levels is 9800 Hz); use them as they are.
- LSPs, 1600: ω_i = f32((π_c/4000) · level_i), then two fixes, in this order:
  1. Ordering. Scan pairs i = 2..10 (1-based) in increasing i. When ω_i < ω_{i−1}: set ω_{i−1} ← ω_i − 0.1 and
     ω_i ← (old ω_{i−1}) + 0.1 (radians, single precision), then resume the scan at the pair i = 3 (the pair (2, 1) is
     not re-examined after a fix). The scan ends when it passes i = 10.
  2. Minimum spacing. For i = 2, 3, 4: if ω_i − ω_{i−1} < 50 · π_c/4000, set ω_i = ω_{i−1} + 50 · π_c/4000. For
     i = 5..10 the same with 100 Hz. In increasing i. The difference ω_i − ω_{i−1} is a single-precision subtraction
     (exact for these values); the threshold, the comparison and the sum are in double; the result is stored in single
     precision.

### 10.3 Interpolation

For each interpolated frame (3200: frame 1, between the previous frame and frame 2; 1600: frame 1 between the previous
frame and frame 2, frame 3 between frames 2 and 4):

- Voicing: the received bit, except that a voiced frame between two unvoiced neighbors becomes unvoiced.
- ω0: if the (corrected) frame is unvoiced, ω0 = ω0_min. If voiced: both neighbors voiced, ω0 = (ω0_prev + ω0_next)/2;
  only the next voiced, ω0 = ω0_next; only the previous voiced, ω0 = ω0_prev. L from 10.2.
- Energy: E = f32(sqrt(f32(E_prev · E_next))), the product rounded to single precision before the square root.

LSPs are interpolated linearly from the previous codec frame's final LSPs to this frame's, per element:
ω = f32((1 − t) ω_prev + f32(t ω_next)), with t = 0.5 for 3200 frame 1 and t = 0.25, 0.5, 0.75 for 1600 frames 1, 2,
3. The product t ω_next is rounded to single precision before the sum; (1 − t) ω_prev and the sum are double. Only
t = 0.75 is affected (the other products are exact); computing both products in double gives 1-ulp LSP differences
in a large share of the 1600 frame-3 interpolations (90% of them on random bits). Frames
that carry their own ω0 keep it and their received voicing; frame 2 of 1600 uses its own ω0 and energy with
interpolated LSPs.

After the codec frame, the state takes the last frame's ω0, voicing, energy and final LSPs (after the 1600 fixes).

### 10.4 LSP to LPC

A(z) = (P(z) + Q(z))/2 with

  P(z) = (1 + z⁻¹) Π_{i odd} (1 − 2 cos(ω_i) z⁻¹ + z⁻²), Q(z) = (1 − z⁻¹) Π_{i even} (1 − 2 cos(ω_i) z⁻¹ + z⁻²),

i 1-based over 1..10 (ω_1, ω_3, ω_5, ω_7, ω_9 in P). a_0..a_10 are the coefficients of z⁰..z⁻¹⁰ (a_0 = 1). Compute in
single precision; any evaluation order (product expansion or impulse response of the cascade) is within tolerance.

## 11. Decoder: harmonic amplitudes from the LPC model, post filter

For each 10 ms frame, from its a_0..a_10, energy E and ω0:

### 11.1 LPC spectrum

H_A(k) = DFT_512 of (a_0, …, a_10, 0, …, 0), k = 0..256. The power spectrum is P(k) = 1/(|H_A(k)|² + 10⁻⁶) for
k = 0..255. H_A is also kept for phase synthesis (12.1).

### 11.2 Post filter (always on)

- H_W(k) = DFT_512 of (a_0, a_1 γ, a_2 γ², …, a_10 γ¹⁰, 0, …) with γ = 0.5 (A(z/γ)); |H_W(k)|² for k = 0..255.
- R(k) = sqrt(|H_W(k)|² · P(k)) (= |A(z/γ)/A(z)| on the unit circle).
- E_before = 10⁻⁴ + Σ_{k=0}^{255} P(k).
- P(k) ← P(k) · (R(k)^β)², β = 0.2.
- E_after = 10⁻⁴ + Σ_{k=0}^{255} P(k) (after the step above).
- g = f32(f32(E_before / E_after) · E), then P(k) ← P(k) · g for all k (energy normalized to the unfiltered spectrum,
  then scaled by the decoded energy).
- Bass boost: P(k) ← f32(P(k) · 1.4 · 1.4) for k = 0..63 (0 to 1 kHz, +2.9 dB), the factor 1.4 · 1.4 formed in
  double (1.9599999999999997, not the double nearest 1.96).

### 11.3 Sampling the harmonics

For m = 1..L, with r_d = f32(2π_c/512): a_m = trunc((m − 0.5) ω0 / r_d + 0.5), b_m = trunc((m + 0.5) ω0 / r_d + 0.5)
(double), b_m limited to 256, and A_m = sqrt(Σ_{k=a_m}^{b_m−1} P(k)).

### 11.4 Low-pitch correction

If ω0 < π_c · 150/4000 (150 Hz), A_1 ← 0.032 · A_1.

## 12. Decoder: phase synthesis and the random generator

No phases are sent. Each harmonic's phase is the phase of an excitation passed through the LPC synthesis filter.

### 12.1 Filter phase

For m = 1..L: b = trunc(f32(f32(m · ω0) / r_d) + 0.5) and H_m = conj(H_A(b)). The conjugate is used because the phase
of the all-pole filter 1/A(z) is the negative of the phase of A(z); only the phase of H_m matters.

### 12.2 Excitation and harmonic phases

Every frame, voiced or not, the excitation phase of the fundamental advances by the frame length:
φ_ex ← f32(φ_ex + f32(ω0 · 80)), then wraps to about [−π, π]: φ_ex ← f32(φ_ex − 2π_c ⌊f32(φ_ex / 2π_c + 0.5)⌋), the
subtraction in double. φ_ex is a running sum that never resets, so keep it in single precision with exactly this
update: kept in double it drifts away from the reference's value, and voiced output then loses waveform match (SNR
against the reference falls to about 20 dB on a long vowel) while sounding the same.

For m = 1..L:

- voiced: excitation X_m = e^{j m φ_ex} (cosine and sine of f32(m · φ_ex));
- unvoiced: X_m = e^{jθ} with θ = 2π_c · r / 32767 for a fresh random value r (12.3), one draw per harmonic in
  increasing m;
- φ_m = atan2(Im(H_m X_m), Re(H_m X_m) + 10⁻¹²).

### 12.3 Random generator

A 32-bit linear congruential generator: state s ← (1103515245 · s + 12345) mod 2³², output r = (s >> 16) AND 0x7FFF
(0..32767). The state starts at 1. The first outputs are 16838, 5758, 10113, 17515, 31051 (table `rng_first64`).

The reference keeps one generator for the whole process, shared by every decoder instance. A firmware decoder keeps its
own, seeded with 1 when the decoder is created; that is what the test harness compares against (one decoder per
process).

Draw order within a 10 ms frame: the unvoiced excitation draws (12.2) come before the phase post filter draws (12.4);
a frame uses only one of the two.

### 12.4 Phase post filter (background noise)

After 12.2:

- e = 10 log10((10⁻¹² + Σ_{m=1}^{L} A_m²) / L).
- If e < 40 and the frame is unvoiced, the background estimate b ← 0.9 b + 0.1 e.
- Threshold T = 10^((b + 6)/20).
- If the frame is voiced: for m = 1..L in increasing m, if A_m < T, replace φ_m with (2π_c/32767) · r for a fresh
  random value r.

## 13. Decoder: sinusoidal synthesis and output

### 13.1 Frame waveform

Build a half spectrum Y(k), k = 0..256, all zero, then for m = 1..L in increasing m set Y(b_m) = A_m e^{jφ_m} with
b_m = trunc(f32(f32(m · ω0) · 512) / 2π_c + 0.5) (last division in double), limited to 255. A later harmonic that lands
on the same bin overwrites the earlier one. The frame waveform is the real 512-point IDFT with conjugate-symmetric
extension and no 1/N:

  y[n] = Re Y(0) + Re Y(256) (−1)ⁿ + 2 Σ_{k=1}^{255} Re(Y(k) e^{j2πkn/512}), n = 0..511,

so y[n] = Σ_m 2 A_m cos(2π b_m n/512 + φ_m). Note the harmonic frequencies are rounded to the 15.625 Hz bin grid.
y is periodic in 512; y[512 − d] is the waveform at time −d.

### 13.2 Overlap-add

t[i], i = 0..159, is the triangular window t[i] ≈ i/80 for i < 80 and (160 − i)/80 for i ≥ 80, generated in the
reference by adding f32 steps of 1/80 (table `synthesis_window`; t[0] = 0, t[80] = 1, t[159] ≈ 1/80).

The decoder keeps a memory o[0..78] from the previous frame (zeros at creation). The 80 output samples of the frame are

- out[i] = o[i] + y[433 + i] · t[i] for i = 0..78 (y[433 + i] is the waveform at time i − 79);
- out[79] = y[0] · t[79];

and the new memory is o[i] = y[1 + i] · t[80 + i] for i = 0..78. So each frame's waveform has its time origin at the
frame's last output sample, rises over this frame and falls over the next one. The tail sample y[80] · t[159] is
dropped (the memory holds 79 values, not 80).

### 13.3 Limiter and conversion

On the 80 output values: let x_max be the largest value (signed, starting from 0, so negative peaks are not
considered). If x_max > 30000, multiply all 80 values by g = f32(1/f32(u · u)) with u = f32(x_max/30000): a
peak x dB over the set point is attenuated by 2x dB and ends x dB under it. Then clamp to [−32767, 32767] and convert
to 16-bit by truncation toward zero. The memory o of 13.2 is not affected by the limiter.

## 14. Tables

`tables.json` maps each name to `{dims, type, count, sum, sha256, values, note}`. The checksum is SHA-256 over the
values packed as little-endian IEEE 754 float32 (int32 for integer tables), in order. Sums are of the stored float32
values.

### 14.1 LSP levels, mode 1600 (Hz)

| Table | Bits | Levels |
|---|---|---|
| lsp1600_q1 | 4 | 225 to 600 step 25 (16) |
| lsp1600_q2 | 4 | 325 to 700 step 25 (16) |
| lsp1600_q3 | 4 | 500 to 1250 step 50 (16) |
| lsp1600_q4 | 4 | 700 to 2200 step 100 (16) |
| lsp1600_q5 | 4 | 950 to 2450 step 100 (16) |
| lsp1600_q6 | 4 | 1100 to 2600 step 100 (16) |
| lsp1600_q7 | 4 | 1500 to 3000 step 100 (16) |
| lsp1600_q8 | 3 | 2300 to 3000 step 100 (8) |
| lsp1600_q9 | 3 | 2500 to 3200 step 100 (8) |
| lsp1600_q10 | 2 | 2900, 3100, 3300, 3500 |

Index i of a table selects the i-th level from the lowest (index 0 is the first value listed).

### 14.2 LSP difference levels, mode 3200 (Hz)

All ten tables have 32 levels (5 bits).

| Tables | Levels |
|---|---|
| lspd3200_q1, q2, q3, q7, q8, q9, q10 | 25 to 800 step 25 |
| lspd3200_q4, q5, q6 | 25 to 200 step 25 (8 levels), then 250 to 1400 step 50 (24 levels) |

### 14.3 Pitch estimator FIR (48 taps, symmetric; h[47 − j] = h[j])

| j | h[j] | j | h[j] |
|---|---|---|---|
| 0 | −1.0818124e−03 | 12 | −1.1705810e−02 |
| 1 | −1.1008344e−03 | 13 | −1.8199275e−02 |
| 2 | −9.2768838e−04 | 14 | −2.2065282e−02 |
| 3 | −4.2289438e−04 | 15 | −2.0920610e−02 |
| 4 | 5.5034190e−04 | 16 | −1.2808831e−02 |
| 5 | 2.0029849e−03 | 17 | 3.2204775e−03 |
| 6 | 3.7058509e−03 | 18 | 2.6683811e−02 |
| 7 | 5.1449415e−03 | 19 | 5.5520624e−02 |
| 8 | 5.5924666e−03 | 20 | 8.6305944e−02 |
| 9 | 4.3036754e−03 | 21 | 1.1480192e−01 |
| 10 | 8.0284511e−04 | 22 | 1.3674206e−01 |
| 11 | −4.8204610e−03 | 23 | 1.4867556e−01 |

The decimal values round to the float32 values in `tables.json`.

### 14.4 Table index

| Name | Type, count | Sum | SHA-256 (first 16 hex) | Content |
|---|---|---|---|---|
| lsp1600_q1 … q10 | float32, 16/16/16/16/16/16/16/8/8/4 | 6600, 8200, 14000, 23200, 27200, 29600, 36000, 21200, 22800, 12800 | see tables.json | 14.1 |
| lspd3200_q1–q3, q7–q10 | float32, 32 each | 13200 | 2bcef849c53a88c7 | 14.2 |
| lspd3200_q4–q6 | float32, 32 each | 20700 | d79f93d1e888c0f4 | 14.2 |
| nlp_lpf_fir | float32, 48 | 1.00000001 | 59c2bcc9acfc0b37 | 14.3 |
| analysis_window | float32, 320 | 0.601647003 | 39adf80c60326fcf | 5.2 |
| analysis_window_dft | float32, 512 | 2.21613859 | 8d8fe2426cec4d32 | 5.4 |
| synthesis_window | float32, 160 | 80.000017 | 4ffcaae2d04e88f6 | 13.2 |
| nlp_dft_window | float32, 64 | 31.4999999 | 242bec43d35bc936 | 6.4 |
| wo_dequant | float32, 128 | 22.4820221 | 0f0882db6bf433e6 | 9.1 |
| wo_dequant_L | int32, 128 | 3012 | 06ddc64cc43537e6 | 10.2 |
| energy_dequant | float32, 32 | 23093.7867 | 5df232bf755e8587 | 9.2 |
| rng_first64 | int32, 64 | 1110943 | 32b6a9017bdcf7a7 | 12.3 |

The LSP and FIR tables are the only values taken from the reference; the others follow from the formulas and are given
so an implementation can check its start-up computation or store them instead.

## 15. Numerical precision, fixed point and test targets

### 15.1 What must be exact

- Bit stream: field order, widths, Gray code (section 4). Exact.
- Dequantization from bits: ω0, L, energy, LSPs, the 1600 ordering and spacing fixes, interpolation weights and the
  interpolated voicing (sections 10.2–10.3). Exact in single precision with the tables; L exactly as `wo_dequant_L`
  (and trunc(π_c/ω0) in double for interpolated ω0).
- The random generator and its draw order (12.3): exact, or unvoiced and background-noise phases differ.

### 15.2 What is close, not exact

Decoder output depends on DFT round-off, cos/sin/atan2/exp/log precision and the order of sums, so PCM is not
bit-exact against the reference even in float32. Integer decisions inside the decoder must still match, or the output
diverges for good:

| Decoder decision | What a mismatch does |
|---|---|
| L of every frame (10.2), including 79 at ω0_min | changes the number of random draws; every later random phase differs |
| A_m < T in the phase post filter (12.4) | same: one extra or missing draw desynchronizes the generator |
| Background estimate b (12.4) | accumulates; keep it single precision with the stated update |
| φ_ex (12.2) | accumulates; double precision drifts from the reference |
| Harmonic bins (11.3, 12.1, 13.1) | a different bin moves a harmonic by 15.625 Hz |

The encoder's decisions are thresholds on computed values, so arithmetic differences flip some of them:

| Encoder decision | Where it is fragile |
|---|---|
| Pitch peak and sub-multiple choice (6.5, 6.6) | near-equal spectral peaks, F_l near the threshold (noise, sweeps); a flip moves ω0 by a factor and then everything downstream |
| Refinement argmax (7.1) | flat harmonic sums (noise, silence) |
| L and the 5% Nyquist rule (7.2) | ω0 near π/L |
| Voicing SNR and ratio rules (7.3) | SNR near 6 dB, R near ±10 or −4 dB |
| LPC energy and LSPs (8.1–8.5) | steady tones and DC: E is round-off decided unless R is summed as 8.1 says |
| Pitch estimator notch (6.2) | the mixed single/double rounding of the notch; all-single arithmetic flips a sub-multiple on sweeps |
| Bandwidth expansion (8.4) | the factors c_i; f32(0.994^i) instead moves LSP indexes on a few frames |
| LPC root search (8.5) | sign tests near a root; six bisections fix x to about 0.0002 |
| ω0, energy and LSP indexes (9.1–9.4) | values near a level midpoint; 3200 difference coding carries a flip into the later LSPs |

Measured with an independent model written from this page alone (DFTs in double precision; the encoder otherwise in
single precision following the notes above; the decoder's parameter decoding, φ_ex and bin indexes in single precision
and its spectra, amplitudes and background estimate in double) against the reference, on the synthetic inputs of the
harness (tones, sweeps, pulse trains, synthetic vowels, noise, clipping, silence, random and swept bit streams; 4686
codec frames):

| Measure | Result |
|---|---|
| Encoder frames bit-exact | 100% (4686 of 4686) |
| Decoded parameters (ω0, L, E, LSPs, voicing) and random draws per frame | 100% exact on every input |
| Decoder SNR against the reference | 47–89 dB on every input except silence (39 dB at a few LSB) and the swept bit stream in 3200 |
| Decoder log spectral distance | ≤ 0.06 dB mean on every input except the swept bit stream in 3200 (1.0 dB) |

The swept bit stream sends every byte value in every position; its 3200 all-zero frames decode to ten LSPs below
250 Hz, an LPC filter so ill-conditioned that float32 round-off in the reference decides the output. It is not a
target.

The same model with all arithmetic in double precision (a stand-in for "correct but different arithmetic") reaches
64% bit-exact encoder frames: 100% on the pulse trains and most vowels, 98–100% on noise, 22–80% on the 90 Hz
vowel, near 0% on steady tones, DC and sweeps, where the LPC energy and LSPs are round-off decided. Its decoder, fed
exact parameters, keeps the spectra (log spectral distance under 0.7 dB on voiced inputs) but loses waveform match
(SNR 20–80 dB) through φ_ex drift and random-generator desynchronization.

### 15.3 Fixed point

The HR_C7000 platform's CK803S has no FPU (float is emulated in software); the STM32F4 has a single-precision FPU. A
float32 implementation that follows the precision notes can be bit-exact on nearly every encoder frame and
parameter-exact in the decoder. A fixed-point implementation (like the MBE vocoder in `core/mbe`, on
`core/dsp/fxp.h`) cannot be bit-exact on the encoder side and does not need to be: the bit stream it produces is valid
Codec 2 for any decoder. Its targets are the harness's "fixed" gate (`HARNESS.md`): exact or near-exact parameter
decode, decoded spectra within 1 dB of the reference, encoder indexes mostly within one step on speech-like input.

Where a fixed-point design needs headroom:

| Quantity | Range |
|---|---|
| Analysis buffer, DFT input | 16-bit samples times a window peaking at 0.00433 (w[160]) |
| Squared signal in the pitch estimator | 32-bit squares of 16-bit samples, plus the notch gain |
| Autocorrelation R[0] and LPC energy E | over 60 dB of range; E is a small difference for predictable signals |
| Levinson recursion | reflection coefficients near ±1 for peaky spectra |
| LSP polynomial near the roots | the sign test decides the grid interval |
| Decoder power spectrum P(k) | over 80 dB between formant peaks and valleys |
| Harmonic amplitudes A_m | from below 1 to over 10⁵ (limited only at the output) |

## 16. What this firmware does: tables and bits

The part the encoder and the decoder share, in `core/codec2`: the bit stream of section 4, the quantizer levels and
the dequantizers of sections 9 and 10.2. Integers only: no float, no 64-bit product, one 32-bit divide (L).

| File | What |
|---|---|
| `c2_int.h` | the shared declarations: `c2_index_t` (a frame's fields as indexes), the fixed-point formats, `c2_params_t` |
| `c2_bits.c` | `c2_pack`, `c2_unpack`; the silence frames `c2_silence_3200`, `c2_silence_1600` |
| `c2_tab.c` | LSP levels, `c2_wo_dequant`, `c2_wo_L`, `c2_L_of_wo`, `c2_e_dequant`, `c2_lsp_dequant` |
| `THIRD_PARTY` | where the table values come from (below) |

### 16.1 Bit packing

One table per mode lists the fields of section 4.2 or 4.3 in order, each as its offset in `c2_index_t` and its width.
Pack and unpack walk the same table, so they cannot disagree on the layout.

| Case | Behavior |
|---|---|
| Index wider than its field | its low bits are sent |
| Voicing flag | any non-zero value is 1 |
| Fields a mode does not carry | `c2_unpack` sets them to 0 (3200: voicing 3 and 4, the second ω0 and energy) |

Every 64-bit string is a valid frame in both modes: unpacking and packing again gives the same bytes.

### 16.2 Formats and tables

| Quantity | Stored as | Against the spec's float32 value |
|---|---|---|
| LSP levels (14.1, 14.2) | `int16_t` Hz; the seven equal 3200 tables are stored once, likewise the three others | exact: whole Hz |
| ω0 (9.1, table `wo_dequant`) | `int32_t`, `C2_WO_Q` (Q28) | exact: every float32 from 2^−5 up is a whole number of 2^−28, and ω0_min is above 2^−5 |
| L (10.2) | computed: ⌊843314856 / w⌋ for w in Q28, at most 79 | equal to trunc(π_c/ω0) in double on every Q28 value from ω0_min to ω0_max (73,790,053 values) |
| Energy (9.2, table `energy_dequant`) | `uint32_t`, `C2_E_Q` (Q18), the float32 value rounded | 1.5·10^−5 relative at most (index 0) |
| Decoded LSPs (10.2) | `int32_t`, `C2_LSP_Q` (Q28) | 4.2·10^−7 rad at most on the harness: the float32 rounding of the reference, plus 0.6 of a unit |

L needs no table: π_c · 2^28 = 843314856.643, so trunc(π_c/ω0) = ⌊843314856 / w⌋ for a whole w. The double quotient of
the reference cannot round up to the next whole number, because the true quotient is at least 0.357/w below it.
`c2_wo_L` is `c2_L_of_wo` of the level; it gives table `wo_dequant_L` (79 at index 0, 10 at index 127).

LSP dequantization:

| Step | How |
|---|---|
| Hz to rad | hz · 210828 + ⌊(hz · 46803 + 2^15) / 2^16⌋, the factor π_c/4000 · 2^28 = 210828.714 in two parts so both products fit 32 bits up to 9800 Hz (the largest 3200 sum) |
| 3200 | the levels summed in Hz (exact), then converted |
| 1600 ordering | the scan of 10.2 with 0.1 rad = 26843546 (f32(0.1) exactly); at most 15 fixes over all 2^36 index combinations (an exhaustive run; 470 combinations need 15, and on all of them `c2_lsp_dequant` is within 4.2·10^−7 rad of the float32 model), and the loop stops after 20 |
| 1600 spacing | a gap is short when below 10541435.708 (50 Hz) or 21082871.416 (100 Hz), so when below 10541436 or 21082872; a short gap becomes 10541436 or 21082871 |

The spacing comparison is the one place a whole unit decides: a gap of exactly 50 or 100 Hz in levels may or may not be
fixed, and either way the LSP moves by less than a unit of float32.

### 16.3 Silence frames

What the spec's encoder sends for digital silence once its start-up frames are past. The reference encoder's output on
the harness's `silence` input is this frame from its third frame on (3200) or its second (1600).

| Field | 3200 | 1600 | Why |
|---|---|---|---|
| Bytes | `01 00 09 43 9C E4 21 08` | `01 00 04 00 25 75 DD F2` | |
| Voicing | 0, 0 | 0, 0, 0, 0 | no harmonics |
| ω0 index | 3 | 3, 3 | the pitch estimator on zeros still sees the 1.0 its notch adds (6.2) and settles on 59.375 Hz |
| Energy index | 0 | 0, 0 | E = 0 (8.1), the lowest level |
| LSP indexes | 0, 14, 15, 11, 11, 11, 15, 15, 15, 15 | 0, 3, 6, 5, 6, 9, 9, 5, 7, 3 | the LSPs (π_c/10) · i of zero energy (8.1), quantized (9.3, 9.4) |

### 16.4 Provenance

The LSP levels are values from the Codec 2 reference implementation (LGPL 2.1), taken by decision of the RadioFoundry
maintainer, not under a separate grant from the Codec 2 authors (section 1). The ω0 and energy levels follow from the
formulas of 9.1 and 9.2; their float32 values come from `tables.json`. The tables were written once from
`tables.json`, not generated; no code was taken.
`core/codec2/THIRD_PARTY`, `REUSE.toml` and [docs/CLEANROOM.md](../CLEANROOM.md) record it.

### 16.5 Tests

`tests/host/c2TabTest.c`:

| Check | Scope |
|---|---|
| Sum and SHA-256 of 14.4 | the 20 LSP tables, `wo_dequant`, `wo_dequant_L`, `energy_dequant`, each converted back to float32 (int32 for L) |
| ω0 and energy formulas | f32(ω0_min + f32(i · s)) and f32(10^((−10 + 1.5625 i)/10)) give the stored values; ω0_min is 0x3D20D97C |
| `c2_L_of_wo` | every Q28 value from ω0_min to ω0_max against the double quotient |
| Layout and Gray code | every value of every field (widths 1, 2, 3, 4, 5, 7) alone at its bits of 4.2 / 4.3, and back |
| Round trips | 200,000 random index sets and 200,000 random frames per mode |
| `c2_lsp_dequant` | within 10^−6 rad of a float32 model of 10.2, on 1,000,000 random index sets and the corners per mode, and on two 1600 sets that need 15 ordering fixes (the cap must not cut them short) |
| Silence frames | their fields, and the LSP indexes the quantizers of 9.3 and 9.4 give for zero energy |

With the reference harness (`RFOUNDRY_C2_HARNESS=<dir>`, or the directory as the first argument; it stays outside the
repository): all 60 keys, 5,854 frames. Each frame unpacks to the reference decoder's indexes and packs back to its
bytes; each of the 4,686 encoder frames packs to the reference's bytes; the float32 model gives the reference's decoded
LSPs exactly and its 1600 ordering fix count; `c2_lsp_dequant` is within 1e-4 rad of them (4.2·10^−7 measured); the
silence frames are what the reference encoder sends for silence. Without the harness these checks are skipped with a
notice.

## 17. What this firmware does: the decoder

Sections 10 to 13 in fixed point, one implementation for every platform, in `core/codec2`. Integers only: no float, no
libm, no 64-bit product or divide in a per-sample or per-bin loop (the `core/dsp/fxp.h` kernels where a 32 x 32 product
is needed). Parameters decode exactly as the reference decodes them (15.1); the synthesis is as close to the exact
arithmetic as its formats allow, closer than float32 on ill-conditioned spectra.

| File | What |
|---|---|
| `c2_dec.c` | `c2_dec_init`, `c2_dec_frame`, `c2_dec_block`, `c2_dec_params`: the frame's fields to 10 ms parameters (10.2, 10.3), concealment |
| `c2_synth.c` | one 10 ms frame from its parameters: LSPs to spectra (10.4, 11), phases (12), the frame waveform, overlap-add and limiter (13); the exact helpers of 17.3 |
| `c2_dec_int.h` | the decoder's state laid over `c2_dec_state_t`, its scratch over `c2_scratch_t`, both with `_Static_assert` |

### 17.1 Blocks, frames and state

`c2_dec_frame` takes a codec frame's bits once; each `c2_dec_block` then synthesizes two 10 ms frames (160 samples), so
every block costs about the same. A 3200 frame is one block (frames 1, 2), a 1600 frame two (frames 1, 2, then 3, 4).
Interpolation (10.3) needs only the previous codec frame's last 10 ms frame and the current frame's fields: frames 2 and
4 carry their own ω0, energy and voicing, the LSPs of 1600 frames 1 to 3 are interpolated from the previous frame's and
the current frame's at t = 1/4, 1/2, 3/4. No four-frame buffer is kept.

| State (`c2_dec_t`, 1,200 of 1,216 B) | Bytes | Format |
|---|---:|---|
| `amp[80]`: log2 A_m of the frame being synthesized | 320 | `C2_LOG_Q` (Q24) |
| `phi[80]`: the direction of conj H_A at each harmonic's phase bin (12.1) | 320 | two int16 |
| `ola[79]`: overlap-add memory o (13.2) | 316 | `C2_OUT_Q` (Q4) int32: past 16 bits before the limiter |
| φ_ex (12.2) | 8 | a float32 value held exactly in Q48 (two words) |
| background estimate b (12.4), random generator (12.3) | 8 | dB in Q20; the generator's 32-bit state, seed 1 |
| previous 10 ms frame, current frame's LSPs and fields | 110 | `c2_params_t`, `C2_LSP_Q`, `c2_index_t` |
| the last block's two frames' parameters and draws (`c2_dec_params`) | 108 | `c2_params_t` |
| blocks left, block index, concealment count, flags | 8 | |

| Scratch (2,400 of 2,560 B, contents undefined between calls) | Bytes |
|---|---:|
| 512 words: log2 P(k) and the post-filtered log2 P(k), then the frame's spectrum Y and waveform (`dsp_irfft`) | 2,048 |
| cos ω_i (Q29), the coefficients of A(z/γ) (Q23), log2 \|H_W\|² at every fourth bin (Q16) | 352 |

### 17.2 Formats and steps

| Step | How | Against the exact value |
|---|---|---|
| ω0, L, E, LSPs (10.2, 10.3) | `c2_tab.c` (16.2); ω0 means rounded as float32 rounds them; E = ⌊√(E_a E_b)⌋ in Q18 | ω0 and L exact; E within a unit of Q18; LSPs within 0.6 of a Q28 unit |
| cos ω_i | the nearest of 512 grid angles (a Q30 table, 129 entries) and a second-order Taylor step | 6.5·10^−9 |
| A(z/γ) (11.2) | expanded from the LSPs in Q23 (`γ = 1/2`: the products are shifts) | |
| \|H_W\|² (11.2) | Clenshaw's recurrence in Q30 at every fourth bin, a 4-point cubic between | the cubic within 1.5·10^−5 of 0.2 log2 \|H_W\|² on the reference set |
| \|A\|² and arg A (11.1, 12.1) | the LSP product form, A(e^{jω}) = 2^5 e^{−j5.5ω} [cos(ω/2) Π_odd (cos ω − cos ω_i) + j sin(ω/2) Π_even (cos ω − cos ω_i)]: each difference exact in Q29, carried to 15 significant bits, products in 16 x 16-bit multiplies | each factor within 2^−16, of no bias; an FFT's absolute error would swamp \|A\| near a formant (down to 7·10^−5 on the tones) |
| P, post filter, sums (11.2, 11.3) | log2 values in Q24; 2^x and log2 from 64-entry tables (second order: 3·10^−7 and 1.1·10^−6; first order in the sums, whose error scales a whole sum alike); each band summed from its own largest bin | |
| A_m, e, b, T (11.3, 12.4) | log2 A_m; e and b in dB, Q20; A_m < T compared as log2 values | |
| phases (12.2, 12.4) | e^{jφ_m} = X_m H_m / \|H_m\|: the excitation's cosine and sine (`dsp_cos_i`) times the stored direction, \|H_m\| folded into the amplitude's log2: no arctangent | |
| frame waveform (13.1) | Y in Q_s, s chosen per frame from Σ A_m ≤ √(L Σ A_m²) so the transform stays under 2^30; `dsp_irfft`; to Q4 | |
| overlap-add, limiter (13.2, 13.3) | t[i] = i/80 in Q15; the limiter's gain (30000/x_max)² from `dsp_recip` in Q30 | |

Σ A_m is held to 2^24 (five times the loudest frame of the reference set) so the output, at most 2 Σ A_m, fits Q4 with
the overlap-add's sum; a frame past it comes out quieter than the reference's, before a limiter that takes it near
silence anyway.

### 17.3 What is reproduced exactly

| Quantity | How |
|---|---|
| f32(m ω0), f32(80 ω0), f32((ω0_a + ω0_b)/2) | whole numbers of Q28 rounded to 24 significant bits, to nearest even |
| φ_ex (12.2) | Q48 in 64 bits: each float32 sum rounded as float32 rounds it; the wrap's k from five thresholds (k − 0.5 − h)·2π_c, h half the float32 unit below k; 2π_c·k as the double it is. Equal to float32 arithmetic on 1,000,000 random steps |
| band edges a_m, b_m (11.3) | ⌊(2 j ω0 + (R − 1)/2) / R⌋ with r_d = R/2^30 (R = 13176795): one 32-bit divide |
| phase bin (12.1) | 4M/R as quotient and remainder, rounded up to Q + 1 when the remainder is within half a float32 unit of Q + 1/2 |
| synthesis bin (13.1) | M · 2^30 against (2j − 1) · 2π_c · 2^48, both whole: the double division of the reference can sit within 2^−50 of a half integer (0.012 of a Q28 unit at j = 81) |
| random draws (12.3) | the generator per decoder, seed 1; unvoiced draws and post-filter draws in increasing m |

All three bins equal a float32/double model of the spec for every harmonic of every ω0 a decoder can meet (the 128
levels and the 8,128 means: 164,485 harmonics).

### 17.4 Concealment

Not in the spec: what the receiver does when the channel cannot trust a frame.

| Case | Output |
|---|---|
| `c2_dec_frame(…, bad = true)` or no bits | the last good codec frame's final parameters held (ω0, voicing, LSPs); the energy halved every 10 ms frame (−3 dB) |
| after 8 concealed 10 ms frames (80 ms, −24 dB) | muted: the overlap-add memory played out, then silence |
| lost frames before any good one | silence; the first good frame then decodes as from `c2_dec_init` (10.1), nothing faded |
| the first good frame after | interpolates from the faded energy (10.3) |
| a frame before the last one's blocks are out | replaces them: the parameters move on as if they had played |
| `c2_dec_block` with no frame to play | false, silence, nothing advanced |

### 17.5 Against the reference

`compare.py --gate fixed` (`HARNESS.md`) on the candidate `c2DecCand` writes, all 60 keys: PASS.

| Measure | Result |
|---|---|
| params_close; rand_calls_exact | 100% and 100% on every key (16,000 10 ms frames) |
| LSD mean / p95 (gated keys) | ≤ 0.11 / 0.33 dB |
| LTAS, env (all keys but sweep_bits.3200) | ≤ 0.07, ≤ 0.02 dB |
| SNR (keys with reference rms ≥ 100, sweep_bits.3200 exempt) | 47.8 to 74.9 dB; sweep_bits.3200 6.9 dB, as the float32 model's |

### 17.6 Cost

| Item | Arm (Cortex-M4, -Os) | C-SKY (CK803S, -Os) |
|---|---:|---:|
| `c2_dec.o` + `c2_synth.o` text | 6,708 B (1,128 + 5,580) | 8,872 B (1,128 + 7,744) |
| of it tables (cos, log2, 2^x, the φ_ex wrap) | 1,132 B | 1,132 B |
| from `core/dsp` besides | `dsp_irfft`, `dsp_cos_i`, `dsp_recip`, `dsp_isqrt64` | the same |

No `__muldi3`, `__divdi3` or float symbol in the C-SKY objects, no libgcc helper in the Arm ones.

Time per block (two 10 ms frames), counted as Cortex-M4 instructions (the -Os code run under an instruction-counting
emulator, on 344 blocks of four reference streams) and scaled to the CK803S at 168 MHz by the full-rate decoder's ratio
(its CK803S cycles from an instruction-level model of that core, over its Cortex-M4 instructions counted the same way:
1.34 on the mean frame, 1.47 on the worst):

| | Cortex-M4 instructions | CK803S estimate |
|---|---:|---:|
| mean block | 421,000 | 565,000 cycles, 3.4 ms |
| largest block | 465,000 | 620,000 to 680,000 cycles, 3.7 to 4.1 ms |
| full-rate decoder (P25), for scale | 219,000 mean, 434,000 largest per 20 ms | 1.7 ms, 3.8 ms |

Against the audio block's 4 ms the worst block is at the limit. Where it goes, per block: the LSP products 23%,
`dsp_irfft` 25%, the spectral sums (`log2_sum`) 4%, the phase bins 3%, the rest of `c2_synth_frame` (the other
per-bin work, |H_W|, the band sums, the harmonics, overlap-add) 35%, the helpers it calls 10%. The cost hardly depends
on L: the spectra are per bin. Host (x86-64, -O2): 55 to 66 µs per block on the mean, 80 µs at most.

### 17.7 Tests

`tests/host/c2DecTest.c`, built plain and with `-DFXP_SPLIT16` (the C-SKY product path); both give the same PCM:

| Check | Scope |
|---|---|
| Random generator | the first 64 values: the first five, the sum and the SHA-256 of `rng_first64` (14.4) |
| f32 rounding, bins, φ_ex | against float32/double models: 2,000,000 values; every harmonic of every reachable ω0; 1,000,000 φ_ex steps |
| cos, log2, 2^x | within 1·10^−8, 1.5·10^−6, 1.5·10^−6 |
| API | no frame, the blocks of each mode, a frame given early, params before a block, init over a used and a garbage state |
| Concealment | silence before a good frame, and the stream after it as from init; the fade 3 dB per 10 ms; tail, then silence; resume to the same level |
| Extremes | every byte value in every byte, both modes: parameters in range, no sample over the limiter's 30,000 |
| Golden PCM | CRC-32 of synthetic streams (random and structured frames, lost frames, a burst past the fade) in both modes |

With the reference harness (`RFOUNDRY_C2_HARNESS=<dir>`): every 10 ms frame of the 60 keys within params_close, with
the reference's random draw count, and the golden PCM CRCs of `vowel_a_vibrato.3200` and `utterance.1600`. The Cortex-M4
build (its DSP-instruction kernels) gives the same two CRCs. `tests/host/c2DecCand.c` (not in CI) writes the candidate
files compare.py reads and prints the host time per block.


## 18. What this firmware does: the encoder

The encoder of sections 5 to 9 in `core/codec2`, fixed point, one implementation for every platform: no float, no
libm, no 64-bit product or divide per sample and no library multiply on a core without a wide one (`core/dsp/fxp.h`),
no RTOS call. It meets the harness's fixed gate (18.5); its bits are valid Codec 2 for any decoder.

| File | What |
|---|---|
| `c2_enc.c` | `c2_enc_init`, `c2_enc_block`: the block driver, the speech DFT (5.3), refinement, band sums and voicing (7), frame assembly (9.5) |
| `c2_nlp.c` | the pitch estimator (6) |
| `c2_lpc.c` | LPC analysis: autocorrelation, the Schur recursion and the orders it sets to 0 (18.3), energy, predictor, bandwidth expansion, LSPs (8) |
| `c2_quant.c` | the quantizers (9) over `c2_tab.c`'s levels |
| `c2_enc_tab.c` | the encoder's tables: analysis window, window spectrum, pitch estimator window and low-pass, expansion factors |
| `c2_enc_int.h` | the encoder's own declarations, and what its host tests read back (`c2_enc_trace`) |

### 18.1 Blocks, state and scratch

One `c2_enc_block` call takes 160 samples: two 10 ms analysis frames (5-7) and one LPC analysis on the second (8). A
3200 frame is complete every block. A 1600 frame takes two: the first block gives voicing, ω0 and energy of frames 1 and
2 and returns false with the bits untouched; the second gives frames 3 and 4 and the LSPs. The LSP search runs only
where a frame sends LSPs, so every block costs about the same (18.6).

| Part | Bytes | Holds |
|---|---:|---|
| State (`C2_ENC_STATE_BYTES` 1,216) | 1,124 | analysis buffer 320 x int16 (640); pitch estimator 392: notch memories, the low-pass's last 47 inputs, the 48 decimated values kept, the previous bin; the 1600 frame's first half (18); the last block's trace (68) |
| Scratch (`C2_SCRATCH_BYTES` 2,560) | 2,556 | per stage, one after the other: the pitch estimator's FFT buffer and low-pass input (2,048 + 508); the speech DFT, then its power table in place, and the bins below 1 kHz as 16-bit parts (2,048 + 312); the LPC analysis's windowed buffer (554) |

`_Static_assert`s in `c2_enc.c` hold both. The scratch carries nothing between calls. Stack: 792 B deepest on C-SKY
(840 B with Levinson in its place), 500 B on Arm: the LPC analysis under the block call, its recursion the deepest
branch and the LSP search 4 B under it.

### 18.2 Formats

| Quantity | Format | Why |
|---|---|---|
| Pitch estimator chain (6.1-6.4) | Q0 int32: squares up to 2^30, the notch output in [-2^30, 2^30 - 2], the spec's +1.0 exactly 1 | the notch output is the input less a weighted mean of earlier inputs, all in 0..2^30 |
| Low-pass taps | Q31; 64-bit sums of symmetric pairs | 24 products per output |
| Pitch DFT input | the frame's largest value under 2^24, window Q15 | 64 inputs sum under 2^30: no FFT stage overflows |
| F(k) | 64-bit power, then scaled so the largest is under 2^27 | 20 F of the 6.6 thresholds fits 32 bits |
| Speech DFT input | windowed buffer (window Q23) scaled so the largest value is under 2^22 | 277 inputs sum under 2^31 |
| Speech DFT output | scaled so the largest part is under 2^15; powers 32-bit; the 1e-4 floor of 7.3 in the same scale | every decision of 7 is a ratio |
| ω0 | Q28 (`C2_WO_Q`) | the dequantizer's format |
| Periods, bins per harmonic | Q16 samples; Q20 bins | 2 pi_c / q and 512 / q by 32-bit hardware divides |
| LPC input, R | windowed buffer under 2^14 (int16); R exact in 64 bits, then R[0] in [2^29, 2^30] | 8-product dot products fit 32 bits |
| Schur recursion (8.2) | forward and backward error correlations, 32-bit in R's scale; k Q30 | each bounded by R[0] while R is positive definite; one rounding a step |
| Stopping margin | 2^-23 R[0] (18.3) | the reference's float32 R is off by about 2^-22 R[0] |
| Line stop | \|k\| >= 0.996, Q30 1,069,446,857 (18.3) | one 32-bit compare per order: 1 - k^2 <= 0.008 |
| Predictor | Q22, from k, for the LSPs only | |a_i| <= C(10, i) <= 252 for |k_i| <= 1 |
| LSP polynomials | scaled to their largest (under 2^25), x in Q30 | only the sign is used |
| LSPs | Hz, Q16, from `dsp_atan2` (1e-4 rad) | |
| LPC energy | log2 in Q11 (`dsp_log2_q11`) | the quantizer is uniform in dB |

### 18.3 Deviations from the reference

| Where | Reference | Here | Why, and what it does |
|---|---|---|---|
| Pitch estimator history (6.3, 6.4) | 80 low-pass outputs a frame, q[0..319] | the 16 the decimation reads, 48 kept | same values; 4x less work |
| Notch (6.2) | f32 and double, 0.95 | 0.95 in Q31, rounded toward zero; output held to [-2^30, 2^30 - 2] | toward zero so a small output decays as the spec's does; the bound moves only a full-scale -32768 after silence |
| DFTs (5.3, 6.4) | float32 | `dsp_rfft`, Q15 twiddles, block scaled | fixed point: the bins differ by the transform's rounding |
| Levinson-Durbin (8.2) | float32 Levinson on a float32 R: \|k_i\| > 1 set to 0 | the Schur recursion on the exact R; k is 0 at each order where R less 2^-23 R[0] would give \|k\| >= 1, and the recursion goes on past it as 8.2's does. On a frame where that sets an order to 0, the first order from 3 on with \|k\| >= 0.996, and every order after it, take k = 0 too, unless order 1 or 2 has such a k | the reference's rejected orders, without its round-off; on two steady lines or more (two_tone, DTMF), the envelope the decoder renders at the input's level (below) |
| Energy (8.3) | sum of a_i R[i] in float32 | the recursion's error R[0] prod(1 - k_i^2), with those orders' k at 0 | equal in exact arithmetic, no difference of large terms; on the 90 Hz vowel's frames checked it is within 0.1 % of a double-precision model, where the reference's float32 sum is 1.6 to 3.5 % off it |
| Refinement and band sums (7.1, 7.2) | float32 powers | 32-bit powers of 15-bit parts | 93 dB of range: on a pure tone the harmonic sums of a candidate between the tone's harmonics fall under it, and ω0 moves one index (tone_200: exact 0 %, within one 99 %) |
| Bins per harmonic (7.1) | f32 repeated addition | Q20, exact repeated addition | a bin can differ where m c is within float32 error of a half |
| L (7.2) | floor of the f32 quotient; the 5% rule on the f32 product | floor, plus one when the quotient is within half a float32 step under the next whole number; the exact product against the midpoint below the first float32 at or above 0.95 π_c | the same L as the spec's on every Q28 ω0 (tested on all 73,790,053); the exact product against 0.95 π_c itself gave another L on 17 |
| Window spectrum (7.3) | W(o + k) over the whole band | W(256 + d) for \|d\| <= 16, 0 beyond | the rest is under 2.7·10^−4 of W(256) (5 units of its Q15) |
| ω0 index (9.1) | floor(128 n + 0.5) | the nearest `c2_wo_dequant` level, ties up | the same decision up to the levels' float32 rounding |
| LSP search (8.5) | float32 polynomial | Clenshaw's recurrence on the scaled polynomial, 32-bit | the sign test only |
| Amplitudes A_m (7.2) | formed | only their sums below 1, 2, 4 kHz | nothing else reads them |

Which orders take k = 0. Found with the codec2-extra vectors (deep spectral nulls, near-Nyquist content,
fricatives; kept outside the repository like the harness): before this rule the encoder sent energy index 0 where the
reference sends 5 or more on 199 of 200 frames of a 3400 Hz tone, 499 of 500 of full-scale 4 kHz, 240 of 500 of a
full-scale square wave, 73 and 39 of 200 of band noise with brickwall edges at 3600-3990 and 3300-3950 Hz, and on
such noise above 2 kHz it decoded 2 to 3 dB (3200) or 1 to 7 dB (1600) under the reference's level.

| What | Finding |
|---|---|
| Not fixed-point range or saturation | the integer R is the exact autocorrelation of the windowed buffer; a double-precision model of section 8 sends the same index 0 (alt_fullscale: E = 0.0013, 6e-10 of R[0]) |
| The reference's E is its float32 round-off | a float32 model of 8.1-8.3 matches the reference's energy index on every frame of the nine keys checked (the 3400 Hz tone, both full-scale squares, six band-noise keys), with the Levinson sum taking R[i] after the sum of a_j R[i - j] (R[i] first matches none of vowel_a_110's dumped predictors); the same model in double precision on 0 to 27 % of the frames of the -12 dBFS and full-scale ones |
| How | the float32 sum leaves each R[j] off by 1e-7 to 3e-7 of R[0], which is not the autocorrelation of any signal. Where the spectrum's floor is under that, the reference's R stops being positive definite at some order: Levinson meets \|k\| > 1 and sets it to 0; it goes on from the same predictor, most often meets \|k\| > 1 again at the later orders (not always: on the chirps a recursion that goes on agrees with the reference more often than one that stops), and E is the error of an order before, 1e-6 to 1e-4 of R[0]. On alt_fullscale only order 1 takes its k (E = 1.7e-4 R[0], index 23); exact, order 2 alone takes the error to 6e-8 R[0] |
| The fix | the orders that take k = 0 come from R with 2^-23 R[0] taken off R[0], which is positive definite at an order exactly while that order's smallest eigenvalue is above the margin; k and E come from the exact R with the same orders at 0, the recursion going on past them as 8.2's does. The Schur recursion keeps each order's k exact to a few 2^-30 R[0]: Levinson's sums carried the Q22 predictor's rounding, near 2^-22 R[0], which would decide the rejected orders itself. Stopping at the first rejected order instead gives the same bits on every codec2-extra key, but the chirps' energy within one falls from 49.0 and 38.7 % to 43.0 and 33.3 % (the test checks it) |
| What it leaves alone | no speech-like harness frame and no smooth-spectrum control of codec2-extra changes with the margin (the same bits with 2^-29); on the 30 s speech-like inputs only frames of the brickwall fricatives do |
| What it cannot do | match the reference frame by frame where its round-off decides: which order fails, and the value of the accepted k, depend on the float32 rounding of each R sum (band noise: even the reference's own R with Levinson in double gives 86 to 92 % within one). Emulating that sum would be 2,992 soft-float multiply-adds per analysis on a CK803S |

Two steady lines or more. Measured against the input (this decoder on this encoder's bits; level: output less input;
in-band error: codec2-extra's measure, RMS dB over 94-3750 Hz on the bins within 30 dB of the input's long-term
peak), the margin alone put two_tone 5.5 dB (3200) and 3.8 dB (1600) under the encoder before it. On a pure sinusoid
the error collapses at the order that resolves the line, |k| near 1: a single tone at order 1 or 2, two tones at order
4 (|k| 0.9972 to 0.9985 on two_tone and DTMF pairs), after three orders that resolve neither. A predictor holding two
lines that sharp comes out of 8.4's 15 Hz expansion and the LSP quantizer far under the input: the margin's order 6
on two_tone sends energy index 8 and decodes 14 dB low. Where the margin marks the frame, the reference's own stop is
its round-off (it runs two_tone to order 10, index 5), so the line stop keeps the three orders before the collapse,
with their error (index 28):

| Input (3200 / 1600; level dB, in-band error dB) | Before the margin | Margin | Margin and line stop |
|---|---:|---:|---:|
| two_tone (500 + 1500 Hz, -18 dBFS each) | -8.7, 14.5 / -15.1, 33.0 | -14.2, 17.4 / -18.9, 26.5 | -1.8, 13.6 / -2.4, 13.2 |
| Five DTMF pairs (697 to 1633 Hz), -12 dBFS peak | -13.6 to -9.0, 8.7 to 18.1 / -25.8 to -18.5, 27.5 to 32.3 | -10.9 to -8.8, 10.4 to 20.1 / -24.5 to -12.1, 22.0 to 30.3 | -1.3 to +1.0, 10.1 to 18.5 / -1.7 to +0.7, 10.4 to 20.7 |
| The same, -24 dBFS peak | -4.5 to +1.5, 6.7 to 15.3 / -22.0 to -11.6, 22.9 to 32.0 | -10.1 to -6.5, 7.3 to 17.1 / -22.7 to -11.3, 19.4 to 28.3 | -0.6 to +0.8, 9.6 to 17.6 / -1.7 to +0.5, 9.1 to 19.3 |

At -24 dBFS in 3200 the encoder before the margin had 1.7 to 4.7 dB less in-band error on the pairs: its sharp
predictor sat at the quantizer's lowest energy level, which holds the decoded level up. A single line keeps the
margin's orders: the envelope before its collapse is order 0 or 1, flat, and spreads the tone over every harmonic;
the reference's energy index on tones near 4 kHz, DC and alternating full scale is the margin's. Nothing else is
touched: band noise has no marked frame with |k| over 0.995 from order 3 on (codec2-extra), the 30 s speech-like
inputs none over 0.955, the harness's speech-like frames are not marked; on the harness only two_tone's bits change.

Steady single tones are where the margin still costs level against the encoder before it: tone_200
-15.0 dB against -11.0 (3200), tone_3900_pk_m12 -18.9 against -14.6. Those figures were the Q22 predictor's
rounding (on tone_200 Levinson stopped at order 7 with k_4 = 0.08 for an exact 0.45), and the reference's own decode
of tone_200 is -12.6 dB with 3.6 dB more in-band error. The margin's stop on a tone is order 2 or 3, the reference's
2 to 5: tone_200 decodes within 0.5 dB of the old figures in both modes only at order 5, deeper in round-off than the
orders the reference rejects on the 3400 Hz tone and on alternating full scale (both checked, 18.7). Averaged over 28
single tones (150 Hz to 3.2 kHz, -12 and -24 dBFS) the margin is 0.6 dB above the old level at -12 dBFS (3200), 5.7
dB under it at -24 dBFS (3200), and 7 dB and 2 dB above it in 1600. Two more keys stay under the encoder before the
margin in 3200: tone_3800_pk_m12 with 2.2 dB less level (-18.0 dB; the reference's own decode -33.8), and brickwall
noise at 500-1500 Hz, -12 dBFS, with 1.7 dB more in-band error (4.3 dB; the reference's 3.5).

### 18.4 Arithmetic per platform

The same bits from every build: the host plain and with `FXP_SPLIT16` (the products a C-SKY core makes from 16-bit
halves), Cortex-M3 with halves and Cortex-M4 with its DSP instructions (run on an emulator) give the same bits on the
harness's inputs. A C-SKY object calls no `__muldi3`, `__divdi3` or float routine; its only library calls are variable
64-bit shifts (`__ashldi3`, `__ashrdi3`, `__lshrdi3`), a few dozen per block in per-frame code (scales, the ratio
tests, the Schur recursion's divides).

### 18.5 Against the reference

`compare.py --gate fixed` (encoder metrics, no `--c2dump`), all 56 PCM keys: **PASS**.

| Keys | Frames bit-exact | Voicing | ω0 index (within one) | Energy index (within one) | LSP indexes (within one) |
|---|---:|---:|---:|---:|---:|
| All 56 | 75.4 % of 4,686 (72.1 % before 18.3's margin) | 100 % | 95.2 % (98.8 %) | 83.4 % (84.9 %) | 85.5 % (88.6 %; 86.4 % (89.7 %) before the line stop) |
| Speech-like, worst key | | 100 % | 99.0 % | 100 % | 99.2 % within one |
| Gate (speech-like, per key) | | >= 97 % | (>= 95 %) | (>= 97 %) | (>= 95 %) |

Every pulse train, vowel, noise and clipping key is at 99 % or more within one on every field; the lowest exact rates
are vowel_u_90.3200's LSPs (91.7 %) and utterance.3200's (97.3 %). The tonal keys (tones, DC, sweeps; not gated) are
round-off decided as 15.2 says, and the margin moved them both ways: dc_offset is bit-exact on every frame (1 % before),
the sweeps' energy index agrees on 33 to 40 % (8 to 38 %), tone_1000 and tone_3000 stay exact, and tone_200's energy
agrees on 1 % (100 %: the reference's float32 recursion stops at order 4 with 7 times the exact error, which Levinson's
own rounding happened to give). The line stop changes only two_tone: its LSP indexes agree on 1 % (3200) and 30 %
(1600), 31 and 60 % before it, as it sends the envelope of order 3 where the reference runs to order 10. The pitch
estimator's bin agrees with the reference's on 98.8 % of all 10 ms frames.

codec2-extra (64 keys, 14,700 codec frames), before and after the margin of 18.3 (the line stop changes none of them);
E: energy index exact (within one), the same in both modes (one analysis feeds both); LSP indexes within one, 3200 /
1600; "0 for 5+": energy fields where this encoder sends 0 and the reference 5 or more:

| Keys | E before | E after | LSP before | LSP after | 0 for 5+ before / after |
|---|---:|---:|---:|---:|---:|
| All 64 | 54.5 % (65.4 %) | 58.6 % (75.3 %) | 67.1 / 89.9 % | 75.4 / 94.6 % | 1,127 / 26 |
| alt_fullscale (full-scale 4 kHz) | 0.2 % (0.2 %) | 100 % (100 %) | 60.1 / 50.0 % | 100 / 100 % | 499 / 0 |
| tone_3400_pk_m12 | 0.5 % (0.5 %) | 0.5 % (100 %) | 30.4 / 90.0 % | 100 / 100 % | 199 / 0 |
| steps_fullscale | 4.0 % (4.0 %) | 52.0 % (52.0 %) | 47.2 / 56.8 % | 56.8 / 76.0 % | 240 / 0 |
| band_brickwall_3300_3950_m12 | 3.0 % (8.0 %) | 12.0 % (31.5 %) | 23.9 / 90.4 % | 51.1 / 92.2 % | 39 / 0 |
| band_brickwall_3600_3990_m12 | 10.0 % (17.0 %) | 14.0 % (35.0 %) | 30.3 / 83.4 % | 56.9 / 88.3 % | 73 / 0 |
| band_brickwall_3300_3950_m30 | 91.0 % (91.5 %) | 81.0 % (90.5 %) | 25.5 / 88.7 % | 31.4 / 90.5 % | 17 / 13 |
| band_brickwall_3600_3990_m30 | 97.5 % (97.5 %) | 97.5 % (97.5 %) | 19.4 / 77.0 % | 41.6 / 83.8 % | 5 / 5 |
| Brickwall bands from 2 kHz, -12 dBFS (7 keys) | 6.9 % (16.1 %) | 14.1 % (40.8 %) | 36.3 / 93.1 % | 47.9 / 94.8 % | 133 / 0 |
| Brickwall bands from 2 kHz, -30 dBFS (7 keys) | 53.6 % (70.1 %) | 47.4 % (69.4 %) | 41.1 / 91.5 % | 45.1 / 93.0 % | 48 / 26 |
| Tones 3600-3950 Hz (4 keys) | 75.1 % (100 %) | 25.4 % (50.2 %) | 47.7 / 72.5 % | 75.1 / 80.0 % | 0 / 0 |
| Smooth-spectrum controls (4 keys) | 100 % (100 %) | 100 % (100 %) | 100 / 100 % | 100 / 100 % (the same bits) | 0 / 0 |
| 30 s speech-like sequences (2 keys) | 94.6 % (97.4 %) | 94.7 % (97.9 %) | 97.1 / 99.7 % | 97.3 / 99.7 % | 7 / 0 |

Decoded level against the reference's (this decoder on this encoder's bits, the reference decoder on the reference's;
codec2-extra's figures): on brickwall noise from 2 kHz at -12 dBFS, -3.1 to -2.0 dB before and +0.1 to +2.4 dB after
(3200), -7.2 to -1.1 dB and -1.5 to +0.7 dB (1600); the in-band spectral error against the input drops by the same
order (3300-3950 Hz in 1600: 9.5 dB over the reference's, then 2.0). The controls stay within 0.1 dB.

What stays apart, and why:

| Where | Why |
|---|---|
| Brickwall band noise, energy within one 30 to 50 % | which order fails and the accepted k are the reference's float32 round-off, frame by frame (18.3); the averages agree (mean energy index difference +1.4 at -12 dBFS, -0.1 at -30 dBFS) |
| steps_fullscale, 52 % | a held -32768 gets index 4 from the reference and a held +32767 index 23: a power of two windows into float32 products without rounding, so its R sums round differently. This encoder sends 23 on both |
| Tones at 3600-3950 Hz | the reference's recursion runs to order 5 on a 3800 Hz tone (E 2.4e-7 of R[0], index 0) and stops at order 2 on 3900 Hz with 0.07 of the exact error; this one rejects the orders the margin says. On 3800 Hz its decode is the closer to the input (32 dB less in-band error than the reference's) |
| -30 dBFS bands, 0 for 5+ on 26 of 1,400 fields | at that level the input's own int16 rounding (8e-8 of R[0]) is the spectrum's floor, about the margin, and the reference's round-off stops some frames an order or two earlier |

### 18.6 Cost

| Measure | Value |
|---|---|
| Flash, `-Os` | Arm (Cortex-M4) 5,558 B: c2_enc 2,106, c2_lpc 1,730, c2_nlp 844, c2_enc_tab 516, c2_quant 362; C-SKY 8,090 B (2,786, 2,844, 1,500, 516, 444). The shared tables (`c2_tab.c`, `c2_bits.c`) and `core/dsp` come on top. The Schur recursion, its margin and the line stop cost 104 B on Arm and 6 B on C-SKY against Levinson (the line stop 48 and 40 B) |
| Instructions per block, emulated Cortex-M3 with halves (a C-SKY stand-in) | 456 k mean on the utterance, 503 k worst on any harness or codec2-extra input (steps_fullscale, 3200); 450 k and 496 k with Levinson. Per key the mean and the worst block grow 1.9 % at most, the worst block of all 1.5 %; the line stop's share is 0.3 % at most (two_tone) |
| Same, Cortex-M4 with DSP | 368 k mean, 409 k worst (364 k, 403 k with Levinson) |
| Of a block (M3) | the four 512-point FFTs 254 k; pitch estimator besides its FFT 50 k; LPC with LSPs 64 k (58 k with Levinson: the recursion runs twice, with the margin and without); refinement 24 k; DFT powers 17 k; voicing 13 k |
| CK803S estimate | 0.91 M cycles worst, 5.4 ms of the 6 ms budget at 168 MHz: scaled by the full-rate decoder's worst frame, which an instruction-level CK803S model put at 3.8 ms, against its worst count on the same emulator (352 k, 1.8 cycles each). Scaled by that decoder's mean instead, 7.4 ms: the measurement on the radio decides |
| Host (x86-64, `-O2`) | 26 us per block |

The four FFTs are half the work; `core/dsp` is where a faster one would pay. `-O2` for the encoder's files saves 3 %
for 1.1 KB more flash, so they build `-Os`.

### 18.7 Tests

`tests/host/c2EncTest.c`, built plain and with `-DFXP_SPLIT16` (`make -C tests/host run-codec2-enc`):

| Check | Scope |
|---|---|
| Tables | windows, window spectrum, expansion factors against the spec's formulas; low-pass taps against 14.3 |
| Harmonic count | `c2e_harmonics` on every Q28 ω0 from ω0_min to ω0_max against a float32 model of 7.2; every frame of the synthetic utterance (and of each harness key) has that L for its ω0, ω0 in range, the pitch bin in 16..128 |
| Golden CRCs | the harness's pulses_200 (made in the test, sample for sample) and a synthetic utterance, both modes; the same constants in both builds |
| Init, halves, scratch | init after any input in either mode starts over; 1600 completes a frame every second block, the first leaving the bits alone; scratch filled with garbage before every call |
| Silence | the frames of 16.3 from the third frame (3200) or the second (1600) |
| Hostile input | full scale, alternating extremes, -32768 held, random, +-1: every frame packs back |
| Near 4 kHz (18.3) | alternating full scale (codec2-extra's alt_fullscale, sample for sample): energy index 23 on every field from the second, the reference's; a 3400 Hz tone (its tone_3400_pk_m12): within one of the reference's 7; 64 sines over 3300-3950 Hz at -12 dBFS: no index 0, and decoded (`c2_dec.c`) within -7 to +3 dB of the input in 3200 (-4.9 dB; -9.2 dB and index 0 on a quarter of the fields before the margin) |
| Two lines (18.3) | the harness's two_tone (made in the test, checked against its file when the harness is given) and DTMF 1 and # (-18 dBFS a tone), both modes: decoded within 3 dB under the encoder before the margin, and -5 to +3 dB against the input (-2.4 to +1.0 dB; before: -31.8 to -8.4 dB; the margin alone put two_tone at -14.5 and -22.9) |

With the harness (`RFOUNDRY_C2_HARNESS=<dir>`): each stage against the reference's `enc.jsonl`, per speech-like key at
the fixed gate's rates (voicing exact >= 97 %, ω0 index within one >= 95 %, energy within one >= 97 %, LSPs within one
>= 95 %), and the pitch estimator's bin within one >= 95 %; golden CRCs on vowel_a_110 and utterance (utterance.3200's
moved with the Schur recursion: two frames' upper LSPs by one index, the test says which).

With codec2-extra (`RFOUNDRY_C2_EXTRA=<dir>`): twelve of its keys in both modes, each at the agreement 18.5 gives it
less a few points (energy within one, LSPs within one per mode), and no energy index 0 where the reference sends 5 or
more on the full-scale, tone, -12 dBFS band, control and 30 s keys; the alternating and tone inputs made in the test
are checked against its files. `tests/host/c2EncCand.c` (not in CI) writes the candidate files compare.py reads.
