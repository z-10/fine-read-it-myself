import { useEffect, useState } from 'react'
import Library from './pages/Library'
import NovelPage from './pages/NovelPage'
import ChapterPage from './pages/ChapterPage'
import VoicesPage from './pages/VoicesPage'
import SettingsPage from './pages/SettingsPage'
import SitesPage from './pages/SitesPage'
import Activity from './pages/Activity'
import AcknowledgementsPage from './pages/AcknowledgementsPage'
import { api, type SetupState } from './api'

// hash routes: #/ #/novel/<id> #/chapter/<id> #/voices #/sites #/settings #/activity #/acknowledgements
type Route =
  | { page: 'library' } | { page: 'voices' } | { page: 'settings' } | { page: 'sites' } | { page: 'activity' } | { page: 'acknowledgements' }
  | { page: 'novel'; id: number } | { page: 'chapter'; id: number; tab?: string }

function parse(): Route {
  const h = location.hash.replace(/^#\/?/, '')
  const m = h.match(/^(novel|chapter)\/(\d+)(?:\/(\w+))?/)
  if (m) return m[1] === 'chapter' ? { page: 'chapter', id: Number(m[2]), tab: m[3] } : { page: 'novel', id: Number(m[2]) }
  if (h === 'plugins') return { page: 'sites' }   // keep existing bookmarks working
  if (h === 'voices' || h === 'settings' || h === 'sites' || h === 'activity' || h === 'acknowledgements') return { page: h }
  return { page: 'library' }
}

export default function App() {
  const [route, setRoute] = useState<Route>(parse)
  const [setup, setSetup] = useState<SetupState | null>(null)
  useEffect(() => {   // first start: the required models download on their own; follow until they are all in
    const tick = () => api.setup().then(setSetup).catch(() => {})
    tick()
    const t = setInterval(tick, setup && !setup.complete ? 1000 : 15000)
    return () => clearInterval(t)
  }, [setup?.complete])
  useEffect(() => {
    const on = () => setRoute(parse())
    addEventListener('hashchange', on)
    return () => removeEventListener('hashchange', on)
  }, [])
  const nav = (page: Route['page'], href: string, text: string) =>
    <a href={href} className={route.page === page ? 'on' : ''}>{text}</a>
  return (
    <div className="app">
      <header className="top">
        <a href="#/" className="brand">Fine, I'll read it myself!</a>
        <nav>
          {nav('library', '#/', 'Library')}
          {nav('activity', '#/activity', 'Activity')}
          {nav('voices', '#/voices', 'Voices')}
          {nav('sites', '#/sites', 'Sites')}
          {nav('settings', '#/settings', 'Settings')}
          {nav('acknowledgements', '#/acknowledgements', 'Acknowledgements')}
        </nav>
      </header>
      {setup && !setup.complete && <ModelsBanner s={setup} retry={() => api.installModels().then(setSetup)} />}
      <main>
        {route.page === 'novel' ? <NovelPage key={route.id} id={route.id} />
          : route.page === 'chapter' ? <ChapterPage key={route.id} id={route.id} initialTab={route.tab} />
            : route.page === 'voices' ? <VoicesPage />
              : route.page === 'sites' ? <SitesPage />
              : route.page === 'settings' ? <SettingsPage />
                : route.page === 'activity' ? <Activity />
                  : route.page === 'acknowledgements' ? <AcknowledgementsPage />
                  : <Library />}
      </main>
    </div>
  )
}

const gb = (n: number) => `${(n / 1e9).toFixed(1)} GB`

// the models every chapter needs are still downloading (or a download stopped)
function ModelsBanner({ s, retry }: { s: SetupState; retry: () => void }) {
  const pct = s.total_bytes ? Math.min(100, Math.round((100 * s.done_bytes) / s.total_bytes)) : 0
  if (s.items.some((i) => i.group === 'bundled' && !i.installed))
    return <div className="banner">The voice pool that ships with the app is missing ({s.voices_dir}). Reinstall the app.</div>
  return (
    <div className="banner">
      {s.installing ? <>
        <div>Downloading the models this app needs (first start only): {gb(s.done_bytes)} of {gb(s.total_bytes)} — {s.current}</div>
        <div className="bar"><span style={{ width: `${pct}%` }} /></div>
      </> : <>
        <div>{s.error ? `Model download stopped: ${s.error}` : `Models to download: ${gb(s.missing_bytes)}`}</div>
        <button className="small-btn" onClick={retry}>{s.error ? 'Retry' : 'Download'}</button>
      </>}
    </div>
  )
}
