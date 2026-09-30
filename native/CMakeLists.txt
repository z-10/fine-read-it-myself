# readmyself native engine: one ggml shared by VoxCPM2 (from audio.cpp), the director LLM
# (from llama.cpp) and the ModernBookNLP port. GPU backend: Vulkan (Windows/Linux), Metal (macOS).
cmake_minimum_required(VERSION 3.21)
project(readmyself_native C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (APPLE)
    set(RM_DEFAULT_GPU metal)
else()
    set(RM_DEFAULT_GPU vulkan)
endif()
set(RM_GPU ${RM_DEFAULT_GPU} CACHE STRING "GPU backend: vulkan, metal or none")
set_property(CACHE RM_GPU PROPERTY STRINGS vulkan metal none)

# ---------------------------------------------------------------- ggml (audio.cpp's fork)
# static: every tool/server is a single binary (no DLL search-path issues in the app bundle)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
if (RM_GPU STREQUAL "vulkan")
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
elseif (RM_GPU STREQUAL "metal")
    set(GGML_METAL ON CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ggml)

# ---------------------------------------------------------------- small vendored C libraries
add_library(rm_cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(rm_cjson PUBLIC third_party/cjson)
target_compile_definitions(rm_cjson PUBLIC CJSON_HIDE_SYMBOLS)

set(RM_LIBYAML_DEFS _POSIX_C_SOURCE=200809L YAML_DECLARE_STATIC=1
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING=\"0.2.5\")
add_library(rm_yaml STATIC
    third_party/libyaml/src/api.c third_party/libyaml/src/reader.c third_party/libyaml/src/scanner.c
    third_party/libyaml/src/parser.c third_party/libyaml/src/loader.c)
target_include_directories(rm_yaml PUBLIC third_party/libyaml/include)
target_compile_definitions(rm_yaml PUBLIC ${RM_LIBYAML_DEFS})

# ---------------------------------------------------------------- audio: VoxCPM2 + framework subset
file(GLOB_RECURSE RM_AUDIO_SOURCES CONFIGURE_DEPENDS audio/src/*.cpp)
# model contracts compiled in as @builtin/<family> (audio.cpp does this only in deployment builds;
# the published VoxCPM2 GGUF does not embed its spec)
set(RM_MODEL_SPECS_INC "")
foreach(family voxcpm2)
    set(spec "${CMAKE_CURRENT_SOURCE_DIR}/audio/model_specs/${family}.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spec}")
    file(READ "${spec}" json)
    string(APPEND RM_MODEL_SPECS_INC "    {\"${family}\", R\"AUDIOCPP_SPEC(${json})AUDIOCPP_SPEC\"},\n")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/model_specs.inc" CONTENT "${RM_MODEL_SPECS_INC}" @ONLY)
add_library(rm_audio STATIC ${RM_AUDIO_SOURCES})
target_include_directories(rm_audio PUBLIC audio/include)
target_include_directories(rm_audio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
find_package(Threads REQUIRED)
target_link_libraries(rm_audio PUBLIC ggml PRIVATE rm_cjson rm_yaml Threads::Threads ${CMAKE_DL_LIBS})
if (MSVC)
    target_compile_options(rm_audio PRIVATE /permissive- /utf-8 /bigobj)
    target_compile_definitions(rm_audio PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()

# ---------------------------------------------------------------- llm: libllama, every model architecture (local script producers)
# llama.cpp commit 60499061 (2026-05-16): the upstream ggml that audio.cpp's fork is based on
file(GLOB RM_LLM_SOURCES CONFIGURE_DEPENDS llm/src/*.cpp llm/src/models/*.cpp)
add_library(rm_llm STATIC ${RM_LLM_SOURCES})
target_include_directories(rm_llm PUBLIC llm/include PRIVATE llm/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rm_llm PUBLIC ggml)
# no unity build: upstream model files define same-named static helpers (e.g. ggml_view_2d_slice)

# JSON schema -> GBNF (llama.cpp common/json-schema-to-grammar) for schema-constrained director output
add_library(rm_llm_schema STATIC llm/common/json-schema-to-grammar.cpp)
target_include_directories(rm_llm_schema PUBLIC llm/common third_party)
foreach(t rm_llm rm_llm_schema)
    if (MSVC)
        target_compile_options(${t} PRIVATE /utf-8 /bigobj)
        target_compile_definitions(${t} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()

# ---------------------------------------------------------------- tools
add_executable(rm-tts tools/rm_tts.cpp)
target_link_libraries(rm-tts PRIVATE rm_audio)
if (MSVC)
    target_compile_options(rm-tts PRIVATE /permissive- /utf-8)
    target_compile_definitions(rm-tts PRIVATE NOMINMAX)
endif()

add_executable(rm-llm tools/rm_llm.cpp)
target_link_libraries(rm-llm PRIVATE rm_llm rm_llm_schema)
if (MSVC)
    target_compile_options(rm-llm PRIVATE /utf-8)
    target_compile_definitions(rm-llm PRIVATE NOMINMAX)
endif()

# ---------------------------------------------------------------- booknlp: ModernBookNLP port
add_library(rm_booknlp STATIC booknlp/src/tokenizer.cpp booknlp/src/ggml_model.cpp booknlp/src/bert.cpp
    booknlp/src/frontend.cpp booknlp/src/entity_tagger.cpp
    booknlp/src/quotes.cpp booknlp/src/bpe.cpp booknlp/src/modernbert.cpp booknlp/src/quote_attrib.cpp
    booknlp/src/names.cpp booknlp/src/gender.cpp booknlp/src/coref.cpp booknlp/src/pipeline.cpp
    booknlp/src/convert.cpp)
target_include_directories(rm_booknlp PUBLIC booknlp/src PRIVATE llm/src third_party)
target_link_libraries(rm_booknlp PUBLIC rm_llm)
add_executable(rm-bnlp booknlp/tools/rm_bnlp.cpp)
target_link_libraries(rm-bnlp PRIVATE rm_booknlp)
target_include_directories(rm-bnlp PRIVATE third_party)
foreach(t rm_booknlp rm-bnlp)
    if (MSVC)
        target_compile_options(${t} PRIVATE /utf-8 /bigobj)
        target_compile_definitions(${t} PRIVATE NOMINMAX)
    endif()
endforeach()

# ---------------------------------------------------------------- server: readmyself backend (HTTP API)
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

add_library(rm_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(rm_miniz PUBLIC third_party/miniz)

add_library(rm_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(rm_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(rm_sqlite PRIVATE SQLITE_DQS=0 SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lexbor EXCLUDE_FROM_ALL)

# glint MP3 encoder (double-precision signal path, no -march=native)
file(GLOB RM_GLINT_SOURCES CONFIGURE_DEPENDS third_party/glint/src/*.cpp)
add_library(rm_glint STATIC ${RM_GLINT_SOURCES})
target_include_directories(rm_glint PUBLIC third_party/glint/include PRIVATE third_party/glint/src)
if (MSVC)
    target_compile_definitions(rm_glint PRIVATE _USE_MATH_DEFINES NOMINMAX)
    target_compile_options(rm_glint PRIVATE /utf-8)
endif()

# built-in resources (frozen schemas, director prompt, source definitions) compiled in
file(GLOB_RECURSE RM_RESOURCE_FILES CONFIGURE_DEPENDS server/resources/*)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RM_RESOURCE_FILES})
set(RM_RESOURCES_INC "")
foreach(f ${RM_RESOURCE_FILES})
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}/server/resources" "${f}")
    file(READ "${f}" content)
    string(APPEND RM_RESOURCES_INC "    {\"${rel}\", R\"RMRES(${content})RMRES\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/resources.inc" CONTENT "${RM_RESOURCES_INC}" @ONLY)

file(GLOB RM_SERVER_SOURCES CONFIGURE_DEPENDS server/*.cpp)
add_executable(readmyself-server ${RM_SERVER_SOURCES})
target_include_directories(readmyself-server PRIVATE server third_party third_party/httplib llm/src
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(readmyself-server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT LEXBOR_STATIC)
target_link_libraries(readmyself-server PRIVATE rm_booknlp rm_llm_schema rm_audio rm_yaml rm_sqlite rm_miniz lexbor_static rm_glint
    OpenSSL::SSL OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(readmyself-server PRIVATE ws2_32 crypt32 user32 advapi32)
    if (RM_GPU STREQUAL "vulkan")   # the Vulkan loader comes with the GPU driver: start without it (CPU), see main()
        target_link_libraries(readmyself-server PRIVATE delayimp)
        target_link_options(readmyself-server PRIVATE /DELAYLOAD:vulkan-1.dll)
    endif()
endif()
if (MSVC)
    target_compile_options(readmyself-server PRIVATE /utf-8 /bigobj /permissive-)
    target_compile_definitions(readmyself-server PRIVATE NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------- bundle for the desktop app
option(RM_BUILD_TESTS "Build native source plugin regression tests" OFF)
if (RM_BUILD_TESTS)
    enable_testing()
    add_executable(rm-source-tests tests/sources.cpp server/sources.cpp server/scrape.cpp
        server/settings.cpp server/resources.cpp server/text.cpp server/net.cpp server/library.cpp server/db.cpp)
    target_include_directories(rm-source-tests PRIVATE server third_party third_party/httplib llm/src
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(rm-source-tests PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT LEXBOR_STATIC)
    target_link_libraries(rm-source-tests PRIVATE rm_booknlp rm_llm rm_yaml rm_sqlite lexbor_static OpenSSL::SSL OpenSSL::Crypto)
    if (WIN32)
        target_link_libraries(rm-source-tests PRIVATE ws2_32 crypt32)
    endif()
    if (MSVC)
        target_compile_options(rm-source-tests PRIVATE /utf-8 /bigobj /permissive-)
        target_compile_definitions(rm-source-tests PRIVATE NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME source-plugins COMMAND rm-source-tests)
endif()

# cmake --install build --prefix build/bundle  ->  bin/readmyself-server + app-local runtime libraries
install(TARGETS readmyself-server RUNTIME DESTINATION bin)
set(CMAKE_INSTALL_OPENMP_LIBRARIES ON)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
include(InstallRequiredSystemLibraries)
