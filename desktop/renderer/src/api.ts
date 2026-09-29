// Backend API (readmyself-server, same origin).
export type Plugin = { id: string; name: string; homepage: string }
export type Novel = {
  id: number; source: string; url: string; title: string; author: string; cover: string; description: string
  narrator: string; director: string; added_at: number; checked_at: number | null; chapters?: number
}
export type Step = 'analyze' | 'script' | 'narrate'
export type ActiveJob = { id: number; kind: Step; state: 'queued' | 'running'; stage: string; progress: number }
export type Chapter = {
  id: number; novel_id: number; position: number; url: string; title: string; published: number | null
  analyzed_at: number | null; scripted_at: number | null; script_producer: string; narrated_at: number | null
  duration_s: number | null; quality_issues: string; error: string; story: boolean; jobs: ActiveJob[]
  to_check: number   // disputed speakers of the "check" kind the user has not looked at
}
// producers that can be used now: local models that are downloaded ("local" = the default one) and the user's endpoints
export type Producer = { name: string; label: string; local: boolean }
export type Pitch = 'all' | 'low' | 'mid' | 'high'
export type Gender = 'male' | 'female'
export type Age = 'child' | 'teen' | 'adult' | 'elder'
export type Character = {
  name: string; aliases: string[]; gender: Gender; age: Age; pitch: Pitch; voice: string; voice_hint: string
  locked: number; first_chapter: number | null; lines: number
}
export type Voice = {
  id: string; label: string; reader: string; gender: Gender; metadata_gender: string
  gender_overridden: boolean; banned: boolean; usable: boolean; band: string; f0: number
}
export type Job = {
  id: number; chapter_id: number; kind: Step; state: string; stage: string; progress: number; log: string
  director: string; chapter_title: string; novel_id: number; position: number
}
export type ProducerProfile = { name: string; base_url: string; api_key: string; model: string; json_schema: boolean }
export type Settings = {
  default_director: string; directors: ProducerProfile[]; check_interval_hours: number; mp3_bitrate: string
}

// the voice catalog (tested voices on the Hugging Face hub); installed = in the bundle or downloaded
export type CatalogVoice = { id: string; reader: string; gender: Gender; band: string; f0: number; installed: boolean; sample_url: string }
export type VoiceDownload = { running: boolean; done: number; total: number; current: string; error: string; log: string[] }
export type Status = { warnings: string[]; voices: number; queue: number }
export type SetupItem = { id: string; title: string; license: string; size: number; installed: boolean; group: string }
export type SetupState = {
  models_dir: string; voices_dir: string; items: SetupItem[]; complete: boolean; missing_bytes: number; installing: boolean
  current: string; done_bytes: number; total_bytes: number; error: string; log: string[]
}

// step artifacts (see native/server/steps.h)
// check: why step 1 is unsure about this quote's speaker (cleared when the user sets the speaker)
export type Span = { id: number; para: number; kind: 'narration' | 'dialogue'; text: string; speaker?: string; check?: string
  by?: 'user' }   // by: set or changed by the user - final, the producer never overrides it
export type Analysis = {
  version: number; title: string; url: string; detector: 'modernbooknlp' | 'none'; paragraphs: string[]
  spans: Span[]; characters: { name: string; gender: string }[]
}
export type CastEntry = { gender: Gender; age: Age; voice: string; aliases: string[] }
export type Line = {
  id: number; para: number; kind: 'title' | 'narration' | 'dialogue'; speaker: string; text: string
  confirmed?: boolean   // the user kept the speaker of a disputed line (sent once with the save)
}
// speaker check: a disputed quote (analysis span id; script line id + 1). level "check": no speech tag, ~1 in 3 wrong
export type Dispute = { id: number; from: string; blind: string; to: string; evidence: string; level?: 'check' | 'likely'; resolved?: boolean }
export type Script = {
  version: number; producer: string; title: string; cast: Record<string, CastEntry>; lines: Line[]; quality_issues: string[]
  review?: Dispute[]
}
export type Narration = { version: number; duration: number; lines: { id: number; voice: string; start: number; duration: number }[] }

async function req<T>(method: string, path: string, body?: unknown): Promise<T> {
  const r = await fetch(`./api${path}`, {
    method,
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  })
  if (!r.ok) {
    let msg = `${r.status} ${r.statusText}`
    try { msg = (await r.json()).detail ?? msg } catch { /* not JSON */ }
    throw new Error(msg)
  }
  return r.json()
}

