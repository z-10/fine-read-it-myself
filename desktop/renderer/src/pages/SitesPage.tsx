import { useEffect, useState } from 'react'
import { api, type Plugin, type PluginConfig } from '../api'

export default function SitesPage() {
  const [plugins, setPlugins] = useState<Plugin[] | null>(null)
  const [error, setError] = useState('')
  const load = () => {
    setError('')
    api.plugins().then(setPlugins).catch((e: Error) => setError(e.message))
  }
  useEffect(load, [])

  return <section className="settings plugins">
    <h1 className="page-title">Sites</h1>
    <p className="muted small">Configure the sites you read from. Paste a novel or chapter URL in the Library to start.</p>
    {error && <p className="error" role="alert">{error} <button className="ghost small-btn" onClick={load}>Retry</button></p>}
    {!plugins && !error && <p className="hint">Loading…</p>}
    {plugins?.length === 0 && <p className="hint">No sites are available.</p>}
    {plugins?.some((plugin) => !plugin.config || plugin.delay_seconds === undefined) &&
      <p className="note" role="status">The backend needs to be updated and restarted to configure sites.
        {' '}<button className="ghost small-btn" onClick={load}>Retry</button></p>}
    {plugins?.map((plugin) => plugin.config && plugin.delay_seconds !== undefined
      ? <PluginCard key={plugin.id} plugin={plugin} initialConfig={plugin.config} defaultDelay={plugin.delay_seconds} />
      : <article key={plugin.id} className="plugin-card">
          <h3>{plugin.name}</h3>
          <a className="small" href={plugin.homepage} target="_blank" rel="noreferrer">{plugin.homepage}</a>
        </article>)}
  </section>
}

function PluginCard({ plugin, initialConfig, defaultDelay }: { plugin: Plugin; initialConfig: PluginConfig; defaultDelay: number }) {
  const [config, setConfig] = useState(initialConfig)
  const [cookie, setCookie] = useState('')
  const [delay, setDelay] = useState(config.delay_seconds?.toString() ?? '')
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')

  async function save(patch: Partial<PluginConfig>) {
    setBusy(true); setMessage(''); setError('')
    try {
      const saved = await api.savePlugin(plugin.id, patch)
      setConfig(saved); setCookie(''); setDelay(saved.delay_seconds?.toString() ?? '')
      setMessage('Saved')
    } catch (e) { setError((e as Error).message) }
    finally { setBusy(false) }
  }

  function submit(e: React.FormEvent) {
    e.preventDefault()
    const seconds = delay.trim() ? Number(delay) : null
    if (seconds !== null && (!Number.isFinite(seconds) || seconds < 0 || seconds > 60)) {
      setError('Request delay must be between 0 and 60 seconds.'); return
    }
    save({ delay_seconds: seconds, ...(cookie.trim() ? { cookie: cookie.trim() } : {}) })
  }

  return <form className="plugin-card" onSubmit={submit}>
    <h3>{plugin.name}</h3>
    <a className="small" href={plugin.homepage} target="_blank" rel="noreferrer">{plugin.homepage}</a>
    <fieldset disabled={busy}>
      <label>Request delay (seconds)
        <input type="number" min="0" max="60" step="0.1" value={delay}
          placeholder={String(defaultDelay)} onChange={(e) => setDelay(e.target.value)} />
      </label>
      <p className="muted small">Leave empty to use the default for this site ({defaultDelay} seconds).</p>
      {plugin.login && <>
        <label>Login cookie <span className="muted small">{config.cookie ? '(saved)' : '(optional)'}</span>
          <input type="password" autoComplete="off" value={cookie}
            placeholder={config.cookie ? 'Leave empty to keep the saved cookie' : 'Cookie request header'}
            onChange={(e) => setCookie(e.target.value)} />
        </label>
        <p className="muted small">To use your own site login, open the site in your browser, then developer tools (F12) → Network.
          Reload a chapter and copy its Cookie request header here. The cookie is stored on the computer running this app.</p>
      </>}
      <div className="toolbar">
        <button type="submit">{busy ? 'Saving…' : 'Save'}</button>
        {config.cookie && <button type="button" className="ghost" onClick={() => save({ cookie: '' })}>Forget login</button>}
      </div>
    </fieldset>
    {error && <p className="error" role="alert">{error}</p>}
    {message && <p className="note" role="status">{message}</p>}
  </form>
}
