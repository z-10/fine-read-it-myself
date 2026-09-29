import { useEffect, useState } from 'react'
import { api, type Producer, type ProducerProfile, type Settings, type SetupState, type ShareState } from '../api'

const EMPTY: ProducerProfile = { name: '', base_url: '', api_key: '', model: '', json_schema: true }
const gb = (n: number) => `${(n / 1e9).toFixed(1)} GB`
// MP3 rates offered for speech (48 kHz mono, MPEG-1); the server accepts every MPEG-1 rate
const MP3_RATES = ['64k', '80k', '96k', '112k', '128k', '160k', '192k'] as const

export default function SettingsPage() {
  const [s, setS] = useState<Settings | null>(null)
  const [producers, setProducers] = useState<Producer[]>([])
  const [setup, setSetup] = useState<SetupState | null>(null)
  const [msg, setMsg] = useState('')
  const [error, setError] = useState('')
  const [share, setShare] = useState<ShareState | null>(null)
  const loadShare = () => setTimeout(() => api.share().then(setShare).catch(() => {}), 400)   // after the listener (re)starts
  useEffect(() => { api.share().then(setShare).catch(() => {}) }, [])

  const loadModels = () => {
    api.setup().then(setSetup).catch((e) => setError((e as Error).message))
    api.producers().then(setProducers)
  }
  useEffect(() => {
    api.settings().then(setS)
    loadModels()
  }, [])
  useEffect(() => {   // a model download is running: follow it
    if (!setup?.installing) return
    const t = setInterval(() => api.setup().then((x) => { setSetup(x); if (!x.installing) loadModels() }), 1000)
    return () => clearInterval(t)
  }, [setup?.installing])
  if (!s) return <p className="hint">Loading…</p>

  // Settings save on their own, like everything else: choices at once, typed fields when you leave them.
  // A remote producer is saved once its name, model and URL are filled in.
  const complete = (d: ProducerProfile) => !!(d.name.trim() && d.model.trim() && d.base_url.trim())
  async function persist(next: Settings) {
    try {
      await api.saveSettings({ ...next, directors: next.directors.filter(complete) })
      setMsg('Saved'); setError('')
      api.producers().then(setProducers)
    } catch (e) { setError((e as Error).message) }
  }
  const set = <K extends keyof Settings>(k: K, v: Settings[K], save = false) => {
    const next = { ...s, [k]: v }
    setS(next)
    if (save) persist(next)
  }
  const setProfile = (i: number, p: Partial<ProducerProfile>) =>
    set('directors', s.directors.map((d, j) => (j === i ? { ...d, ...p } : d)))
  const commit = () => persist(s)   // on leaving a typed field
  const localModels = (setup?.items ?? []).filter((i) => i.id === 'llm' || i.id.startsWith('llm:'))
  const choices = [
    ...producers.filter((p) => p.local),
    ...s.directors.filter((d) => d.name).map((d) => ({ name: d.name, label: `${d.name} (${d.model})`, local: false })),
  ]
  const pct = setup?.total_bytes ? Math.min(100, Math.round((100 * setup.done_bytes) / setup.total_bytes)) : 0

  return (
    <section className="settings">
      <h1 className="page-title">Settings</h1>
      <p className="muted small">A producer turns a chapter's analysis into a script: it double-checks who says each line
        (ModernBookNLP alone gets 84.9% of the quotes in our test right) and describes the characters.</p>

      <h3>Local producers <span className="muted small">(run on this computer's GPU)</span></h3>
      <p className="muted small">Bigger models get more speakers right but need more GPU memory and take longer per chapter.</p>
      <table className="setup">
        <tbody>
          {localModels.map((m) => (
            <tr key={m.id}>
              <td>{m.title.replace('Script producer model: ', '')}</td>
              <td className="small muted">{gb(m.size)}</td>
              <td className="small muted">{m.license}</td>
              <td>{m.installed ? <span className="step done">installed</span>
                : setup?.installing && setup.current === m.title ? <span className="small muted">{pct}%</span>
                : setup?.installing && m.id === 'llm' ? <span className="small muted">waiting</span>   // part of the first-start download
                : <button className="small-btn" disabled={setup?.installing} title={setup?.installing ? 'after the current download' : undefined}
                    onClick={() => api.installModels([m.id]).then(setSetup).catch((e) => setError((e as Error).message))}>
                    Download</button>}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {setup?.installing && <div className="bar"><span style={{ width: `${pct}%` }} /></div>}
      {setup?.error && <p className="error">{setup.error} — press Download again to resume.</p>}

      <h3>Remote producers <span className="muted small">(any OpenAI-compatible endpoint)</span></h3>
      {s.directors.length === 0 && <p className="muted small">No remote producers yet.</p>}
      {s.directors.map((d, i) => (
        <div key={i} className="profile">
          <button className="ghost remove small-btn" title="Remove this producer"
            onClick={() => set('directors', s.directors.filter((_, j) => j !== i), true)}>Remove</button>
          {!complete(d) && <div className="small muted wide">incomplete — not saved yet (needs a name, model and endpoint URL)</div>}
          <label>Name <input placeholder="e.g. openai" value={d.name} onBlur={commit} onChange={(e) => setProfile(i, { name: e.target.value })} /></label>
          <label>Model <input placeholder="e.g. gpt-5.6-luna" value={d.model} onBlur={commit} onChange={(e) => setProfile(i, { model: e.target.value })} /></label>
          <label className="wide">Endpoint URL <input placeholder="https://api.example.com/v1" value={d.base_url} onBlur={commit}
            onChange={(e) => setProfile(i, { base_url: e.target.value })} /></label>
          <label className="wide">API key <input type="password" placeholder="leave empty if the endpoint needs none"
            value={d.api_key} onBlur={commit} onChange={(e) => setProfile(i, { api_key: e.target.value })} /></label>
        </div>))}
      <button className="ghost" onClick={() => set('directors', [...s.directors, { ...EMPTY }])}>+ Add producer</button>

      <h3>Share on my network</h3>
      <p className="muted small">Open this app from a browser on your phone, tablet or another computer on the same network.
        {' '}<b>There is no password:</b> anyone on your network can then use it, including deleting novels and voices — only
        turn this on at home or on a network you trust. Windows may ask to allow the app through its firewall.</p>
      {share?.forced
        ? <p className="note">Started in server mode (--serve): always shared on port {share.port}.</p>
        : <>
            <label><input type="checkbox" checked={s.share_network}
              onChange={(e) => { set('share_network', e.target.checked, true); loadShare() }} /> Share on my network</label>
            <label>Port <input type="number" min={1024} max={65535} value={s.share_port}
              onBlur={() => { commit(); loadShare() }} onChange={(e) => set('share_port', Number(e.target.value))} /></label>
          </>}
      {share?.running && share.urls.length > 0 && (
        <p>Open on other devices: {share.urls.map((u) => <code key={u} style={{ marginRight: 12 }}>{u}</code>)}</p>)}
      {share?.error && <p className="error">{share.error}</p>}

      <h3>Defaults</h3>
      <label>Default producer{' '}
        <select value={s.default_director} onChange={(e) => set('default_director', e.target.value, true)}>
          {!choices.some((c) => c.name === s.default_director) &&
            <option value={s.default_director} disabled>{s.default_director} (not available)</option>}
          {choices.map((c) => <option key={c.name} value={c.name}>{c.label}</option>)}
        </select></label>
      <label>Check for new chapters every (hours){' '}
        <input type="number" min={0.5} step={0.5} value={s.check_interval_hours}
          onBlur={commit} onChange={(e) => set('check_interval_hours', Number(e.target.value))} /></label>
      <label>MP3 quality{' '}
        <select value={s.mp3_bitrate} onChange={(e) => set('mp3_bitrate', e.target.value, true)}>
          {!MP3_RATES.includes(s.mp3_bitrate as never) && <option value={s.mp3_bitrate}>{s.mp3_bitrate}</option>}
          {MP3_RATES.map((r) => (
            <option key={r} value={r}>{r.replace('k', ' kbps')} · ~{Math.round((parseInt(r) * 3600) / 8 / 1000)} MB per hour
              {r === '96k' ? ' (default)' : ''}</option>
          ))}
        </select></label>

      {(msg || error) && <p className={error ? 'error' : 'note'}>{error || msg}</p>}
    </section>
  )
}
