# glint v0.8.0 — the AAC release

23 commits since v0.7.0, all in three days. glint is no longer an MP3
encoder: it is a **dual-codec (MP3 + AAC-LC), MIT-licensed, clean-room
encoder** whose AAC side places **behind only Apple and Fraunhofer FDK**
in quality at 128 kbps — while needing **47.4 KB of RAM** and encoding
with **no per-coefficient floating point** on FPU-less microcontrollers.
Every claim below is measured; the full experiment log including every
dead end lives in `PLAN.md` (§A0–A6).

To our knowledge this makes glint the only competitive-quality AAC-LC
encoder under a plain OSI license: ffmpeg's native encoder is LGPL (and
behind on quality), FDK's license is not OSI-approved and disclaims
patent grants, and vo-aacenc (Apache-2.0) is unmaintained and places
last on every clip in our league.

## The AAC-LC encoder

Written from the ISO 13818-7 / 14496-3 specs in three days of measured,
gated passes:

- **Wire format**: all four window sequences (short blocks with
  attack-split grouping), per-band M/S stereo, selective TNS,
  optimal-sectioning Huffman (per-band codebook DP with exact bit
  accounting — the count==emission identity is unit-tested), CBR with a
  bit-debt controller, constant-quality VBR (V0–V9), ADTS output, all
  12 standard sample rates, mono/stereo.
- **Normative tables** extracted from two independent implementations
  (vo-aacenc and ffmpeg), cross-checked bit-for-bit and verified
  prefix-free by the generator (`tools/gen_aac_tables.py`).
- **Psychoacoustics**: metric-aligned Bark masks with tonality-aware
  offsets at low rates, and a **distortion-controlled allocator** —
  per-band scalefactors in closed form from noise targets
  `mask^0.6 · k`, with the loudness knob bisected to the bit budget.
  Short frames get per-group masks and their own allocation tilt.
- **Validated by two independent decoders**: every configuration
  decodes with zero errors in both ffmpeg and Apple CoreAudio, which
  produce metrically identical output. A decode-based ctest gate
  covers 8 configurations including a transient torture case and VBR.

### Quality (measured league, 2026-07-06)

**128 kbps stereo, mean noise-to-mask ratio in dB** (lower is better;
rank ①–⑥ among the six encoders; `-q normal`):

| clip | Apple | FDK | **glint** | LAME-MP3 | ffmpeg | vo-aacenc |
|---|---|---|---|---|---|---|
| speech | **−6.9** | −5.4 | −3.6 ⑶ | −2.2 | −0.7 | +1.4 |
| electronic | −9.9 | **−11.4** | −4.3 ⑶ | −3.1 | −1.9 | +2.3 |
| quartet | −2.9 | −2.6 | **−5.8 ①** | −2.0 | +0.2 | +1.2 |
| industrial | −1.1 | −0.8 | **−1.4 ①** | −0.0 | +1.6 | +1.3 |
| piano | −9.4 | −8.5 | **−10.2 ①** | −7.5 | −4.1 | −1.8 |
| castanets | −7.4 | **−9.0** | −8.5 ⑵ | +2.6 | +7.1 | +18.6 |

