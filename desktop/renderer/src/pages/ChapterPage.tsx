import { useEffect, useMemo, useRef, useState } from 'react'
import {
  api, audioUrl, lineAudioUrl, stepState, STEP_LABEL,
  type Analysis, type Span, type CastEntry, type Chapter, type Character, type Line, type Narration, type Producer, type Dispute, type Script, type Step, type Voice,
} from '../api'
import { ago, fmtTime, ProducerSelect, ReviewRuler, VoiceCells } from '../components'

type Tab = 'analysis' | 'script' | 'narration'
const SPECIAL = ['Narrator', 'Unknown', 'System']

export default function ChapterPage({ id, initialTab }: { id: number; initialTab?: string }) {
  const [ch, setCh] = useState<Chapter | null>(null)
  const [producers, setProducers] = useState<Producer[]>([])
  const [producer, setProducer] = useState('')
  const [tab, setTab] = useState<Tab>('analysis')
  const [error, setError] = useState('')
  const [dirty, setDirty] = useState(false)   // unsaved edits in the open editor
  const [version, setVersion] = useState(0)   // bump to reload artifacts after a step finished
  const prevJobs = useRef(0)

  const load = () => api.chapter(id).then((c) => {
    if (prevJobs.current && !c.jobs.length) setVersion((v) => v + 1)
    prevJobs.current = c.jobs.length
    setCh(c)
  }).catch((e) => setError(e.message))
  useEffect(() => {
    load()
    api.producers().then(setProducers)
  }, [id])
  const busy = !!ch?.jobs.length
  useEffect(() => {
    if (!busy) return
    const t = setInterval(load, 1500)
    return () => clearInterval(t)
  }, [busy])
  useEffect(() => {   // open the most advanced step that exists
    if (!ch) return
    if (initialTab === 'analysis' || initialTab === 'script' || initialTab === 'narration') setTab(initialTab)
    else setTab(ch.narrated_at ? 'narration' : ch.scripted_at ? 'script' : 'analysis')
  }, [ch?.id])

  async function run(steps: Step[]) {
    if (dirty && !confirm('You have unsaved edits. Run anyway (they stay in the editor)?')) return
    try {
      await api.run(id, steps, steps.includes('script') ? producer : '')
      await load()
    } catch (err) {
      setError((err as Error).message)
    }
  }

  if (error && !ch) return <p className="error">{error}</p>
  if (!ch) return <p className="hint">Loading…</p>
  const st = (s: Step) => stepState(ch, s)
  const job = (s: Step) => ch.jobs.find((j) => j.kind === s)
  const stepCard = (s: Step, n: number, when: number | null, extra: React.ReactNode, enabled: boolean) => (
    <div className={`step-card ${st(s)}`}>
      <div className="step-head"><span className="step-n">{n}</span> {STEP_LABEL[s]}
        <span className={`step ${st(s)}`}>{st(s) === 'none' ? 'not yet' : st(s)}</span></div>
      <div className="small muted">
        {job(s) ? `${job(s)!.stage || job(s)!.state} ${job(s)!.state === 'running' ? Math.round(job(s)!.progress * 100) + '%' : ''}`
          : when ? ago(when) : '—'}
      </div>
      <div className="step-actions">
        {extra}
        <button disabled={!enabled || !!job(s)} onClick={() => run([s])}>{when ? 'Redo' : 'Run'}</button>
      </div>
    </div>
  )

  return (
    <>
      <a href={`#/novel/${ch.novel_id}`} className="back">← Novel</a>
      <h1 className="chapter-title">{ch.position}. {ch.title}</h1>
      <div className="meta"><a href={ch.url} target="_blank" rel="noreferrer">open on site</a>
        {ch.script_producer && <> · script by {producers.find((p) => p.name === ch.script_producer)?.label ?? ch.script_producer}</>}</div>
      {ch.error && <p className="error">{ch.error}</p>}
      {error && <p className="error">{error}</p>}

      <div className="step-cards">
        {stepCard('analyze', 1, ch.analyzed_at, null, true)}
        {stepCard('script', 2, ch.scripted_at,
          <ProducerSelect producers={producers} value={producer} onChange={setProducer} />, !!ch.analyzed_at || !!job('analyze'))}
        {stepCard('narrate', 3, ch.narrated_at, null, !!ch.scripted_at || !!job('script'))}
        <div className="step-card all">
          <div className="step-head">All steps</div>
          <div className="small muted">analyze → script → narrate</div>
          <div className="step-actions"><button disabled={busy} onClick={() => run(['analyze', 'script', 'narrate'])}>Run all</button></div>
        </div>
      </div>

      <div className="tabs">
        {(['analysis', 'script', 'narration'] as Tab[]).map((t) => (
          <button key={t} className={tab === t ? 'on' : ''}
            onClick={() => { if (!dirty || confirm('Discard unsaved edits?')) { setDirty(false); setTab(t) } }}>
            {t === 'analysis' ? '1 · Analysis' : t === 'script' ? '2 · Script' : '3 · Narration'}
          </button>
        ))}
      </div>
      {tab === 'analysis' && <AnalysisEditor key={version} id={id} novelId={ch.novel_id} setDirty={setDirty} onSaved={load} />}
      {tab === 'script' && <ScriptEditor key={version} id={id} novelId={ch.novel_id} narrated={ch.narrated_at} setDirty={setDirty} onSaved={load} />}
      {tab === 'narration' && <NarrationView key={version} ch={ch} />}
    </>
  )
}

