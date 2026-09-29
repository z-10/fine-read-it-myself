import { useEffect, useState } from 'react'
import { sampleUrl, STEP_LABEL, stepState, type Chapter, type Character, type Gender, type Pitch, type Producer, type Step, type Voice } from './api'

const STATE_TEXT = { none: '—', done: 'done', outdated: 'outdated', queued: 'queued', running: 'running' } as const

// one badge per step: Analysis / Script / Audio
export function StepBadges({ c }: { c: Chapter }) {
  return (
    <span className="steps">
      {(['analyze', 'script', 'narrate'] as Step[]).map((s) => {
        const st = stepState(c, s)
        const job = c.jobs.find((j) => j.kind === s)
        const title = `${STEP_LABEL[s]}: ${STATE_TEXT[st]}${job?.stage ? ` (${job.stage})` : ''}`
        return (
          <span key={s} className={`step ${st}`} title={title}>
            {STEP_LABEL[s]}
            {st === 'running' && job ? ` ${Math.round(job.progress * 100)}%` : ''}
          </span>
        )
      })}
      {c.to_check > 0 && <span className="step check" title="disputed speakers to check in the script">{c.to_check} to check</span>}
    </span>
  )
}

// A producer menu; an empty value starts on the default producer (Settings) and reports it through onChange.
export function ProducerSelect({ producers, value, onChange }: {
  producers: Producer[]; value: string; onChange: (v: string) => void
}) {
  const dflt = producers.find((p) => p.default)?.name ?? producers[0]?.name ?? ''
  useEffect(() => { if (!value && dflt) onChange(dflt) }, [value, dflt])
  return (
    <select value={value || dflt} onChange={(e) => onChange(e.target.value)}>
      {value && !producers.some((p) => p.name === value) &&   // saved, but its model was removed since
        <option value={value} disabled>{value} (not available)</option>}
      {producers.map((p) => <option key={p.name} value={p.name}>{p.label}{p.default ? ' · default' : ''}</option>)}
    </select>
  )
}

export const fmtTime = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
export const fmtDate = (unix: number | null) => (unix ? new Date(unix * 1000).toLocaleDateString() : '')
export const ago = (unix: number | null) => {
  if (!unix) return 'never'
  const s = Date.now() / 1000 - unix
  if (s < 90) return 'just now'
  if (s < 5400) return `${Math.round(s / 60)} min ago`
  if (s < 129600) return `${Math.round(s / 3600)} h ago`
  return `${Math.round(s / 86400)} days ago`
}

export const PITCHES = [['all', 'all pitches'], ['low', 'low pitch'], ['mid', 'medium pitch'], ['high', 'high pitch']] as const

// A character's voice, as table cells: gender (F/M), pitch filter, voice from the pool (reader name, gender,
// pitch band; only voices matching gender + pitch are offered), and the voice's sample line.
export function VoiceCells({ c, voices, onEdit }: {
  c: Pick<Character, 'gender' | 'pitch' | 'voice' | 'locked'>; voices: Voice[]
  onEdit: (p: { gender?: Gender; pitch?: Pitch; voice?: string }) => void
}) {
  const label = (vid: string) => voices.find((v) => v.id === vid)?.label ?? vid
  return (
    <>
      <td><div className="seg">
        {(['female', 'male'] as const).map((g) =>
          <button key={g} className={c.gender === g ? 'on' : ''} disabled={c.gender === g}
            onClick={() => onEdit({ gender: g })}>{g === 'female' ? 'F' : 'M'}</button>)}
      </div></td>
      <td><select value={c.pitch} onChange={(e) => onEdit({ pitch: e.target.value as Pitch })}>
        {PITCHES.map(([p, t]) => <option key={p} value={p}>{t}</option>)}
      </select></td>
      <td>
        <select value={c.voice} onChange={(e) => onEdit({ voice: e.target.value })}>
          {!voices.some((v) => v.id === c.voice) && <option value={c.voice}>{c.voice ? label(c.voice) : '(not assigned yet)'}</option>}
          {voices.filter((v) => v.id === c.voice || (v.gender === c.gender && (c.pitch === 'all' || v.band === c.pitch)))
            .map((v) => <option key={v.id} value={v.id}>{v.label}</option>)}
        </select>
        {c.locked ? <span className="tag" title="set by you; never changed automatically">locked</span> : null}
      </td>
      <td>{c.voice && <audio controls preload="none" src={sampleUrl(c.voice)} className="mini" />}</td>
    </>
  )
}

