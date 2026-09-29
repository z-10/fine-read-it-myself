# glint v0.9.0 — the three-codec release

72 commits since v0.8.0. glint is no longer a dual-codec **encoder**: it is
now a **three-codec suite — MP3, AAC-LC and Opus — that both encodes and
decodes**, under a single MIT license, entirely clean-room, with one CLI
and Python / Rust / Dart bindings over the whole thing. Every codec is
complete in both directions; the Opus codec is new in this release, and
both the MP3 and AAC decoders are new. Every claim below is measured and
gated in CI; the full experiment log — including the dead ends — lives in
`PLAN.md`.

## Headline

- **Opus, from scratch (RFC 6716 / 7845 / 8251).** A clean-room,
  **RFC-conformant decoder** (12/12 official test vectors, verified by the
  normative `opus_compare`) — SILK, CELT and hybrid, with PLC, SILK in-band
  FEC, output gain, non-48 kHz output rates, and multistream/surround — plus
  a **competitive CELT encoder** validated on every frame by libopus's own
  decoder. Ogg `.opus` read and write. In the 10-clip PEAQ ODG league the
  glint encoder is **transparent at 192 kbps everywhere** and at 96 kbps
  wins or ties most clips against libopus.
- **MP3 and AAC-LC decoders, from scratch.** Clean-room MPEG-1/2 Layer III
  (all window types, reservoir, M/S + intensity stereo) and AAC-LC (all four
  window sequences, M/S, TNS, PNS, intensity) decoders — each built from the
  *encoder's own* Huffman tables, so encode and decode cannot drift. The MP3
  decoder matches ffmpeg to **128–131 dB** across 19 streams (LAME included);
  the AAC decoder matches ffmpeg **and** Apple CoreAudio, and its FFT-based
  IMDCT decodes at **~440× realtime**.
- **A codec Swiss-army-knife CLI.** `glint_cli` now encodes, decodes and
  transcodes MP3 / AAC / Opus over a universal float-PCM pipeline, with
  WAV/raw I/O at any bit depth, resampling, gain, peak-normalize, `--info`,
  and stdin/stdout piping.
- **Bindings for everything.** Python, Rust and Dart wrappers now cover
  decode, transcode, resample, WAV I/O and one-call encode for all three
  codecs — not just MP3/AAC encode.

## The Opus codec

Built bottom-up from the RFC in gated milestones (`PLAN.md` § O0–O5):

- **Range coder** byte-identical to libopus (the foundation everything keys
  on: complemented decoder value, raw bits filled from the buffer end,
  0xFF carry chains, Q15 `tell_frac`).
- **CELT decoder** — energy-envelope decode, bit allocator, PVQ shapes,
  mixed-radix inverse MDCT, anti-collapse — and **CELT encoder**: prefilter,
  transient detection with short blocks, `tf_analysis`, `alloc_trim`,
  dynalloc with wire boosts, spread/tapset, intensity + dual-stereo,
  unconstrained VBR, and FFT-phase tonality analysis (no MLP). libopus's
  decoder verifies our final range on **every** encoded frame.
- **SILK decoder** — bit-exact: NLSF→LPC chain, LTP, stereo unmix,
  excitation shell coder — completing the decoder to full RFC conformance.
- **O5 polish**: SILK in-band FEC decode, output rates 8–48 kHz, output
  gain, and multistream/surround (mapping family 1, 5.1 + quad) decode.

The whole O0–O5 roadmap is done. Each layer has its own crosscheck gate
against libopus; the decoder passes all 12 official vectors, and the encoder
gate is libopus decoding glint's streams with a per-packet range identity.

## MP3 + AAC-LC decoders

Both decoders complete their codec in the decode direction, so glint is now
symmetric on all three:

- **MP3 (D1)**: MPEG-1 and MPEG-2 LSF, bit reservoir, all block/window
  types, M/S and intensity stereo, polyphase synthesis. Gated at **19/19 vs
  ffmpeg, 128–131 dB** (including LAME streams and hand-built intensity
  frames no encoder emits).
