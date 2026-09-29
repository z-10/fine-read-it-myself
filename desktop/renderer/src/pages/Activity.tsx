import { Fragment, useEffect, useState } from 'react'
import { api, STEP_LABEL, type Job } from '../api'

export default function Activity() {
  const [jobs, setJobs] = useState<Job[]>([])
  const [open, setOpen] = useState<number | null>(null)

  useEffect(() => {
    const tick = () => api.jobs().then(setJobs)
    tick()
    const t = setInterval(tick, 2000)
    return () => clearInterval(t)
  }, [])

  return (
    <section>
      <h1 className="page-title">Activity</h1>
      {!jobs.length ? <p className="empty">Nothing has run yet. Select chapters on a novel page and run a step.</p> : (
        <table className="jobs"><tbody>
          {jobs.map((j) => (
            <Fragment key={j.id}>
              <tr onClick={() => setOpen(open === j.id ? null : j.id)} className="click">
                <td className="num">#{j.id}</td>
                <td><span className="step done">{STEP_LABEL[j.kind] ?? j.kind}</span></td>
                <td><a href={`#/chapter/${j.chapter_id}`} onClick={(e) => e.stopPropagation()}>{j.position}. {j.chapter_title}</a></td>
                <td><span className={`st ${j.state}`}>{j.state}</span></td>
                <td className="small muted">{j.stage}</td>
                <td className="w">{j.state === 'running' && <div className="bar"><span style={{ width: `${Math.round(100 * j.progress)}%` }} /></div>}</td>
                <td>{j.state === 'queued' && <button className="ghost small-btn" onClick={(e) => { e.stopPropagation(); api.cancelJob(j.id) }}>cancel</button>}</td>
              </tr>
              {open === j.id && <tr><td colSpan={7}><pre className="log">{j.log || '(no output yet)'}</pre></td></tr>}
            </Fragment>))}
        </tbody></table>
      )}
    </section>
  )
}