// ---------------------------------------------------------------- 1. analysis

function AnalysisEditor({ id, novelId, setDirty, onSaved }: {
  id: number; novelId: number; setDirty: (d: boolean) => void; onSaved: () => void
}) {
  const [a, setA] = useState<Analysis | null>(null)
  const [orig, setOrig] = useState('')
  const [cast, setCast] = useState<Character[]>([])
  const [editing, setEditing] = useState<number | null>(null)
  const [msg, setMsg] = useState('')
  const [missing, setMissing] = useState(false)
  const [pick, setPick] = useState<{ sid: number; start: number; end: number } | null>(null)   // text selected in narration

  useEffect(() => {
    api.analysis(id).then((x) => { setA(x); setOrig(JSON.stringify(x)) }).catch(() => setMissing(true))
    api.cast(novelId).then(setCast)
  }, [id])
  const dirty = !!a && JSON.stringify(a) !== orig
  useEffect(() => setDirty(dirty), [dirty])

  if (missing) return <p className="empty">Not analyzed yet. Run step 1.</p>
  if (!a) return <p className="hint">Loading…</p>
  const names = [...new Set([...SPECIAL.slice(1), ...a.characters.map((c) => c.name), ...cast.map((c) => c.name)])]
  const setSpan = (sid: number, p: Partial<Analysis['spans'][number]>) =>
    setA({ ...a, spans: a.spans.map((s) => (s.id === sid ? { ...s, ...p } : s)) })
  // "Not a quote": the quote goes back into the narration around it (same paragraph), quote marks kept
  const absorb = (sid: number) => {
    const i = a.spans.findIndex((x) => x.id === sid)
    const q = a.spans[i]
    const prev = i > 0 && a.spans[i - 1].para === q.para && a.spans[i - 1].kind === 'narration' ? a.spans[i - 1] : null
    const next = i + 1 < a.spans.length && a.spans[i + 1].para === q.para && a.spans[i + 1].kind === 'narration' ? a.spans[i + 1] : null
    const text = [prev?.text, `“${q.text}”`, next?.text].filter(Boolean).join(' ')
    const merged: Span = { id: prev?.id ?? q.id, para: q.para, kind: 'narration', text, by: 'user' }
    const from = prev ? i - 1 : i, to = next ? i + 1 : i
    setA({ ...a, spans: [...a.spans.slice(0, from), merged, ...a.spans.slice(to + 1)] })
    setPick(null)
  }
  // "Make quote": the selected part of a narration span becomes a quote (speaker to set by the user)
  const makeQuote = (sid: number, start: number, end: number) => {
    const i = a.spans.findIndex((x) => x.id === sid)
    const n = a.spans[i]
    const marks = /^[\s“”"]+|[\s“”"]+$/g
    const before = n.text.slice(0, start).replace(/[\s“"]+$/, '').trim()
    const inner = n.text.slice(start, end).replace(marks, '')
    const after = n.text.slice(end).replace(/^[\s”"]+/, '').trim()
    if (!inner) return
    let next = Math.max(...a.spans.map((x) => x.id)) + 1
    const parts: Span[] = []
    if (before) parts.push({ ...n, id: n.id, text: before, by: 'user' })
    parts.push({ id: before ? next++ : n.id, para: n.para, kind: 'dialogue', text: inner, speaker: 'Unknown', by: 'user' })
    if (after) parts.push({ ...n, id: next++, text: after, by: 'user' })
    setA({ ...a, spans: [...a.spans.slice(0, i), ...parts, ...a.spans.slice(i + 1)] })
    setPick(null)
  }
  const byPara = new Map<number, Analysis['spans']>()
  a.spans.forEach((s) => byPara.set(s.para, [...(byPara.get(s.para) ?? []), s]))
  const dialogue = a.spans.filter((s) => s.kind === 'dialogue')
  const unknown = dialogue.filter((s) => !s.speaker || s.speaker === 'Unknown').length
  const flagged = dialogue.filter((s) => s.check).length

  async function save() {
    try {
      const saved = await api.saveAnalysis(id, a!)
      setA(saved)
      setOrig(JSON.stringify(saved))
      setMsg('Saved. The script is now outdated; produce it again to use these changes.')
      onSaved()
    } catch (err) {
      setMsg((err as Error).message)
    }
  }

  return (
    <>
      <div className="editor-bar">
        <span className="small muted">
          {a.detector === 'modernbooknlp' ? 'Speakers detected by ModernBookNLP' : 'Made without speaker detection: run step 1 again'}
          {' · '}{dialogue.length} dialogue lines{unknown ? `, ${unknown} without a speaker` : ''}
          {flagged ? `, ${flagged} uncertain` : ''}
          {' · the producer double-checks every quote; speakers you set here are final'}
        </span>
        <span className="spacer" />
        {msg && <span className="note">{msg}</span>}
        <button className="ghost" disabled={!dirty} onClick={() => setA(JSON.parse(orig))}>Discard</button>
        <button disabled={!dirty} onClick={save}>Save</button>
      </div>
      {a.characters.length > 0 && (
        <p className="small muted">Characters: {a.characters.map((c) => `${c.name} (${c.gender === 'female' ? 'F' : 'M'})`).join(', ')}</p>
      )}
      <ReviewRuler selector=".dl.to-check" version={a.spans.filter((x) => x.check).map((x) => x.id).join(',')} />
      <div className="doc">
        {[...byPara.entries()].map(([p, spans]) => (
          <div key={p} className="para">
            {spans.map((s) => s.kind === 'dialogue' ? (
              <div key={s.id} className={`dl${s.check ? ' to-check' : ''}`} data-level={s.check ? 'check' : undefined}>
                <SpeakerPick names={names} value={s.speaker ?? 'Unknown'} title={s.check ? `To check: ${s.check}` : undefined}
                  className={`speaker ${!s.speaker || s.speaker === 'Unknown' ? 'unknown' : ''} ${s.check ? 'check' : ''}`}
                  onChange={(v) => setSpan(s.id, { speaker: v, check: undefined, by: 'user' })} />
                <SpanText value={s.text} editing={editing === s.id} onEdit={() => setEditing(s.id)}
                  onChange={(t) => setSpan(s.id, { text: t, by: 'user' })} onDone={() => setEditing(null)} quote />
                <button className="ghost tiny" title="Not speech: put it back into the narration" onClick={() => absorb(s.id)}>not a quote</button>
              </div>
            ) : (
              <span key={s.id}>
                <SpanText value={s.text} editing={editing === s.id} onEdit={() => setEditing(s.id)}
                  onChange={(t) => setSpan(s.id, { text: t, by: 'user' })} onDone={() => setEditing(null)}
                  onSelect={(start, end) => setPick(end > start ? { sid: s.id, start, end } : null)} />
                {pick?.sid === s.id && (
                  <button className="tiny" onMouseDown={(e) => e.preventDefault()}
                    onClick={() => makeQuote(s.id, pick.start, pick.end)}>make quote</button>
                )}
              </span>
            ))}
          </div>
        ))}
      </div>
    </>
  )
}

// speaker dropdown (every known name) with "new name…" for a speaker not in the list yet
function SpeakerPick({ names, value, onChange, className, title }: {
  names: string[]; value: string; onChange: (v: string) => void; className: string; title?: string
}) {
  const [typing, setTyping] = useState(false)
  const [draft, setDraft] = useState('')
  if (typing) {
    const done = () => { if (draft.trim()) onChange(draft.trim()); setTyping(false); setDraft('') }
    return <input className={className} autoFocus value={draft} placeholder="new name"
      onChange={(e) => setDraft(e.target.value)} onBlur={done}
      onKeyDown={(e) => { if (e.key === 'Enter') done(); if (e.key === 'Escape') { setTyping(false); setDraft('') } }} />
  }
  const options = [...new Set([value, ...names])].sort((x, y) => x.localeCompare(y))
  return (
    <select className={className} title={title} value={value}
      onChange={(e) => (e.target.value === '__new_name__' ? setTyping(true) : onChange(e.target.value))}>
      {options.map((n) => <option key={n} value={n}>{n}</option>)}
      <option value={'__new_name__'}>new name…</option>
    </select>
  )
}

function SpanText({ value, editing, onEdit, onChange, onDone, onSelect, quote }: {
  value: string; editing: boolean; onEdit: () => void; onChange: (t: string) => void; onDone: () => void
  onSelect?: (start: number, end: number) => void   // narration: text selected with the mouse (offsets in value)
  quote?: boolean
}) {
  if (editing)
    return <textarea className="span-edit" autoFocus value={value} rows={Math.max(2, Math.ceil(value.length / 90))}
      onChange={(e) => onChange(e.target.value)} onBlur={onDone} />
  const selection = (el: HTMLElement): [number, number] | null => {
    const sel = window.getSelection()
    if (!sel || sel.isCollapsed || sel.rangeCount === 0) return null
    const r = sel.getRangeAt(0)
    if (r.startContainer !== r.endContainer || !el.contains(r.startContainer)) return null
    return [Math.min(r.startOffset, value.length), Math.min(r.endOffset, value.length)]
  }
  return (
    <span className={quote ? 'span q' : 'span'} title={onSelect ? 'click to edit, select words to mark them as a quote' : 'click to edit'}
      onMouseUp={(e) => { if (onSelect) { const r = selection(e.currentTarget); onSelect(r ? r[0] : 0, r ? r[1] : 0) } }}
      onClick={(e) => { if (!selection(e.currentTarget)) onEdit() }}>
      {quote ? `“${value}”` : value}{' '}
    </span>
  )
}

// ---------------------------------------------------------------- 2. script

function ScriptEditor({ id, novelId, narrated, setDirty, onSaved }: {
  id: number; novelId: number; narrated: number | null; setDirty: (d: boolean) => void; onSaved: () => void
}) {
  const [s, setS] = useState<Script | null>(null)
  const [orig, setOrig] = useState('')
  const [novelCast, setNovelCast] = useState<Character[]>([])
  const [voices, setVoices] = useState<Voice[]>([])
  const [uncertainOnly, setUncertainOnly] = useState(false)
  const [dialogueOnly, setDialogueOnly] = useState(false)
  const [msg, setMsg] = useState('')
  const [missing, setMissing] = useState(false)
  const [newName, setNewName] = useState('')

  useEffect(() => {
    api.script(id).then((x) => { setS(x); setOrig(JSON.stringify(x)) }).catch(() => setMissing(true))
    api.cast(novelId).then(setNovelCast)
    api.voices().then((v) => setVoices(v.filter((x) => x.usable)))
  }, [id])
  const dirty = !!s && JSON.stringify(s) !== orig
  useEffect(() => setDirty(dirty), [dirty])
  const origLines = useMemo(() => (orig ? (JSON.parse(orig) as Script).lines : []), [orig])

  if (missing) return <p className="empty">No script yet. Run step 2.</p>
  if (!s) return <p className="hint">Loading…</p>
  const speakers = [...new Set([...SPECIAL, ...Object.keys(s.cast), ...novelCast.map((c) => c.name)])]
  const setLine = (i: number, p: Partial<Line>) => setS({ ...s, lines: s.lines.map((l, j) => (j === i ? { ...l, ...p } : l)) })
  const setCastEntry = (name: string, p: Partial<CastEntry>) => setS({ ...s, cast: { ...s.cast, [name]: { ...s.cast[name], ...p } } })
  const used = new Set(s.lines.map((l) => l.speaker))
  // disputed lines (script line n+1 = analysis span n) still waiting for the user
  const disputes = new Map<number, Dispute>((s.review ?? []).map((r) => [r.id + 1, r]))
  const origSpeaker = (i: number) => origLines[i]?.speaker
  const open = (i: number) => {
    const r = disputes.get(i)
    return r && !r.resolved && !s.lines[i].confirmed && s.lines[i].speaker === origSpeaker(i) ? r : undefined
  }
  const openCount = { check: 0, likely: 0 }
  s.lines.forEach((_, i) => { const r = open(i); if (r) openCount[r.level ?? 'check']++ })
  // pick a speaker for a disputed line: a stranger's description joins the chapter cast
  function choose(i: number, who: string) {
    const cast = !SPECIAL.includes(who) && !s!.cast[who] && !novelCast.some((c) => c.name === who)
      ? { ...s!.cast, [who]: { gender: 'male' as const, age: 'adult' as const, voice: '', aliases: [] } } : s!.cast
    setS({ ...s!, cast, lines: s!.lines.map((l, j) => (j === i ? { ...l, speaker: who, confirmed: true } : l)) })
  }


  function addCharacter() {
    const n = newName.trim()
    if (!n || s!.cast[n] || SPECIAL.includes(n)) return
    const known = novelCast.find((c) => c.name === n)
    setS({ ...s!, cast: { ...s!.cast, [n]: { gender: known?.gender ?? 'male', age: known?.age ?? 'adult', voice: '', aliases: [] } } })
    setNewName('')
  }
  function removeCharacter(n: string) {
    const { [n]: _gone, ...rest } = s!.cast
    setS({ ...s!, cast: rest })
  }
  async function save() {
    try {
      const saved = await api.saveScript(id, s!)
      setS(saved)
      setOrig(JSON.stringify(saved))
      setMsg(narrated ? 'Saved. Narrate again to hear the changes (unchanged lines are reused).' : 'Saved.')
      api.cast(novelId).then(setNovelCast)   // new characters got their voices
      onSaved()
    } catch (err) {
      setMsg((err as Error).message)
    }
  }

  return (
    <>
      <div className="editor-bar">
        <span className="small muted">{s.lines.length} lines · by {s.producer}</span>
        <label className="small"><input type="checkbox" checked={dialogueOnly} onChange={(e) => setDialogueOnly(e.target.checked)} /> dialogue only</label>
        {openCount.check + openCount.likely > 0 && <>
          <span className="small">
            {openCount.check > 0 && <span className="step check">{openCount.check} to check</span>}
            {openCount.likely > 0 && <span className="step likely">{openCount.likely} probably fine</span>}
          </span>
          <label className="small"><input type="checkbox" checked={uncertainOnly} onChange={(e) => setUncertainOnly(e.target.checked)} /> only these</label>
        </>}
        <span className="spacer" />
        {msg && <span className="note">{msg}</span>}
        <button className="ghost" disabled={!dirty} onClick={() => setS(JSON.parse(orig))}>Discard</button>
        <button disabled={!dirty} onClick={save}>Save</button>
      </div>
      {s.quality_issues.length > 0 && <div className="issues">{s.quality_issues.map((q) => <div key={q}>⚠ {q}</div>)}</div>}
      {!!s.review?.length && (
        <details className="review">
          {/* script line n+1 is analysis span n (line 0 is the title) */}
          <summary>Speaker check: {s.review.length} disputed {s.review.length === 1 ? 'quote' : 'quotes'},{' '}
            {s.review.filter((r) => r.to !== r.from).length} changed</summary>
          {s.review.map((r) => (
            <div key={r.id} className="small">
              {r.to !== r.from ? <b>{r.from} → {r.to}</b> : <span>kept <b>{r.from}</b></span>}
              <span className="muted"> (step 1: {r.from}, second reading: {r.blind})</span>
              {' '}“{(s.lines[r.id + 1]?.text ?? '').slice(0, 80)}” <span className="muted">{r.evidence}</span>
            </div>
          ))}
        </details>
      )}

      <details className="cast-section" open={!uncertainOnly}>
        <summary><h3>Chapter cast <span className="small muted">({Object.keys(s.cast).length})</span></h3></summary>
      <table className="cast-table">
        <thead><tr><th>Name</th><th>Gender</th><th>Pitch</th><th>Voice</th><th></th><th>Aliases</th><th></th></tr></thead>
        <tbody>
          {Object.entries(s.cast).map(([name, e]) => {
            const nc = novelCast.find((c) => c.name === name)
            return (
              <tr key={name}>
                <td><b>{name}</b>{nc ? '' : <span className="tag">new</span>}
                  <input className="hint-edit" placeholder="voice description"
                    value={voices.some((v) => v.id === e.voice) ? '' : e.voice /* older scripts stored the voice id here */}
                    title="the producer's description of this voice" onChange={(ev) => setCastEntry(name, { voice: ev.target.value })} /></td>
                {nc
                  ? <VoiceCells c={nc} voices={voices} onEdit={async (p) => {
                      setNovelCast(await api.patchCharacter(novelId, name, p))
                      if (p.gender) setCastEntry(name, { gender: p.gender })
                    }} />
                  : <>
                      <td><div className="seg">
                        {(['female', 'male'] as const).map((g) =>
                          <button key={g} className={e.gender === g ? 'on' : ''} disabled={e.gender === g}
                            onClick={() => setCastEntry(name, { gender: g })}>{g === 'female' ? 'F' : 'M'}</button>)}
                      </div></td>
                      <td colSpan={3} className="small muted">a voice is picked when the script is saved or produced</td>
                    </>}
                <td><input value={e.aliases.join(', ')} placeholder="comma-separated"
                  onChange={(ev) => setCastEntry(name, { aliases: ev.target.value.split(',').map((x) => x.trim()).filter(Boolean) })} /></td>
                <td>{!used.has(name) && <button className="ghost small-btn" onClick={() => removeCharacter(name)}>remove</button>}</td>
              </tr>
            )
          })}
          <tr>
            <td colSpan={7}>
              <input className="add-name" value={newName} placeholder="add a character" onChange={(e) => setNewName(e.target.value)}
                onKeyDown={(e) => e.key === 'Enter' && addCharacter()} />{' '}
              <button className="ghost small-btn" onClick={addCharacter} disabled={!newName.trim()}>add</button>
            </td>
          </tr>
        </tbody>
      </table>
      </details>

      <ReviewRuler selector=".line.uncertain"
        version={`${s.lines.map((_, i) => (open(i) ? i : '')).join(',')}|${uncertainOnly}|${dialogueOnly}`} />
      <h3>Lines</h3>
      <div className="lines">
        {s.lines.map((l, i) => {
          const r = open(i)
          // "only these": each disputed line with the two lines before and the one after it, for context
          const near = (j: number) => [j - 1, j, j + 1, j + 2].some((k) => open(k))
          const context = uncertainOnly && !r && near(i)
          if ((dialogueOnly && l.kind !== 'dialogue' && !r) || (uncertainOnly && !r && !context)) return null
          const gap = uncertainOnly && i > 0 && !near(i - 1)   // a new passage starts here
          return (
          <div key={i} className={`line ${l.kind}${r ? ` uncertain ${r.level ?? 'check'}` : ''}${context ? ' context' : ''}${gap ? ' gap' : ''}`}
            data-level={r ? r.level ?? 'check' : undefined}>
            <select className={`speaker ${l.speaker === 'Unknown' ? 'unknown' : ''}`} value={l.speaker}
              onChange={(e) => setLine(i, { speaker: e.target.value })}>
              {speakers.map((n) => <option key={n} value={n}>{n}</option>)}
            </select>
            <textarea value={l.text} rows={Math.max(1, Math.ceil(l.text.length / 95))} onChange={(e) => setLine(i, { text: e.target.value })} />
            {narrated && origLines[i] && origLines[i].text === l.text && origLines[i].speaker === l.speaker
              ? <LinePlay chapter={id} line={i} v={narrated} /> : <span className="play-slot" />}
            {r && (
              <div className="choices" title={r.evidence}>
                <span className="small muted">{r.level === 'likely' ? 'probably fine:' : 'who speaks?'}</span>
                {[...new Set([r.to, r.from, r.blind])].filter((x) => x && x !== 'Unknown').map((who) => (
                  <button key={who} className={`tiny ${who === l.speaker ? 'on' : 'ghost'}`} onClick={() => choose(i, who)}>
                    {who === l.speaker ? `✓ ${who}` : who}</button>
                ))}
              </div>
            )}
          </div>
          )
        })}
      </div>
    </>
  )
}

function LinePlay({ chapter, line, v }: { chapter: number; line: number; v: number }) {
  const ref = useRef<HTMLAudioElement>(null)
  return (
    <span className="play-slot">
      <button className="ghost small-btn" title="play this line" onClick={() => { ref.current!.currentTime = 0; ref.current!.play() }}>▶</button>
      <audio ref={ref} preload="none" src={lineAudioUrl(chapter, line, v)} />
    </span>
  )
}

// ---------------------------------------------------------------- 3. narration

function NarrationView({ ch }: { ch: Chapter }) {
  const [n, setN] = useState<Narration | null>(null)
  const [s, setS] = useState<Script | null>(null)
  const audio = useRef<HTMLAudioElement>(null)
  const [t, setT] = useState(0)
  useEffect(() => {
    if (!ch.narrated_at) return
    api.narration(ch.id).then(setN).catch(() => {})
    api.script(ch.id).then(setS).catch(() => {})
  }, [ch.id, ch.narrated_at])
  if (!ch.narrated_at) return <p className="empty">Not narrated yet. Run step 3.</p>
  const outdated = stepState(ch, 'narrate') === 'outdated'
  return (
    <>
      {outdated && <p className="note">The script changed after this narration; narrate again to update it.</p>}
      <div className="player sticky">
        <audio ref={audio} controls src={audioUrl(ch.id, ch.narrated_at)} onTimeUpdate={(e) => setT(e.currentTarget.currentTime)} />
        <a className="ghost btn" href={audioUrl(ch.id, ch.narrated_at)} download>Download MP3</a>
        {ch.duration_s && <span className="small muted">{fmtTime(ch.duration_s)}</span>}
      </div>
      {n && s && (
        <div className="lines">
          {n.lines.map((l, i) => {
            const line = s.lines[i]
            const active = t >= l.start && t < l.start + l.duration
            return (
              <div key={i} className={`line narr ${active ? 'active' : ''}`}
                onClick={() => { if (audio.current) { audio.current.currentTime = l.start; audio.current.play() } }}>
                <span className="time">{fmtTime(l.start)}</span>
                <span className="who">{line?.speaker}</span>
                <span className="text">{line?.text}</span>
              </div>
            )
          })}
        </div>
      )}
    </>
  )
}
