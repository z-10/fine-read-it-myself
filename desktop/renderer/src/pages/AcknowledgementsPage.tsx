// Everything the app is built on: models, voices and software, with their authors and licenses.
type Entry = { name: string; by: string; license: string; url: string; note?: string }

const MODELS: Entry[] = [
  { name: 'VoxCPM2', by: 'OpenBMB; GGUF by the audio.cpp project', license: 'Apache-2.0',
    url: 'https://huggingface.co/audio-cpp/audio.cpp-gguf', note: 'speech: reads the script in each character’s cloned voice' },
  { name: 'BookNLP', by: 'David Bamman (UC Berkeley)', license: 'MIT', url: 'https://github.com/booknlp/booknlp',
    note: 'speaker detection: entity tagging and coreference, plus its name aliases and gender statistics' },
  { name: 'ModernBookNLP', by: 'Gaspard Michel', license: 'MIT', url: 'https://huggingface.co/gasmichel/ModernBookNLP',
    note: 'speaker detection: quote attribution' },
  { name: 'BookNLP + ModernBookNLP in GGUF', by: 'converted for this app', license: 'MIT',
    url: 'https://huggingface.co/zloezlo/modernbooknlp-gguf' },
  { name: 'BERT (uncased, L-6 / L-12)', by: 'Google', license: 'Apache-2.0', url: 'https://github.com/google-research/bert',
    note: 'vocabulary and configuration of the BookNLP models' },
  { name: 'ModernBERT-large', by: 'Answer.AI', license: 'Apache-2.0', url: 'https://huggingface.co/answerdotai/ModernBERT-large',
    note: 'tokenizer and configuration of the ModernBookNLP model' },
  { name: 'spaCy (en_core_web_sm tokenizer rules)', by: 'Explosion AI', license: 'MIT', url: 'https://spacy.io',
    note: 'the speaker detection tokenizes text exactly as spaCy does' },
  { name: 'Qwen3.5-4B and Qwen3.5-9B', by: 'Alibaba Qwen; GGUF by Unsloth', license: 'Apache-2.0', url: 'https://huggingface.co/Qwen',
    note: 'local script producers' },
  { name: 'Gemma 4 12B', by: 'Google DeepMind; GGUF by Unsloth', license: 'Apache-2.0', url: 'https://ai.google.dev/gemma',
    note: 'local script producer' },
]

const SOFTWARE: Entry[] = [
  { name: 'ggml', by: 'the ggml authors (as vendored by audio.cpp)', license: 'MIT', url: 'https://github.com/ggml-org/ggml' },
  { name: 'llama.cpp (libllama, JSON-schema grammars)', by: 'the ggml / llama.cpp authors', license: 'MIT', url: 'https://github.com/ggml-org/llama.cpp' },
  { name: 'audio.cpp (VoxCPM2 runtime)', by: 'ShugoAI LLC', license: 'Apache-2.0', url: 'https://github.com/0xShug0/audio.cpp' },
  { name: 'cpp-httplib', by: 'Yuji Hirose', license: 'MIT', url: 'https://github.com/yhirose/cpp-httplib' },
  { name: 'OpenSSL', by: 'the OpenSSL Project', license: 'Apache-2.0', url: 'https://www.openssl.org' },
  { name: 'SQLite', by: 'D. Richard Hipp and contributors', license: 'public domain', url: 'https://sqlite.org' },
  { name: 'lexbor (HTML parsing)', by: 'Alexander Borisov', license: 'Apache-2.0', url: 'https://github.com/lexbor/lexbor' },
  { name: 'glint (MP3 encoding)', by: 'CrispStrobe', license: 'MIT', url: 'https://github.com/CrispStrobe/glint' },
  { name: 'libyaml', by: 'Kirill Simonov, Ingy döt Net', license: 'MIT', url: 'https://github.com/yaml/libyaml' },
  { name: 'cJSON', by: 'Dave Gamble and contributors', license: 'MIT', url: 'https://github.com/DaveGamble/cJSON' },
  { name: 'nlohmann/json', by: 'Niels Lohmann', license: 'MIT', url: 'https://github.com/nlohmann/json' },
  { name: 'miniz (reading the voice archives)', by: 'Rich Geldreich and contributors', license: 'MIT', url: 'https://github.com/richgel999/miniz' },
  { name: 'Electron', by: 'the Electron authors (with Chromium and Node.js)', license: 'MIT', url: 'https://www.electronjs.org' },
  { name: 'React', by: 'Meta Platforms and contributors', license: 'MIT', url: 'https://react.dev' },
]

function Table({ rows }: { rows: Entry[] }) {
  return (
    <table className="setup">
      <tbody>
        {rows.map((e) => (
          <tr key={e.name}>
            <td><a href={e.url} target="_blank" rel="noreferrer"><b>{e.name}</b></a>
              {e.note && <div className="small muted">{e.note}</div>}</td>
            <td className="small muted">{e.by}</td>
            <td className="small muted">{e.license}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

export default function AcknowledgementsPage() {
  return (
    <section>
      <h1 className="page-title">Acknowledgements</h1>
      <p className="muted">This app stands on the work of many people. Everything runs on this computer; the models are
        downloaded from their publishers on first start.</p>

      <h3>Models</h3>
      <Table rows={MODELS} />

      <h3>Voices</h3>
      <p>
        The voices are readers of <a href="https://librivox.org/" target="_blank" rel="noreferrer">LibriVox</a>, the volunteers
        who record public-domain books, taken from <a href="https://www.openslr.org/141/" target="_blank" rel="noreferrer">LibriTTS-R</a> (Yuma
        Koizumi, Heiga Zen, Shigeki Karita, Yifan Ding, Kohei Yatabe, Nobuyuki Morioka, Michiel Bacchiani, Yu Zhang, Wei Han and
        Ankur Bapna, “LibriTTS-R: A Restored Multi-Speaker Text-to-Speech Corpus”, 2023), licensed{' '}
        <a href="https://creativecommons.org/licenses/by/4.0/" target="_blank" rel="noreferrer">CC BY 4.0</a>. Each voice is named
        after its reader. Changes: short reference clips were cut from the recordings; the sample lines were synthesized with
        VoxCPM2. The clips, and which reader each comes from, are in the{' '}
        <a href="https://huggingface.co/datasets/zloezlo/fine-read-it-voices" target="_blank" rel="noreferrer">voice catalog</a>.
      </p>

      <h3>Software</h3>
      <Table rows={SOFTWARE} />
    </section>
  )
}
