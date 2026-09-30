import { useEffect, useState } from 'react'
import { api, type Novel } from '../api'

export default function Library() {
  const [novels, setNovels] = useState<Novel[] | null>(null)
  const [url, setUrl] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')

  const load = () => api.novels().then(setNovels).catch((e) => setError(e.message))
  useEffect(() => {
    load()
  }, [])

  async function track(e: React.FormEvent) {
    e.preventDefault()
    if (!url.trim()) return
    setBusy(true)
    setError('')
    try {
      const n = await api.track(url.trim())
      setUrl('')
      location.hash = `#/novel/${n.id}`
    } catch (err) {
      setError((err as Error).message)
    } finally {
      setBusy(false)
    }
  }

  async function untrack(n: Novel) {
    if (!confirm(`Stop tracking "${n.title}"?`)) return
    try {
      await api.untrack(n.id)
      await load()
    } catch (err) {
      setError((err as Error).message)
    }
  }

  return (
    <>
      <form className="add" onSubmit={track}>
        <input value={url} onChange={(e) => setUrl(e.target.value)} placeholder="Paste a web novel URL" disabled={busy} />
        <button disabled={busy || !url.trim()}>{busy ? 'Adding…' : 'Track'}</button>
      </form>
      {error && <p className="error">{error}</p>}
      {novels === null ? (
        <p className="hint">Loading…</p>
      ) : novels.length === 0 ? (
        <p className="empty">No tracked novels yet. Paste a novel's URL above.</p>
      ) : (
        <div className="grid">
          {novels.map((n) => (
            <div key={n.id} className="card">
              <a href={`#/novel/${n.id}`} className="card-link">
                {n.cover ? <img src={n.cover} alt="" /> : <div className="nocover" />}
                <div>
                  <div className="title">{n.title}</div>
                  <div className="author">{n.author}</div>
                  <div className="meta">{n.chapters ?? 0} chapters</div>
                </div>
              </a>
              <button className="ghost" title="Stop tracking" onClick={() => untrack(n)}>Remove</button>
            </div>
          ))}
        </div>
      )}
    </>
  )
}
