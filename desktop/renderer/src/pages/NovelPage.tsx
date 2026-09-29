import { useEffect, useMemo, useState } from 'react'
import { api, audioUrl, sampleUrl, stepState, type Chapter, type Character, type Novel, type Pitch, type Producer, type Step, type Voice } from '../api'
import { ago, fmtDate, PITCHES, ProducerSelect, StepBadges, VoiceCells } from '../components'

const PAGE_SIZES = [25, 50, 100, 200, 0] as const   // 0 = all
type Filter = 'all' | 'no-analysis' | 'no-script' | 'no-audio' | 'outdated' | 'issues'
const FILTERS: [Filter, string][] = [
  ['all', 'all'], ['no-analysis', 'not analyzed'], ['no-script', 'no script'], ['no-audio', 'no audio'],
  ['outdated', 'outdated'], ['issues', 'quality issues'],
]

function matches(c: Chapter, f: Filter) {
  switch (f) {
    case 'no-analysis': return stepState(c, 'analyze') === 'none'
    case 'no-script': return stepState(c, 'script') === 'none'
    case 'no-audio': return stepState(c, 'narrate') === 'none'
    case 'outdated': return stepState(c, 'script') === 'outdated' || stepState(c, 'narrate') === 'outdated'
    case 'issues': return !!c.quality_issues
    default: return true
  }
}

export default function NovelPage({ id }: { id: number }) {
  const [novel, setNovel] = useState<Novel | null>(null)
  const [chapters, setChapters] = useState<Chapter[]>([])
  const [producers, setProducers] = useState<Producer[]>([])
  const [tab, setTab] = useState<'chapters' | 'cast'>('chapters')
  const [error, setError] = useState('')
  const [note, setNote] = useState('')
  const [checking, setChecking] = useState(false)
  const [showDesc, setShowDesc] = useState(false)

  const loadChapters = () => api.chapters(id).then(setChapters)
  useEffect(() => {
    Promise.all([api.novel(id), api.chapters(id)])
      .then(([n, c]) => { setNovel(n); setChapters(c) })
      .catch((e) => setError(e.message))
    api.producers().then(setProducers)
  }, [id])
  const busy = chapters.some((c) => c.jobs.length)
  useEffect(() => {
    const t = setInterval(loadChapters, busy ? 2000 : 10000)
    return () => clearInterval(t)
  }, [id, busy])

  async function check() {
    setChecking(true)
    setNote('')
    try {
      const r = await api.refresh(id)
      setNote(r.new_chapters ? `${r.new_chapters} new chapter${r.new_chapters > 1 ? 's' : ''}` : 'No new chapters')
      setNovel(await api.novel(id))
      await loadChapters()
    } catch (err) {
      setError((err as Error).message)
    } finally {
      setChecking(false)
    }
  }

  if (error) return <p className="error">{error}</p>
  if (!novel) return <p className="hint">Loading…</p>
  const audio = chapters.filter((c) => c.narrated_at).length

  return (
    <>
      <a href="#/" className="back">← Library</a>
      <section className="novel">
        {novel.cover ? <img src={novel.cover} alt="" /> : <div className="nocover big" />}
        <div className="info">
          <h1>{novel.title}</h1>
          <div className="author">{novel.author}</div>
          <div className="meta">
            {chapters.length} chapters · {audio} narrated · checked {ago(novel.checked_at)} ·{' '}
            <a href={novel.url} target="_blank" rel="noreferrer">open on site</a>
          </div>
          {novel.description && (
            <p className={showDesc ? 'desc' : 'desc clamp'} onClick={() => setShowDesc(!showDesc)}>{novel.description}</p>
          )}
          <div className="actions">
            <button onClick={check} disabled={checking}>{checking ? 'Checking…' : 'Check for new chapters'}</button>
            <label className="small">Producer{' '}
              <ProducerSelect producers={producers} value={novel.director}
                onChange={(v) => api.patchNovel(id, { director: v }).then(setNovel)} />
            </label>
            {note && <span className="note">{note}</span>}
          </div>
        </div>
      </section>

      <div className="tabs">
        <button className={tab === 'chapters' ? 'on' : ''} onClick={() => setTab('chapters')}>Chapters</button>
        <button className={tab === 'cast' ? 'on' : ''} onClick={() => setTab('cast')}>Cast</button>
      </div>
      {tab === 'chapters'
        ? <ChaptersTab novelId={id} chapters={chapters} producers={producers} reload={loadChapters} />
        : <CastTab novel={novel} setNovel={setNovel} />}
    </>
  )
}