// Overview ruler for things to review (as in a diff viewer's scrollbar): a mark per element matching `selector`
// at its place in the page (data-level="check" | "likely" sets the colour), click to jump; plus a prev/next
// control (F8 / Shift+F8, Alt+Down / Alt+Up). `version` changes when the set of marked elements may have changed.
export function ReviewRuler({ selector, version }: { selector: string; version: unknown }) {
  type Mark = { top: number; height: number; level: string }
  const [marks, setMarks] = useState<Mark[]>([])
  const [at, setAt] = useState(0)   // marks above the middle of the window
  const [viewH, setViewH] = useState(innerHeight)
  const items = () => [...document.querySelectorAll<HTMLElement>(selector)]
  const measure = () => {
    const H = Math.max(1, document.documentElement.scrollHeight)
    setMarks(items().map((e) => {
      const r = e.getBoundingClientRect()
      return { top: ((r.top + scrollY) / H) * 100, height: Math.max(0.35, (r.height / H) * 100), level: e.dataset.level ?? 'check' }
    }))
    // the strip spans exactly what is visible (phones: the browser bars come and go)
    setViewH(window.visualViewport?.height ?? innerHeight)
    const mid = scrollY + innerHeight / 2
    setAt(items().filter((e) => e.getBoundingClientRect().top + scrollY + 1 < mid).length)
  }
  useEffect(() => {
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(document.body)
    addEventListener('scroll', measure, { passive: true })
    addEventListener('resize', measure)
    window.visualViewport?.addEventListener('resize', measure)
    return () => {
      ro.disconnect(); removeEventListener('scroll', measure); removeEventListener('resize', measure)
      window.visualViewport?.removeEventListener('resize', measure)
    }
  }, [version])
  const show = (e: HTMLElement | undefined) => {
    if (!e) return
    e.scrollIntoView({ block: 'center', behavior: 'smooth' })
    e.classList.remove('flash'); void e.offsetWidth; e.classList.add('flash')
  }
  const step = (dir: 1 | -1) => {
    const mid = scrollY + innerHeight / 2
    const list = items()
    const y = (e: HTMLElement) => e.getBoundingClientRect().top + scrollY + e.offsetHeight / 2
    show(dir > 0 ? list.find((e) => y(e) > mid + 2) ?? list[0] : [...list].reverse().find((e) => y(e) < mid - 2) ?? list[list.length - 1])
  }
  useEffect(() => {
    const key = (ev: KeyboardEvent) => {
      if (ev.key === 'F8' || (ev.altKey && (ev.key === 'ArrowDown' || ev.key === 'ArrowUp'))) {
        ev.preventDefault()
        step(ev.shiftKey || ev.key === 'ArrowUp' ? -1 : 1)
      }
    }
    addEventListener('keydown', key)
    return () => removeEventListener('keydown', key)
  }, [version])
  if (!marks.length) return null
  return (
    <>
      <div className="ruler" aria-hidden style={{ height: viewH }}>
        {marks.map((m, i) => (
          <button key={i} className={`mark ${m.level}`} style={{ top: `${m.top}%`, height: `${m.height}%` }}
            onClick={() => show(items()[i])} title={`${i + 1} of ${marks.length}`} />
        ))}
      </div>
      <div className="review-nav">
        <button className="ghost small-btn" title="previous (Shift+F8, Alt+Up)" onClick={() => step(-1)}>▲</button>
        <span className="small">{Math.min(at + 1, marks.length)} / {marks.length}</span>
        <button className="ghost small-btn" title="next (F8, Alt+Down)" onClick={() => step(1)}>▼</button>
      </div>
    </>
  )
}
