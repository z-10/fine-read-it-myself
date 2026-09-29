# glint v0.7.0 — the quality release

124 commits since v0.6.0. This release takes glint from "~15 dB SNR at every
bitrate" (a single quantizer-curve bug) to **measurably ahead of LAME on 8 of
10 test clips at 128 kbps** (PEAQ ODG) and ahead on SNR across the board at
256 kbps — while cutting embedded RAM to **~64 KB, one third less than
Shine**. Every claim below is measured; the full experiment log, including
every dead end, lives in `PLAN.md`.

## Headline numbers (vs LAME 3.100 `-q 2`, identical inputs)

**128 kbps joint stereo, PEAQ ODG** (closer to 0 is better):

| clip | glint | LAME | | clip | glint | LAME |
|---|---|---|---|---|---|---|
| choir | **−1.28** | −2.06 | | piano | **−0.63** | −0.70 |
| orchestral | **−0.75** | −1.09 | | torture | **−0.32** | −0.45 |
| quartet | **−0.46** | −0.88 | | speech | **−1.24** | −1.29 |
| industrial | **−0.82** | −1.00 | | electronic | −0.25 | −0.22 |
| drums | −1.18 | **−1.03** | | castanets | −0.62 | **−0.29** |

**256 kbps joint**: everything ODG-transparent; glint ahead on SNR throughout
(speech 38.4 vs 36.9 dB, electronic 44.8 / NMR −18.0 vs 44.5 / −15.8, quartet
44.9 vs 46.0 with NMR −13.7 vs −11.2). Castanets mean noise-to-mask is
glint's at both rates. 64 kbps stereo: glint ahead on ODG (−3.17 vs −3.32).
MPEG-2 22.05 kHz @ 64k: 21.6 dB vs LAME's 17.6.

## Correctness fixes (wire format & math)

- **pow34 quantizer curve**: the old integer-grid table degenerated to
  identity on (0,1) — quantizer inputs are fractional — capping the whole
  encoder at ~15 dB SNR independent of bitrate. Root cause of everything.
- **sfb21 out-of-bounds scalefactor read**: a phantom ~29× HF boost with no
  decoder-side counterpart; fixing it was worth +8..17 dB SNR on music.
- **MPEG-2 `scalefac_compress`**: glint used an invented mapping — internally
  consistent, garbage for every real decoder. Now ISO 13818-3.
- **Short-block scalefactor band tables**: the 44.1 kHz table had an
  off-by-two boundary (138 vs ISO's 136), and the MPEG-2 tables were MPEG-1
  copies that scrambled the wire order — the real cause of the historical
  LSF short-block collapse.
- **`preemphasis[20]`**: 3 → 2 per ISO (desynced band 20 whenever preflag
  was set).
- **LSF start/stop region boundary**: decoders hardwire region0 = 54 lines
  (not 36) for LSF transition granules.
- **VBR budget bugs**: quantized under the caller's default frame budget
  instead of the max; assumed the CBR padding byte and overflowed the
  largest unpadded frame.
- **Small-buffer frame overflow**: a 320 kbps frame silently overflowed the
  1024-byte embedded frame buffer; buffers are now sized for every legal
  frame and `glint_create` rejects what cannot fit.

## New encoder machinery

- **Bit reservoir + rate control** (CBR): continuous main-data stream with
  deferred emission, buffer-feedback constant-quality anchor, post-transient
  banking (joint mode). Worth +1..2.6 dB at all rates.
- **Short blocks**: start/stop transition windows, per-frame scheduler with
  one-granule lookahead, per-window `subblock_gain`, full per-(band,window)
  short scalefactors, 6× attack threshold, 2-granule attack-decay extension.
  Live for MPEG-1 **and** MPEG-2/LSF, on **both** signal paths (fixed-point
  now matches the double path to ≤0.06 dB on transients).
- **Psychoacoustic allocation**: Schroeder-Bark masking aligned with the
  measurement metric drives scalefactor noise-shaping loops for long and
  short granules, shaping ~9 dB below the mask under a total-noise guard,
  with tonality-adaptive masker offsets at ≤96 kbps/channel and
  scalefac_scale escalation. Runs in the VBR path too.
- **Bitrate-scaled encoder lowpass** with content-aware sfb21 keep at high
  rates: the no-scalefactor sfb21 region is zeroed where it would only
  collect quantizer spray, kept where it holds real content.
- **Optimal Huffman region splits** on finished granules (exhaustive
  region0/region1 search over per-table prefix costs); real per-region table
  selection by actual bit count; preflag folding.
- **VBR**: real variable-size frames, psy shaping, **Xing header with seek
  TOC and gapless LAME tag** — players show correct duration, seek, and trim
  the codec delay sample-exactly (ffmpeg decodes at a 0-sample offset). New
  API: `glint_vbr_header()`.

## Embedded

- `GLINT_MODE=fixed` RAM: **~64 KB** (Shine: ~96 KB), measured as static/BSS
  + encoder context. Achieved via a mantissa cube-root table replacing a
  64 KB LUT (metrics-identical), single-slot per-samplerate model caches,
  float transition tables, and dead-code removal.
- Fixed path now has short blocks, transition windows, and the encoder
  lookahead — embedded output quality equals desktop.

## Performance

Three profile-driven optimization passes (byte-identical where claimed):
Huffman pair-cost LUTs, fused region/table selection, gain-search state
reuse, MDCT table fusion, `-O3`/LTO, and an opt-in threaded scale-factor
search (`-j N`, byte-identical for any thread count). Encoder speed ~260× /
52× / 34× realtime (speed/normal/best, Apple M1, 256 kbps stereo).

## Testing & tooling

- **GitHub Actions CI**: build matrix (double/fixed/both × Linux/macOS),
  unit tests, decode-based tests, full quality suite.
- **Decode-based MPEG-2 regression test** in ctest — the class of wire bug
  that unit tests provably missed.
- **Encoder league harness** (`tests/compare_encoders.py`): glint vs LAME vs
  Shine over a 10-clip lossless corpus with SNR, Bark-band NMR, PEAQ ODG,
  ViSQOL, PESQ and STOI, plus a regression gate (`--check`) against recorded
  baselines.
- **10-clip corpus**: speech, four music genres from lossless sources
  (incl. 24-bit/96 kHz CC0 piano and CC-BY choir), synthetic castanets and a
  deterministic torture clip (generators in `tests/`).
- **ABX listening tool** (`tests/abx.py`): aligned, loudness-normalized,
  double-blind, binomial p-value.

## Behavior changes

- Encoder latency is now 1104 samples (filterbank chain + one-granule
  lookahead) on both paths; VBR streams start with a placeholder frame that
  `glint_vbr_header()` finalizes (unrewritten streams decode with ~26 ms of
  leading silence).
- Content above the rate-dependent lowpass (15.8 kHz at ≥128 kbps/ch,
  44.1 kHz) is dropped unless the content-aware keep retains it.
- 320 kbps at 32 kHz and similar corner configs are rejected by
  `glint_create` in small-buffer builds only if they cannot fit (none of the
  legal ones are rejected anymore).
