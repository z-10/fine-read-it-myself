# Fine, I'll read it myself!

Have you ever found out that the web novel you love has no audio version? Or that the audio version is hundreds of chapters behind? You didn't? Well, I did. And I decided to do something about it myself.

So here it is: a desktop app that turns web novels into multi-voice audiobooks, with a different voice for every character, entirely on your own computer. No subscription, no waiting for the next audiobook release, and nothing leaves your machine.

Track a novel from a supported site (Royal Road, Wuxiaworld and NovelFire are built in; more sites are declarative YAML plugins), and every chapter goes through three steps, each of which you can inspect and edit:

1. **Analysis** — the chapter is split into narration and quotes, and [ModernBookNLP](https://huggingface.co/gasmichel/ModernBookNLP) / [BookNLP](https://github.com/booknlp/booknlp) detect who speaks each quote (a C++/ggml port, run on your GPU).
2. **Script** — a *producer* double-checks every speaker: a local LLM (Qwen3.5-4B, Qwen3.5-9B or Gemma 4 12B, via llama.cpp) or any OpenAI-compatible endpoint labels the quotes independently; where it disagrees with step 1 it decides between the two, and the uncertain cases are marked for you to confirm. It also describes the characters.
3. **Narration** — each character gets a persistent voice from a pool of tested LibriVox readers, and [VoxCPM2](https://huggingface.co/audio-cpp/audio.cpp-gguf) (via audio.cpp) reads the script into a tagged MP3.

Everything runs locally on Vulkan (NVIDIA, AMD, Intel); models download from their publishers on first start.

## Use it from other devices

The same UI works in any browser on your network:

- **From the desktop app:** Settings → *Share on my network*, then open the address it shows (e.g. `http://192.168.0.11:8765`) on your phone, tablet or another computer.
- **As a server without a window:** `fine-read-it --serve [--port 8765]` runs in the tray (Open in browser, Quit), always shared on that port.

There is no password: anyone on your network can use it, so only share on a network you trust.

## Layout

### Site plugins

The **Sites** tab lists every installed site and keeps its login cookie and request delay settings together.
Changes apply to the next request; existing saved site logins are retained. Leave the delay empty to use the
plugin default. Cookies are masked in API responses and can be removed with **Forget login**.

Paste a book or chapter URL into the the library. Sites can still reject
automated requests (for example, HTTP 403); a plugin does not solve browser challenges.

To add or override a site, put a `.yaml` file in `<data>/plugins` and restart the app. An optional
`user_sources_dir` in `settings.json` supplies a second folder whose definitions take precedence.
See [the plugin format](native/server/SOURCES.md) and the built-in definitions in `native/server/resources/sources`.

### Code

| folder | what |
|---|---|
| `desktop/` | Electron app: `main.js` starts the backend; `renderer/` is the React + TypeScript UI |
| `native/` | C++ backend (`readmyself-server`, HTTP API) and its engines, sharing one ggml |
| `native/server/` | API, library/chapter steps, producers, voices, model downloads |
| `native/booknlp/` | ModernBookNLP / BookNLP port (tokenizer, BERT/ModernBERT graphs, entities, coreference, quote attribution) and the GGUF converter |
| `native/audio/` | VoxCPM2 runtime from audio.cpp |
| `native/llm/` | libllama from llama.cpp (commit 60499061, matching audio.cpp's ggml) |
| `native/third_party/` | ggml, cpp-httplib, lexbor, SQLite, libyaml, cJSON, nlohmann/json, glint, miniz — see `native/NOTICE` |

## Build (Windows)

Requirements: Visual Studio 2022 Build Tools, CMake + Ninja, the Vulkan SDK, OpenSSL (static libraries), Node.js.

```bat
cd native
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRM_GPU=vulkan
cmake --build build
cd ..\desktop
npm install && npm --prefix renderer install
npm run build:renderer
npm start
```

The app ships with six voices in `desktop/voices`: the three clearest male and three clearest female readers of the
[voice catalog](https://huggingface.co/datasets/zloezlo/fine-read-it-voices) (CC BY 4.0, see `ATTRIBUTION.txt` there);
the rest download from the Voices tab. `npm run dist` builds the installer.

## Models and data

| | source | license |
|---|---|---|
| VoxCPM2 (speech) | [audio-cpp/audio.cpp-gguf](https://huggingface.co/audio-cpp/audio.cpp-gguf) | Apache-2.0 |
| BookNLP + ModernBookNLP (speaker detection, GGUF) | [zloezlo/modernbooknlp-gguf](https://huggingface.co/zloezlo/modernbooknlp-gguf) | MIT |
| Qwen3.5-4B / 9B, Gemma 4 12B (local producers) | [unsloth](https://huggingface.co/unsloth) | Apache-2.0 |
| Voices (LibriTTS-R readers) | [zloezlo/fine-read-it-voices](https://huggingface.co/datasets/zloezlo/fine-read-it-voices) | CC BY 4.0 |

The app's Acknowledgements page lists every model, dataset and library with its authors.

## Fine, I'll code it myself

This project was built despite Claude's stubborn refusal to write scrape plugins for web novel websites because "reasons". 

## License

MIT (see `LICENSE`) for this project's own code. Vendored third-party code under `native/` keeps its own license (MIT, Apache-2.0 or public domain); see `native/NOTICE`.
