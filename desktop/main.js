// "Fine, I'll read it myself!" desktop shell: starts the C++ backend (readmyself-server) on a free localhost
// port and shows the UI it serves (renderer/dist).
const { app, BrowserWindow, shell, Menu } = require('electron')
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
  server = spawn(paths.server, ['--host', '127.0.0.1', '--port', String(port), '--data', paths.data,
    '--models', paths.models, '--web', paths.web, '--voices', paths.voices], { env: process.env, windowsHide: true })
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

if (!app.requestSingleInstanceLock()) {
  app.quit()
} else {
  app.on('second-instance', () => {
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
    createMain()
  })
  app.on('before-quit', () => {
    if (server) server.kill()
  })
  app.on('window-all-closed', () => app.quit())
}