- **AAC-LC (D2)**: ADTS, all four window sequences, M/S, TNS, PNS
  (codebook 13), intensity (14/15). glint round-trips at **86–135 dB by
  SNR**; foreign ffmpeg/FDK/Apple streams (whose PNS is decoder-random) match
  in the spectral-envelope domain. The direct O(N²) IMDCT was replaced by
  inverting the encoder's proven MDCT — **403× faster, ~440× realtime**,
  reconstruction-identical.
- **Dual-reference validation**: the AAC decoder is checked against ffmpeg
  **and** Apple CoreAudio, which decode glint's streams metrically identical.

## CLI + bindings

The CLI is now a general codec tool over one interleaved-float pipeline:

```
glint_cli in.wav  out.opus          # encode (mp3 | aac | opus)
glint_cli in.mp3  out.wav           # decode any codec -> WAV/raw
glint_cli in.mp3  out.aac           # transcode
glint_cli --info in.opus            # format / rate / channels / duration
… | glint_cli -F mp3 - -  | …       # stdin/stdout piping
  --rate HZ  --gain DB  --norm[=DB]  --bits 8|16|24|32  --wav-float
```

WAV I/O reads PCM 8/16/24/32, IEEE float 32/64, A-law, µ-law and
WAVE_FORMAT_EXTENSIBLE, and writes any of those depths. Opus output
auto-resamples to 48 kHz; the resampler is a dependency-free Kaiser-windowed
sinc.

**Bindings (Python / Rust / Dart)** gained, over a shared C ABI:
`decode_audio` / `decode_audio_ex` (whole-file auto-detect, output rate,
int16-or-float output, Opus surround up to 8 ch), `encode_audio` (one call,
auto-resamples to a codec-valid rate), `wav_read` / `wav_write` at any bit
depth, `resample`, and Opus file encode. The low-level encoders expose the
full config (MP3 mode/quality/VBR, AAC VBR). Each addition ships with unit +
live round-trip tests wired into `ctest`.

## Robustness and security

glint's entire job is parsing untrusted compressed audio, so the decode
surface is fuzzed under sanitizers as a CI gate (`decoder_fuzz`):

- **Frame decoders** (MP3 / AAC under ASan+UBSan, Opus under ASan) survive
  random, bit-flipped and truncated input with no crash, OOB or hang. This
  found and fixed **four AAC decoder bugs** on malformed input (an infinite
  loop, a heap overflow, a stack overflow, and an OOB read).
- **Container parsers** (the WAV reader and the Ogg-Opus demuxer) are now
  fuzzed too. A parser-hardening audit found and fixed a **WAV
  WAVE_FORMAT_EXTENSIBLE out-of-bounds read** (a truncated `fmt ` chunk
  claiming a 40-byte size read past the buffer); the Ogg demuxer audited
  clean.

## Compatibility

- **Additive and backward-compatible.** All v0.8.0 APIs are unchanged; the
  new decode/transcode/resample/WAV/Opus entry points are additions to the C
  ABI and the three wrappers.
- **Builds**: `double` (default), `fixed` (integer, no-FPU MP3/AAC hot
  paths), and `both` (runtime `-p`) all pass CI on Linux and macOS. The
  embedded/no-FPU encoder core is unchanged — the new whole-file convenience
  code is desktop-only and not compiled into the microcontroller build.
- **ctest is now 8 gates**: unit tests, MPEG-2 decode quality, AAC encode
  quality, MP3 and AAC decoder-vs-ffmpeg, the CLI feature gate, the Python
  bindings, and the decoder+container fuzz gate.

The full per-item history and every measured result (Opus O0–O5, the MP3/AAC
decoders, the CLI/wrapper flexibility pass, and the parser audit) is in
`PLAN.md`.
