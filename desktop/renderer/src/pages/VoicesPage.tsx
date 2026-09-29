import { useEffect, useMemo, useState } from 'react'
import { api, sampleUrl, type CatalogVoice, type Voice, type VoiceDownload } from '../api'

type Filter = 'all' | 'male' | 'female' | 'banned'
const PAGE_SIZES = [25, 50, 100, 0] as const   // 0 = all

export default function VoicesPage() {
  const [tab, setTab] = useState<'installed' | 'more'>('installed')
  const [more, setMore] = useState<number | null>(null)   // catalog voices not installed yet
  useEffect(() => { api.voiceCatalog().then((c) => setMore(c.voices.filter((v) => !v.installed).length)).catch(() => {}) }, [tab])
  return (
    <section>
      <h1 className="page-title">Voices</h1>
      <div className="tabs">
        <button className={tab === 'installed' ? 'on' : ''} onClick={() => setTab('installed')}>Installed</button>
        <button className={tab === 'more' ? 'on' : ''} onClick={() => setTab('more')}>More voices{more ? ` (${more})` : ''}</button>
      </div>
      {tab === 'installed' ? <InstalledVoices /> : <MoreVoices />}
    </section>
  )
}

const BAND = { low: 'low pitch', mid: 'medium pitch', high: 'high pitch' } as Record<string, string>
const readerName = (r: string) => r.replace(/([a-z])([A-Z])/g, '$1 $2')

// tested voices from the voice catalog that are not installed: listen, download one or all
function MoreVoices() {
  const [voices, setVoices] = useState<CatalogVoice[] | null>(null)
  const [error, setError] = useState('')
  const [url, setUrl] = useState('')
  const [dl, setDl] = useState<VoiceDownload | null>(null)
  const [gender, setGender] = useState<'all' | 'male' | 'female'>('all')
  const load = () => api.voiceCatalog().then((c) => { setVoices(c.voices); setError(c.error); setUrl(c.url); setDl(c.download) })
    .catch((e) => setError((e as Error).message))
  useEffect(() => { load() }, [])
  useEffect(() => {
    if (!dl?.running) return
    const t = setInterval(() => api.voiceDownload().then((d) => { setDl(d); if (!d.running) load() }), 1000)
    return () => clearInterval(t)
  }, [dl?.running])

  if (error) return <p className="error">Voice catalog unavailable: {error}</p>
  if (!voices) return <p className="hint">Loading the voice catalog…</p>
  const missing = voices.filter((v) => !v.installed && (gender === 'all' || v.gender === gender))
  const download = (ids: string[]) => api.downloadVoices(ids).then(setDl).catch((e) => setError((e as Error).message))
  return (
    <>
      <p className="muted small">Tested voices of LibriVox readers (LibriTTS-R, CC BY 4.0), from the{' '}
        <a href={url.replace('/resolve/main/', '')} target="_blank" rel="noreferrer">voice catalog</a>. Downloaded voices
        are used for new characters and can be picked in any cast. About 1 MB each.</p>
      <div className="toolbar">
        <div className="seg">
          {(['all', 'male', 'female'] as const).map((g) =>
            <button key={g} className={gender === g ? 'on' : ''} onClick={() => setGender(g)}>{g}</button>)}
        </div>
        <span className="spacer" />
        {dl?.running
          ? <span className="note">Downloading {dl.done} / {dl.total}{dl.current ? ` (${dl.current})` : ''}…</span>
          : missing.length > 0 && <button onClick={() => download(missing.map((v) => v.id))}>Download all {missing.length}</button>}
      </div>
      {dl?.error && <p className="error">{dl.error} — press Download again to resume.</p>}
      {missing.length === 0 && <p className="empty">All {gender === 'all' ? '' : gender + ' '}voices of the catalog are installed.</p>}
      <table className="voices"><tbody>
        {missing.map((v) => (
          <tr key={v.id}>
            <td><audio controls preload="none" src={v.sample_url} className="mini" /></td>
            <td><b>{readerName(v.reader)}</b> <span className="muted">({v.gender}, {BAND[v.band] ?? v.band})</span></td>
            <td><button className="ghost small-btn" disabled={dl?.running} onClick={() => download([v.id])}>Download</button></td>
          </tr>
        ))}
      </tbody></table>
    </>
  )
}