function ChaptersTab({ novelId, chapters, producers, reload }: {
  novelId: number; chapters: Chapter[]; producers: Producer[]; reload: () => void
}) {
  const [filter, setFilter] = useState<Filter>('all')
  const [hideNonStory, setHideNonStory] = useState(true)
  const [newestFirst, setNewestFirst] = useState(false)
  // selection survives actions and a visit to a chapter page (per novel, for this session)
  const selKey = `chapterSelection:${novelId}`
  const [sel, setSel] = useState<Set<number>>(() => {
    try { return new Set(JSON.parse(sessionStorage.getItem(selKey) ?? '[]') as number[]) } catch { return new Set() }
  })
  useEffect(() => {
    try { sessionStorage.setItem(selKey, JSON.stringify([...sel])) } catch { /* storage unavailable */ }
  }, [sel, selKey])
  const [producer, setProducer] = useState('')
  const [page, setPage] = useState(0)
  const [pageSize, setPageSize] = useState<number>(() => {
    try { return Number(localStorage.getItem('chaptersPerPage') ?? 50) } catch { return 50 }
  })
  const [playing, setPlaying] = useState<Chapter | null>(null)
  const [msg, setMsg] = useState('')

  const filtered = useMemo(() => {
    const f = chapters.filter((c) => (!hideNonStory || c.story) && matches(c, filter))
    return newestFirst ? f.reverse() : f
  }, [chapters, filter, hideNonStory, newestFirst])
  const per = pageSize || Math.max(1, filtered.length)
  const pages = Math.max(1, Math.ceil(filtered.length / per))
  useEffect(() => { setPage(0) }, [filter, hideNonStory, pageSize, newestFirst])
  useEffect(() => { if (page >= pages) setPage(pages - 1) }, [page, pages])
  const shown = filtered.slice(page * per, (page + 1) * per)

  const toggle = (cid: number) => setSel((s) => { const n = new Set(s); n.has(cid) ? n.delete(cid) : n.add(cid); return n })
  const allShown = shown.length > 0 && shown.every((c) => sel.has(c.id))
  const toggleShown = () => setSel((s) => {
    const n = new Set(s)
    shown.forEach((c) => (allShown ? n.delete(c.id) : n.add(c.id)))
    return n
  })

  async function run(steps: Step[]) {
    const r = await api.process(novelId, [...sel], steps, producer)
    setMsg(`${r.queued} step${r.queued === 1 ? '' : 's'} queued`)
    reload()
  }

  return (
    <>
      <div className="toolbar">
        <div className="seg">
          {FILTERS.map(([f, t]) => <button key={f} className={filter === f ? 'on' : ''} onClick={() => setFilter(f)}>{t}</button>)}
        </div>
        <label className="small"><input type="checkbox" checked={hideNonStory} onChange={(e) => setHideNonStory(e.target.checked)} /> hide announcements</label>
        <button className="ghost small-btn" onClick={() => setNewestFirst(!newestFirst)}>{newestFirst ? 'newest first' : 'oldest first'}</button>
      </div>
      <div className="toolbar run-bar">
        <span className="small muted">{sel.size ? `${sel.size} selected` : 'Select chapters, then run:'}</span>
        <button disabled={!sel.size} onClick={() => run(['analyze'])}>1 · Analyze</button>
        <button disabled={!sel.size} onClick={() => run(['script'])}>2 · Script</button>
        <button disabled={!sel.size} onClick={() => run(['narrate'])}>3 · Narrate</button>
        <button disabled={!sel.size} onClick={() => run(['analyze', 'script', 'narrate'])}>All steps</button>
        <label className="small">script by <ProducerSelect producers={producers} value={producer} onChange={setProducer} allowDefault /></label>
        {sel.size > 0 && <button className="ghost small-btn" onClick={() => setSel(new Set())}>clear</button>}
        {msg && <span className="note">{msg}</span>}
      </div>
      {playing && (
        <div className="player">
          <span>{playing.position}. {playing.title}</span>
          <audio controls autoPlay src={audioUrl(playing.id, playing.narrated_at ?? 0)} />
          <button className="ghost small-btn" onClick={() => setPlaying(null)}>×</button>
        </div>
      )}
      <table className="chapters">
        <thead>
          <tr>
            <th><input type="checkbox" checked={allShown} onChange={toggleShown} title="select this page" /></th>
            <th>#</th><th>Title</th><th>Steps</th><th>Published</th><th></th>
          </tr>
        </thead>
        <tbody>
          {shown.map((c) => (
            <tr key={c.id}>
              <td><input type="checkbox" checked={sel.has(c.id)} onChange={() => toggle(c.id)} /></td>
              <td className="num">{c.position}</td>
              <td>
                <a href={`#/chapter/${c.id}`} className="ctitle">{c.title}</a>
                {!c.story && <span className="tag">not a chapter?</span>}
                {c.quality_issues && <span className="tag warn" title={c.quality_issues}>quality issues</span>}
                {c.error && <div className="error small">{c.error}</div>}
              </td>
              <td><StepBadges c={c} /></td>
              <td className="date">{fmtDate(c.published)}</td>
              <td>{c.narrated_at && <button className="ghost small-btn" onClick={() => setPlaying(c)} title="play">▶</button>}</td>
            </tr>
          ))}
          {shown.length === 0 && <tr><td colSpan={6} className="muted">No chapters match this filter.</td></tr>}
        </tbody>
      </table>
      <div className="pager">
        {pages > 1 && <>
          <button className="ghost small-btn" disabled={page === 0} onClick={() => setPage(0)}>«</button>
          <button className="ghost small-btn" disabled={page === 0} onClick={() => setPage((p) => p - 1)}>‹</button>
          <span className="small">page {page + 1} of {pages}</span>
          <button className="ghost small-btn" disabled={page >= pages - 1} onClick={() => setPage((p) => p + 1)}>›</button>
          <button className="ghost small-btn" disabled={page >= pages - 1} onClick={() => setPage(pages - 1)}>»</button>
        </>}
        <span className="small muted">{filtered.length} chapters ·</span>
        <label className="small">per page{' '}
          <select value={pageSize} onChange={(e) => {
            const n = Number(e.target.value)
            setPageSize(n)
            try { localStorage.setItem('chaptersPerPage', String(n)) } catch { /* storage unavailable */ }
          }}>
            {PAGE_SIZES.map((n) => <option key={n} value={n}>{n || 'all'}</option>)}
          </select>
        </label>
      </div>
    </>
  )
}