On castanets glint's **PEAQ ODG is 0.00 — the best of all six encoders**
(Apple and FDK: −0.08): the short blocks, TNS and per-group shaping land
the transients transparently. At **256 kbps** everything from
Apple/FDK/glint is ODG-transparent; glint has the best NMR of all
encoders on quartet (−18.7 vs FDK's −13.3) and the highest SNR of the
AAC field on most clips. The full 6-clip × 2-rate log with
SNR/NMR/ODG/PESQ/STOI columns ships in `tests/aac_league_v0.8.txt`.
vo-aacenc — the obvious "just port it" candidate when this work started
— finishes last on essentially every cell.

Constant-quality **VBR**: V0 = 311 kbps / 0.0 % audible band-frames on
the speech clip (better than CBR-256), V4 ≈ 130–160 kbps
content-adaptive, V9 ≈ 42 kbps. No Xing-style header machinery needed —
ADTS frames are self-describing.

### Speed

After a profile-driven, byte-identity-gated perf pass (−52…54 % encode
time): **~272× realtime at `-q speed` and ~168× at `-q best`** on an
Apple M1 (integer build: 241× / 140×) — faster at `-q speed` than every
encoder in the league, including Apple's.

## Embedded: 47.4 KB and no FPU

- `GLINT_MODE=fixed` builds the AAC encoder in **47.4 KB** (24.3 KB
  context + 23.1 KB tables) — under vo-aacenc's measured 48.0 KB — with
  storage-only type changes that are metrics-identical to the desktop
  build, then adds `GLINT_AAC_INT`: **integer MDCT (131 dB transform
  SNR), a Q16 log-domain integer quantizer (0.04 coefficient
  mismatches per frame vs the double formula), and int64 energies** so
  that at `-q speed` no per-coefficient float instruction remains.
- **MP3 got the same treatment** (`GLINT_MP3_INT`): the old "fixed"
  path was Q31 only through the filterbank; now the `-q speed` CBR
  chain is integer end-to-end (new integer 36-pt MDCT + the shared
  log-domain quantizer), metrics-identical to the double rate loop
  within 0.02 dB. MP3 quality is otherwise bit-for-bit unchanged from
  v0.7.0 (regression gate deltas: +0.00 across the board).
- **Provable**: `tools/check_nofpu.sh` disassembles a Cortex-M0+ build
  and fails if any per-coefficient function contains a soft-float
  call. It passes.
- **Runnable without hardware**: `embedded/qemu/` runs the benchmark
  on an emulated Cortex-M3; the semihosted output streams decode
  cleanly in ffmpeg and CoreAudio, and the MP3 stream is **bit-exact
  with a host run** (the integer path is deterministic across
  architectures). Ready-to-flash benchmark projects for the Raspberry
  Pi Pico (`embedded/pico/`) and ESP32 (`esp-idf/example/`) are
  included — QEMU is not cycle-accurate, so real-silicon throughput
  numbers are the one thing still open.

## API, bindings, tooling

- New C API: `glint_aac_create/encode/encode_float/flush/destroy`,
  `glint_version()`. The AAC config struct carries a reserved tail and
  a **zero-init contract** so future options never break the ABI.
  Encoder delay is 2048 samples; `glint_aac_flush` returns the two
  tail frames.
- **AAC in all three bindings** (Python `AacEncoder`, Rust
  `AacEncoder`/`encode_pcm_aac`, Dart `GlintAacEncoder`) — and a fix
  for a latent bug in all of them: the mirrored MP3 config structs
  were missing fields the C struct has had since the quality/VBR
  features, so `glint_create` read uninitialized memory.
- The encoder league harness gained `--codec aac`
  (`tests/compare_encoders.py`): glint vs Apple afconvert, fdkaac,
  ffmpeg native and vo-aacenc, with PEAQ ODG / PESQ / STOI columns.
- A listening pack generator convention and a double-blind ABX tool
  (`tests/abx.py`) for ear-verification of any pair.

## Compatibility notes

- `struct glint_aac_config` must be **zero-initialized** (`= {0}` or
  `memset`) before filling — reserved fields select defaults.
- The fixed-point (`GLINT_MODE=fixed`) AAC build trades ~0.7–1.5 dB
  NMR on stereo versus the float builds (the irreducible half-LSB of
  integer M/S, proven by an L==R experiment; mono is identical,
  castanets is better). Desktop builds are unaffected.
- MP3 encoding is bit-for-bit identical to v0.7.0 in the double path
  and at `-q normal/best` in the fixed path; only fixed `-q speed`
  changed (integer quantizer, metrics-identical).