const enc = encodeURIComponent

export const api = {
  status: () => req<Status>('GET', '/status'),
  setup: () => req<SetupState>('GET', '/setup'),
  installModels: (optional: string[] = []) => req<SetupState>('POST', '/setup/install', { optional }),
  plugins: () => req<Plugin[]>('GET', '/sources'),
  novels: () => req<Novel[]>('GET', '/novels'),
  track: (url: string) => req<Novel>('POST', '/novels', { url }),
  novel: (id: number) => req<Novel>('GET', `/novels/${id}`),
  patchNovel: (id: number, p: Partial<Pick<Novel, 'narrator' | 'director'>>) => req<Novel>('PATCH', `/novels/${id}`, p),
  untrack: (id: number) => req<{ ok: boolean }>('DELETE', `/novels/${id}`),
  chapters: (id: number) => req<Chapter[]>('GET', `/novels/${id}/chapters`),
  refresh: (id: number) => req<{ new_chapters: number }>('POST', `/novels/${id}/refresh`),
  process: (id: number, chapter_ids: number[], steps: Step[], producer = '') =>
    req<{ queued: number }>('POST', `/novels/${id}/process`, { chapter_ids, steps, producer }),

  chapter: (id: number) => req<Chapter>('GET', `/chapters/${id}`),
  run: (id: number, steps: Step[], producer = '') => req<{ queued: number }>('POST', `/chapters/${id}/run`, { steps, producer }),
  analysis: (id: number) => req<Analysis>('GET', `/chapters/${id}/analysis`),
  saveAnalysis: (id: number, a: Analysis) => req<Analysis>('PUT', `/chapters/${id}/analysis`, a),
  script: (id: number) => req<Script>('GET', `/chapters/${id}/script`),
  saveScript: (id: number, s: Script) => req<Script>('PUT', `/chapters/${id}/script`, s),
  narration: (id: number) => req<Narration>('GET', `/chapters/${id}/narration`),

  producers: () => req<Producer[]>('GET', '/directors'),
  cast: (id: number) => req<Character[]>('GET', `/novels/${id}/cast`),
  patchCharacter: (id: number, name: string, p: { voice?: string; gender?: Gender; age?: Age; pitch?: Pitch }) =>
    req<Character[]>('PATCH', `/novels/${id}/cast/${enc(name)}`, p),
  voices: () => req<Voice[]>('GET', '/voices'),
  voiceCatalog: () => req<{ url: string; voices: CatalogVoice[]; error: string; download: VoiceDownload }>('GET', '/voices/catalog'),
  downloadVoices: (ids: string[]) => req<VoiceDownload>('POST', '/voices/download', { ids }),
  voiceDownload: () => req<VoiceDownload>('GET', '/voices/download'),
  patchVoice: (id: string, p: { gender?: Gender | 'reset'; banned?: boolean }) =>
    req<{ voice: Voice; recast: string[] }>('PATCH', `/voices/${id}`, p),
  jobs: () => req<Job[]>('GET', '/jobs'),
  cancelJob: (id: number) => req('POST', `/jobs/${id}/cancel`),
  settings: () => req<Settings>('GET', '/settings'),
  saveSettings: (s: Settings) => req<Settings>('PUT', '/settings', s),
}

export const audioUrl = (chapterId: number, v?: number) => `./api/chapters/${chapterId}/audio${v ? `?v=${v}` : ''}`
export const lineAudioUrl = (chapterId: number, line: number, v?: number) =>
  `./api/chapters/${chapterId}/lines/${line}/audio${v ? `?v=${v}` : ''}`
export const sampleUrl = (voiceId: string) => `./api/voices/${voiceId}/sample`

// step state of a chapter for badges and buttons
export type StepState = 'none' | 'done' | 'outdated' | 'queued' | 'running'
export function stepState(c: Chapter, step: Step): StepState {
  const job = c.jobs.find((j) => j.kind === step)
  if (job) return job.state
  if (step === 'analyze') return c.analyzed_at ? 'done' : 'none'
  if (step === 'script') {
    if (!c.scripted_at) return 'none'
    return c.analyzed_at && c.analyzed_at > c.scripted_at ? 'outdated' : 'done'
  }
  if (!c.narrated_at) return 'none'
  return c.scripted_at && c.scripted_at > c.narrated_at ? 'outdated' : 'done'
}
export const STEP_LABEL: Record<Step, string> = { analyze: 'Analysis', script: 'Script', narrate: 'Audio' }