function CastTab({ novel, setNovel }: { novel: Novel; setNovel: (n: Novel) => void }) {
  const [cast, setCast] = useState<Character[]>([])
  const [voices, setVoices] = useState<Voice[]>([])
  const [msg, setMsg] = useState('')
  const [narratorPitch, setNarratorPitch] = useState<Pitch>('all')
  useEffect(() => {
    api.cast(novel.id).then(setCast)
    api.voices().then((v) => setVoices(v.filter((x) => x.usable)))
  }, [novel.id])

  async function edit(c: Character, p: Parameters<typeof api.patchCharacter>[2]) {
    const before = c.voice
    const updated = await api.patchCharacter(novel.id, c.name, p)
    setCast(updated)
    const after = updated.find((x) => x.name === c.name)?.voice
    setMsg(after && after !== before && !p.voice ? `${c.name}: voice changed to match` : '')
  }

  const narratorVoice = voices.find((v) => v.id === novel.narrator)
  const narratorGender = narratorVoice?.gender ?? 'male'
  async function setNarratorGender(g: 'male' | 'female') {
    const pick = voices.filter((v) => v.gender === g).sort((a, b) => a.f0 - b.f0)   // middle pitch of that gender
    const choice = pick[Math.floor(pick.length / 2)]
    if (choice) setNovel(await api.patchNovel(novel.id, { narrator: choice.id }))
  }
  const narratorOptions = voices
    .filter((v) => v.id === novel.narrator || (v.gender === narratorGender && (narratorPitch === 'all' || v.band === narratorPitch)))
    .sort((a, b) => a.label.localeCompare(b.label))

  return (
    <>
      {msg && <p className="note">{msg}</p>}
      <table className="cast-table">
        <thead><tr><th>Character</th><th>Gender</th><th>Pitch</th><th>Voice</th><th></th></tr></thead>
        <tbody>
          <tr className="narrator-row">
            <td><b>Narrator</b><div className="small muted">reads everything outside dialogue</div></td>
            <td><div className="seg">
              {(['female', 'male'] as const).map((g) =>
                <button key={g} className={narratorGender === g ? 'on' : ''} disabled={narratorGender === g}
                  onClick={() => setNarratorGender(g)}>{g === 'female' ? 'F' : 'M'}</button>)}
            </div></td>
            <td><select value={narratorPitch} onChange={(e) => setNarratorPitch(e.target.value as Pitch)}>
              {PITCHES.map(([p, t]) => <option key={p} value={p}>{t}</option>)}
            </select></td>
            <td><select value={novel.narrator} onChange={(e) => api.patchNovel(novel.id, { narrator: e.target.value }).then(setNovel)}>
              {!narratorVoice && <option value={novel.narrator}>{novel.narrator || '(auto)'}</option>}
              {narratorOptions.map((v) => <option key={v.id} value={v.id}>{v.label}</option>)}
            </select></td>
            <td>{novel.narrator && <audio controls preload="none" src={sampleUrl(novel.narrator)} className="mini" />}</td>
          </tr>
          {cast.map((c) => (
            <tr key={c.name}>
              <td><b>{c.name}</b>{c.aliases.length > 0 && <div className="small muted">aka {c.aliases.join(', ')}</div>}
                {c.voice_hint && <div className="small muted" title="producer's description">{c.voice_hint}</div>}</td>
              <VoiceCells c={c} voices={voices} onEdit={(p) => edit(c, p)} />
            </tr>
          ))}
        </tbody>
      </table>
      {cast.length === 0 && <p className="muted small">Characters appear here when chapter scripts are produced; each new one gets a voice automatically.</p>}
    </>
  )
}
