cmake_minimum_required(VERSION 3.16)
project(glint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC does not define M_PI etc. without this (the Opus/decoder/resample
# sources use M_PI). Applies to all build types, not just Release.
if(MSVC)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Enable optimizations in Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2 /fp:fast)
    elseif(NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
        # -march=native only works for native builds, not cross-compilation
        add_compile_options(-O3 -march=native -ffast-math)
    else()
        add_compile_options(-O3 -ffast-math)
    endif()
    # Link-time optimization for cross-TU inlining
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Signal path mode:
#   double (default) — double-precision only
#   fixed            — Q31 fixed-point only
#   both             — both compiled in, selectable at runtime via CLI -p flag
set(GLINT_MODE "double" CACHE STRING "Signal path: double, fixed, or both")
set_property(CACHE GLINT_MODE PROPERTY STRINGS double fixed both)

set(GLINT_SOURCES
    src/subband.cpp
    src/resample.cpp
    src/mdct.cpp
    src/quantize.cpp
    src/huffman.cpp
    src/reservoir.cpp
    src/bitstream.cpp
    src/psycho.cpp
    src/encoder.cpp
    src/aac_mdct.cpp
    src/aac_coder.cpp
    src/aac_psy.cpp
    src/aac_tns.cpp
    src/aac_encoder.cpp
    src/opus_ec.cpp
    src/opus_laplace.cpp
    src/opus_cwrs.cpp
    src/opus_celt_energy.cpp
    src/opus_celt_rate.cpp
    src/opus_celt_bands.cpp
    src/opus_celt_decoder.cpp
    src/opus_decoder.cpp
    src/opus_ms_decoder.cpp
    src/opus_c_api.cpp
    src/mp3_decoder.cpp
    src/aac_decoder.cpp
    src/flac_decoder.cpp
    src/decoder_c_api.cpp
    src/decode_audio_c_api.cpp
    src/wav_io.cpp
    src/wav_c_api.cpp
    src/encode_audio_c_api.cpp
    src/opus_mdct.cpp
    src/opus_ogg.cpp
    src/vorbis_decoder.cpp
    src/vorbis_c_api.cpp
    src/opus_celt_enc_energy.cpp
    src/opus_celt_enc_vq.cpp
    src/opus_celt_enc_bands.cpp
    src/opus_celt_encoder.cpp
    src/opus_analysis.cpp
    src/opus_celt_pitch.cpp
    src/opus_silk_excitation.cpp
    src/opus_silk_indices.cpp
    src/opus_silk_nlsf.cpp
    src/opus_silk_plc.cpp
    src/opus_silk_frame.cpp
    src/opus_silk_stereo.cpp
    src/opus_silk_resampler.cpp
    src/opus_silk_decoder.cpp
)

# Static library
add_library(glint_static STATIC ${GLINT_SOURCES})
target_include_directories(glint_static PUBLIC include PRIVATE src)
set_target_properties(glint_static PROPERTIES OUTPUT_NAME glint)

# Shared library
add_library(glint_shared SHARED ${GLINT_SOURCES})
target_include_directories(glint_shared PUBLIC include PRIVATE src)
set_target_properties(glint_shared PROPERTIES OUTPUT_NAME glint)

if(GLINT_MODE STREQUAL "fixed")
    target_compile_definitions(glint_static PUBLIC GLINT_FIXED_POINT GLINT_SMALL_BUFFERS GLINT_SMALL_POW34 GLINT_AAC_INT GLINT_MP3_INT)
    target_compile_definitions(glint_shared PUBLIC GLINT_FIXED_POINT GLINT_SMALL_BUFFERS GLINT_SMALL_POW34 GLINT_AAC_INT GLINT_MP3_INT)
elseif(GLINT_MODE STREQUAL "both")
    target_compile_definitions(glint_static PUBLIC GLINT_FIXED_POINT GLINT_BOTH_PATHS)
    target_compile_definitions(glint_shared PUBLIC GLINT_FIXED_POINT GLINT_BOTH_PATHS)
endif()

# CLI
add_executable(glint_cli cli/main.cpp)
target_link_libraries(glint_cli PRIVATE glint_static)
target_include_directories(glint_cli PRIVATE src include cli)

# Opus tools: raw-PCM -> opus_demo .bit encoder and .bit -> Ogg .opus
# muxer (used by the encoder league and ad-hoc listening tests).
add_executable(opus_enc_cli tools/opus_enc_cli.cpp)
target_link_libraries(opus_enc_cli PRIVATE glint_static)
target_include_directories(opus_enc_cli PRIVATE src)
add_executable(opus_mux_cli tools/opusfile_mux_cli.cpp)
target_link_libraries(opus_mux_cli PRIVATE glint_static)
target_include_directories(opus_mux_cli PRIVATE src)

# Ogg-Vorbis decoder CLI: whole .ogg -> f32le PCM (used by the dB gate).
add_executable(vorbis_dec_cli tools/vorbis_dec_cli.cpp)
target_link_libraries(vorbis_dec_cli PRIVATE glint_static)
target_include_directories(vorbis_dec_cli PRIVATE src include)

# Unit tests
enable_testing()
add_executable(glint_test tests/test_unit.cpp)
target_link_libraries(glint_test PRIVATE glint_static)
target_include_directories(glint_test PRIVATE src)
add_test(NAME unit_tests COMMAND glint_test)

# Decode-based MPEG-2 regression test: encodes at 22.05 kHz (long + forced
# short blocks + VBR), decodes with ffmpeg, asserts SNR floors. This is the
# only test that catches m2 WIRE bugs — the unit suite historically passed
# while every real decoder produced garbage (invented scalefac_compress
# mapping, MPEG-1-copied short-sfb tables). Registered only when a Python
# with numpy and an ffmpeg binary are available.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    # Python wrapper gate: encoder/decoder classes + the buckets A+B
    # convenience helpers (resample, decode_file, transcode_file, WAV I/O)
    # against the freshly built shared library.
    add_test(NAME python_bindings
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/test_bindings.py
                     $<TARGET_FILE_DIR:glint_shared>)

    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy"
                    RESULT_VARIABLE GLINT_PY_NUMPY_RC
                    OUTPUT_QUIET ERROR_QUIET)
    find_program(GLINT_FFMPEG ffmpeg)
    if(GLINT_PY_NUMPY_RC EQUAL 0 AND GLINT_FFMPEG)
        add_test(NAME m2_decode_quality
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_quality.py
                         $<TARGET_FILE:glint_cli> --m2-only)
        # AAC wire-correctness gate: encode synthetic signal, decode with
        # ffmpeg, assert zero decoder errors + SNR floors.
        add_test(NAME aac_decode_quality
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aac.py
                         $<TARGET_FILE:glint_cli>)
        # MP3 DECODER gate: decode glint- and LAME-encoded streams with
        # Mp3Decoder and ffmpeg, require ~130 dB agreement.
        add_test(NAME mp3_decoder_vs_ffmpeg
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_mp3_decoder.py
                         $<TARGET_FILE:glint_cli>)
        # AAC-LC DECODER gate: glint roundtrip by SNR + foreign (ffmpeg/
        # fdk/apple) PNS streams by spectral-envelope + energy.
        add_test(NAME aac_decoder_vs_ffmpeg
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_aac_decoder.py
                         $<TARGET_FILE:glint_cli>)
        # CLI gate: encode/decode/transcode/resample/gain/pipe end to end.
        add_test(NAME cli_features
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_cli.py
                         $<TARGET_FILE:glint_cli>)
        # DECODER ROBUSTNESS gate: fuzz MP3/AAC under ASan+UBSan with
        # random / bit-flipped / truncated input — no crash, OOB or hang.
        add_test(NAME decoder_fuzz
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_decoder_fuzz.py
                         $<TARGET_FILE:glint_cli>)
        set_tests_properties(decoder_fuzz PROPERTIES
                 ENVIRONMENT "FUZZ_ITERS=700" TIMEOUT 700)
        # OGG-VORBIS DECODER gate: decode a sox(libvorbis) corpus with glint,
        # ffmpeg and sox; require glint to match both references (raw >=100 dB,
        # shape >=120 dB vs ffmpeg; parity with ffmpeg vs sox). Deterministic.
        add_test(NAME vorbis_decoder_vs_ffmpeg
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_vorbis_decoder.py
                         $<TARGET_FILE:vorbis_dec_cli>)
        add_test(NAME flac_decoder_vs_ffmpeg
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_flac_decoder.py
                         $<TARGET_FILE:glint_cli>)
    else()
        message(STATUS "m2_decode_quality test skipped (needs python3+numpy and ffmpeg)")
    endif()
endif()