function InstalledVoices() {
  const [voices, setVoices] = useState<Voice[]>([])
  const [filter, setFilter] = useState<Filter>('all')
  const [msg, setMsg] = useState('')
  const [page, setPage] = useState(0)
  const [pageSize, setPageSize] = useState<number>(() => {
    try { return Number(localStorage.getItem('voicesPerPage') ?? 25) } catch { return 25 }
  })

  const load = () => api.voices().then(setVoices)
  useEffect(() => { load() }, [])

  const shown = useMemo(() => voices.filter(v =>
    filter === 'all' ? true :
      filter === 'banned' ? v.banned :
        v.gender === filter && v.usable), [voices, filter])
  const per = pageSize || Math.max(1, shown.length)
  const pages = Math.max(1, Math.ceil(shown.length / per))
  useEffect(() => { setPage(0) }, [filter, pageSize])
  useEffect(() => { if (page >= pages) setPage(pages - 1) }, [page, pages])
  const pageRows = shown.slice(page * per, (page + 1) * per)

  async function patch(v: Voice, p: { gender?: 'male' | 'female' | 'reset'; banned?: boolean }) {
    const r = await api.patchVoice(v.id, p)
    setMsg(r.recast.length ? `Re-cast: ${r.recast.join('; ')}` : '')
    load()
  }

  const counts = {
    male: voices.filter(v => v.usable && v.gender === 'male').length,
    female: voices.filter(v => v.usable && v.gender === 'female').length,
    banned: voices.filter(v => v.banned).length,
  }

  return (
    <>
      <div className="toolbar">
        <div className="seg">
          {([['all', `all (${voices.length})`], ['male', `male (${counts.male})`], ['female', `female (${counts.female})`],
            ['banned', `banned (${counts.banned})`]] as const).map(([f, t]) =>
            <button key={f} className={filter === f ? 'on' : ''} onClick={() => setFilter(f)}>{t}</button>)}
        </div>
      </div>
      <p className="muted small">Gender comes from the LibriTTS-R reader metadata; flip it here if it's wrong.
        Banned voices are never used; characters using a banned or flipped voice get a new one automatically
        (unless you picked their voice yourself).</p>
      {msg && <p className="note">{msg}</p>}
      <table className="voices"><tbody>
        {pageRows.map(v => (
          <tr key={v.id} className={v.usable ? '' : 'off'}>
            <td><audio controls preload="none" src={sampleUrl(v.id)} className="mini" /></td>
            <td><b>{v.label}</b></td>
            <td>
              <div className="seg">
                {(['female', 'male'] as const).map(g =>
                  <button key={g} className={v.gender === g ? 'on' : ''} disabled={v.gender === g}
                    onClick={() => patch(v, { gender: g })}>{g === 'female' ? 'F' : 'M'}</button>)}
              </div>
              {v.gender_overridden && <button className="ghost small-btn" title={`dataset says ${v.metadata_gender}`}
                onClick={() => patch(v, { gender: 'reset' })}>reset</button>}
            </td>
            <td><label className="small"><input type="checkbox" checked={v.banned}
              onChange={e => patch(v, { banned: e.target.checked })} /> banned</label></td>
          </tr>))}
      </tbody></table>
      <div className="pager">
        {pages > 1 && <>
          <button className="ghost" disabled={page === 0} onClick={() => setPage(0)}>«</button>
          <button className="ghost" disabled={page === 0} onClick={() => setPage(p => p - 1)}>‹</button>
          <span className="small">page {page + 1} of {pages}</span>
          <button className="ghost" disabled={page >= pages - 1} onClick={() => setPage(p => p + 1)}>›</button>
          <button className="ghost" disabled={page >= pages - 1} onClick={() => setPage(pages - 1)}>»</button>
        </>}
        <span className="small muted">{shown.length} voices ·</span>
        <label className="small">per page{' '}
          <select value={pageSize} onChange={e => {
            const n = Number(e.target.value)
            setPageSize(n)
            try { localStorage.setItem('voicesPerPage', String(n)) } catch { /* storage unavailable */ }
          }}>
            {PAGE_SIZES.map(n => <option key={n} value={n}>{n || 'all'}</option>)}
          </select>
        </label>
      </div>

      <footer className="attribution">
        <h3>About these voices</h3>
        <p>
          The voices come from <a href="https://www.openslr.org/141/" target="_blank" rel="noreferrer">LibriTTS-R</a>,
          a sound-quality-restored version of the LibriTTS corpus: about 585 hours of English speech at 24&nbsp;kHz,
          read by volunteers of <a href="https://librivox.org/" target="_blank" rel="noreferrer">LibriVox</a> from
          public-domain books. Each voice is named after its reader.
        </p>
        <p className="small">
          Yuma Koizumi, Heiga Zen, Shigeki Karita, Yifan Ding, Kohei Yatabe, Nobuyuki Morioka, Michiel Bacchiani,
          Yu Zhang, Wei Han, and Ankur Bapna, “LibriTTS-R: A Restored Multi-Speaker Text-to-Speech Corpus,” arXiv, 2023.
        </p>
        <p className="small">
          Licensed under <a href="https://creativecommons.org/licenses/by/4.0/" target="_blank" rel="noreferrer">CC BY 4.0</a>.
          Changes: short reference clips were cut from the original recordings and are used to clone the voices
          with VoxCPM2.
        </p>
      </footer>
    </>
  )
}
