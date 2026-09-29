// "Fine, I'll read it myself!" desktop shell: starts the C++ backend (readmyself-server) on a free localhost
// port and shows the UI it serves (renderer/dist).
//
// Server mode: `fine-read-it --serve [--port 8765]` opens no window; the backend shares the UI with browsers on the
// network (no password) and a tray icon offers "Open in browser" and "Quit". The desktop app can also share while
// its window is open (Settings -> Share on my network).
const { app, BrowserWindow, shell, Menu, Tray, nativeImage } = require('electron')
const { spawn } = require('child_process')
const fs = require('fs')
const http = require('http')
const net = require('net')
const path = require('path')

const DEV = !app.isPackaged
const EXE = process.platform === 'win32' ? 'readmyself-server.exe' : 'readmyself-server'
const paths = {
  server: DEV ? path.join(__dirname, '..', 'native', 'build', EXE) : path.join(process.resourcesPath, 'bin', EXE),
  web: DEV ? path.join(__dirname, 'renderer', 'dist') : path.join(process.resourcesPath, 'web'),
  // voice pool (LibriTTS-R, CC BY 4.0) built by `readmyself-server --build-voice-pool` + `--export-voice-pool`
  voices: DEV ? path.join(__dirname, 'voices') : path.join(process.resourcesPath, 'voices'),
  data: process.env.READMYSELF_DATA || path.join(app.getPath('userData'), 'data'),
  models: process.env.READMYSELF_MODELS || path.join(app.getPath('userData'), 'models'),
  logs: path.join(app.getPath('userData'), 'logs'),
}

let server = null
let port = 0
let mainWindow = null
let tray = null

const argv = process.argv.slice(1)
const SERVE = argv.includes('--serve')
const SERVE_PORT = (() => {
  const i = argv.indexOf('--port')
  const n = i >= 0 ? Number(argv[i + 1]) : 8765
  return Number.isInteger(n) && n >= 1024 && n <= 65535 ? n : 8765
})()

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer()
    s.unref()
    s.on('error', reject)
    s.listen(0, '127.0.0.1', () => {
      const p = s.address().port
      s.close(() => resolve(p))
    })
  })
}

function ping(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      res.resume()
      res.statusCode === 200 ? resolve() : reject(new Error(`HTTP ${res.statusCode}`))
    }).on('error', reject)
  })
}

async function startServer() {
  port = await freePort()
  fs.mkdirSync(paths.logs, { recursive: true })
  const log = fs.createWriteStream(path.join(paths.logs, 'server.log'), { flags: 'a' })
  const args = ['--host', '127.0.0.1', '--port', String(port), '--data', paths.data,
    '--models', paths.models, '--web', paths.web, '--voices', paths.voices]
  if (SERVE) args.push('--share-port', String(SERVE_PORT))   // server mode: always shared on the network
  server = spawn(paths.server, args, { env: process.env, windowsHide: true })
  server.stdout.pipe(log)
  server.stderr.pipe(log)
  server.on('exit', (code) => {
    server = null
    log.write(`server exited with ${code}\n`)
  })
  for (let i = 0; i < 100; i++) {
    try {
      return await ping(`http://127.0.0.1:${port}/api/sources`)
    } catch {
      await new Promise((r) => setTimeout(r, 200))
    }
  }
  throw new Error('The backend did not start; see logs/server.log.')
}

function createMain() {
  mainWindow = new BrowserWindow({
    width: 1200, height: 860, title: "Fine, I'll read it myself!", autoHideMenuBar: true,
    icon: path.join(__dirname, 'icon.png'),
    webPreferences: { backgroundThrottling: !process.env.READMYSELF_SCREENSHOT },
  })
  mainWindow.loadURL(`http://127.0.0.1:${port}/${process.env.READMYSELF_START_HASH || ''}`)
  // links to web novel sites open in the default browser
  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })
  if (process.env.READMYSELF_SCREENSHOT) {   // smoke tests: capture the UI once loaded, then quit
    mainWindow.webContents.once('did-finish-load', () => setTimeout(async () => {
      const img = await mainWindow.webContents.capturePage()
      fs.writeFileSync(process.env.READMYSELF_SCREENSHOT, img.toPNG())
      app.quit()
    }, 3000))
  }
}

// server mode: no window, a tray icon instead
async function startTray() {
  const icon = nativeImage.createFromPath(path.join(__dirname, 'tray.png'))
  tray = new Tray(icon)
  tray.setToolTip(`Fine, I'll read it myself! — serving on port ${SERVE_PORT}`)
  const openBrowser = () => shell.openExternal(`http://localhost:${SERVE_PORT}/`)
  const build = (urls) => Menu.buildFromTemplate([
    { label: 'Open in browser', click: openBrowser },
    { label: 'Open window', click: () => { if (mainWindow) mainWindow.focus(); else createMain() } },
    { type: 'separator' },
    ...urls.map((u) => ({ label: `On your network: ${u}`, enabled: false })),
    { type: 'separator' },
    { label: 'Quit', click: () => app.quit() },
  ])
  tray.setContextMenu(build([]))
  tray.on('double-click', openBrowser)
  // the addresses other devices use, once the network listener is up
  http.get(`http://127.0.0.1:${port}/api/share`, (res) => {
    let body = ''
    res.on('data', (c) => { body += c })
    res.on('end', () => { try { tray.setContextMenu(build(JSON.parse(body).urls || [])) } catch { /* keep the plain menu */ } })
  }).on('error', () => {})
}

if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  app.on('second-instance', () => {
    if (SERVE && !mainWindow) return shell.openExternal(`http://localhost:${SERVE_PORT}/`)
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore()
      mainWindow.focus()
    }
  })
  app.whenReady().then(async () => {
    Menu.setApplicationMenu(null)
    try {
      await startServer()
    } catch (e) {
      const w = new BrowserWindow({ width: 640, height: 240 })
      w.loadURL('data:text/html,' + encodeURIComponent(`<p style="font-family:sans-serif">${e.message}</p>`))
      return
    }
    if (SERVE) await startTray()
    else createMain()
  })
  app.on('before-quit', () => {
    if (server) server.kill()
  })
  app.on('window-all-closed', () => { if (!SERVE) app.quit() })   // server mode keeps running in the tray
}
