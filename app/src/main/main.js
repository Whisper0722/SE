'use strict';

// =============================================================================
// main.js - Electron 主进程
//
// 职责：
//   * 创建窗口，加载 Vite 构建出的渲染层（dist/renderer/index.html）
//   * 启动/守护 C++ 核心 player-core.exe（stdio JSON-RPC）
//   * 在渲染进程与核心之间转发请求和通知
//   * 文件夹选择、系统媒体键
//
// 进程模型：Renderer <-> preload <-> 主进程 <-> player-core.exe
// 渲染进程没有 Node 权限，所有能力都经 preload 的 contextBridge 暴露。
// =============================================================================

const { app, BrowserWindow, ipcMain, dialog, globalShortcut } = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const { RpcClient } = require('./rpc-client');

app.setName('Nightfall');

let mainWindow = null;
let rpc = null;
let coreError = null;
let pendingEvents = [];

// 核心可执行文件路径。
// 默认从 app/src/main 向上三级到仓库根，再进入 player-core/build。
// 可用 NIGHTFALL_CORE 覆盖（打包或调试时很有用）。
function corePath() {
  if (process.env.NIGHTFALL_CORE) return process.env.NIGHTFALL_CORE;
  return path.resolve(__dirname, '../../../player-core/build/player-core.exe');
}

// 自包含的运行时 DLL 目录（scripts/bundle-runtime.sh 生成）。
// 换机时只要带上 player-core/runtime，即使没装 MSYS2 也能启动核心。
function runtimeBin() {
  return (
    process.env.NIGHTFALL_RUNTIME_BIN ||
    path.resolve(__dirname, '../../../player-core/runtime')
  );
}

// MSYS2 的 MinGW DLL 目录。只有 runtime 缺失时才会用到它。
// 可用 NIGHTFALL_MSYS2_BIN 覆盖（MSYS2 安装在其他盘时）。
function msys2Bin() {
  return process.env.NIGHTFALL_MSYS2_BIN || 'I:\\msys64\\mingw64\\bin';
}

// 曲库数据库路径。默认放在 Electron 的 userData（每台机器独立）；
// 测试/演示时可用 NIGHTFALL_DB 指向 player-core/build/smoke.db。
function databasePath() {
  return process.env.NIGHTFALL_DB || path.join(app.getPath('userData'), 'library.db');
}

// 把核心通知转发给渲染进程。
// 窗口还没加载完时先缓存，等 did-finish-load 再统一发出，避免丢事件。
function sendEvent(message) {
  if (
    mainWindow &&
    !mainWindow.isDestroyed() &&
    mainWindow.webContents &&
    !mainWindow.webContents.isLoading()
  ) {
    mainWindow.webContents.send('core:event', message);
    return;
  }
  pendingEvents.push(message);
  if (pendingEvents.length > 300) pendingEvents.shift();
}

// 启动 player-core，并做一次 ping 确认协议可用。
// 子进程 PATH 依次加入 runtime 和 MSYS2 bin：
//   runtime 优先（自包含），MSYS2 兜底（开发机）。
async function startCore() {
  const exe = corePath();
  if (!fs.existsSync(exe)) {
    throw new Error(`player-core not found: ${exe}\nBuild it with scripts/build-core.sh first.`);
  }

  const pathEntries = [runtimeBin(), msys2Bin()].filter((entry) =>
    fs.existsSync(entry)
  );

  const client = new RpcClient(
    exe,
    ['--db', databasePath(), '--audio', process.env.NIGHTFALL_AUDIO || 'wasapi'],
    {
      env: {
        ...process.env,
        PATH: pathEntries.join(';') + ';' + (process.env.PATH || ''),
      },
    }
  );

  // 核心推送（scan.progress/playback.state/library.changed）统一转发给渲染层。
  client.onNotification(sendEvent);
  await client.start();
  // spawn 成功不代表 DLL 都加载成功，ping 一次可以尽早发现 0xC0000135 之类的问题。
  await client.request('ping', {}, 5000);
  rpc = client;
  console.log('[main] core ready:', exe);
}

// 创建主窗口。
// 安全设置：contextIsolation + sandbox + 禁用 nodeIntegration。
// 页面只能通过 preload 暴露的 window.nightfall 与主进程通信。
function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 900,
    minHeight: 560,
    backgroundColor: '#000000',
    show: false,
    autoHideMenuBar: true,
    title: 'Nightfall',
    webPreferences: {
      preload: path.join(__dirname, '../preload/preload.js'),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
    },
  });

  mainWindow.once('ready-to-show', () => mainWindow.show());

  mainWindow.webContents.on('did-finish-load', () => {
    const events = pendingEvents;
    pendingEvents = [];
    for (const event of events) {
      mainWindow.webContents.send('core:event', event);
    }

    // ---- 以下都是测试辅助，仅在设置环境变量时生效 ----
    // 自动点击（可串联多个选择器，例如 ".root-remove|.dialog .pill-btn.danger"）。
    if (process.env.NIGHTFALL_AUTOPLAY) {
      const selectors = (process.env.NIGHTFALL_CLICK_SELECTOR || '.track-row').split('|');
      selectors.forEach((selector, index) => {
        setTimeout(
          () => {
            mainWindow.webContents
              .executeJavaScript(`document.querySelector(${JSON.stringify(selector)})?.click();`)
              .catch((error) => console.error('[main] autoplay failed', error));
          },
          1200 + index * 800
        );
      });
    }

    // 在页面执行任意测试脚本（例如注入 pointer 事件测试进度条拖动）。
    if (process.env.NIGHTFALL_EXEC) {
      setTimeout(() => {
        mainWindow.webContents
          .executeJavaScript(process.env.NIGHTFALL_EXEC)
          .catch((error) => console.error('[main] exec failed', error));
      }, Number(process.env.NIGHTFALL_EXEC_DELAY || 1500));
    }

    // 截取窗口内容并退出，用于自动化验证 UI。
    if (process.env.NIGHTFALL_SCREENSHOT) {
      const delay = Number(process.env.NIGHTFALL_SCREENSHOT_DELAY || 2500);
      setTimeout(async () => {
        try {
          const image = await mainWindow.webContents.capturePage();
          fs.writeFileSync(process.env.NIGHTFALL_SCREENSHOT, image.toPNG());
          console.log('[main] screenshot written:', process.env.NIGHTFALL_SCREENSHOT);
        } catch (error) {
          console.error('[main] screenshot failed', error);
        }
        setTimeout(() => app.quit(), 300);
      }, delay);
    }
  });

  mainWindow.loadFile(path.join(__dirname, '../../dist/renderer/index.html'));

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

// 应用启动：先启动核心（失败也继续创建窗口，让界面显示错误），
// 再创建窗口、注册媒体键。
app.whenReady().then(async () => {
  try {
    await startCore();
  } catch (error) {
    coreError = error.message;
    console.error('[main] core failed to start:', error);
  }

  createWindow();

  // 媒体键：Electron 只能接按键，不负责系统媒体浮层（SMTC 不在范围内）。
  // 注册失败通常是因为别的播放器已经占用了该热键。
  const bindMediaKey = (accelerator, method) => {
    try {
      globalShortcut.register(accelerator, () => {
        if (rpc) rpc.request(method).catch(() => {});
      });
    } catch (error) {
      // Another application may own the media key.
    }
  };
  bindMediaKey('MediaPlayPause', 'playback.toggle');
  bindMediaKey('MediaNextTrack', 'playback.next');
  bindMediaKey('MediaPreviousTrack', 'playback.prev');

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

// 渲染进程 -> 核心的通用 RPC 通道。
// 只接受字符串方法名；参数由 preload 传入的普通对象。
ipcMain.handle('core:request', async (_event, method, params) => {
  if (!rpc) throw new Error(coreError || 'player-core is not running');
  if (typeof method !== 'string') throw new Error('invalid method');
  return rpc.request(method, params || {});
});

// 打开系统文件夹选择器；取消时返回 null。
ipcMain.handle('dialog:pickFolder', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openDirectory'],
    title: '选择音乐文件夹',
  });
  return result.canceled ? null : result.filePaths[0];
});

// 启动信息：路径、核心错误、音频模式，供渲染层显示/排错。
ipcMain.handle('app:info', () => ({
  corePath: corePath(),
  dbPath: databasePath(),
  runtimePath: runtimeBin(),
  coreError,
  audioMode: process.env.NIGHTFALL_AUDIO || 'wasapi',
  version: app.getVersion(),
  packaged: app.isPackaged,
}));

// Windows/Linux 关闭所有窗口即退出；macOS 保持应用存活。
app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

// 退出前注销热键并终止核心子进程，避免留下孤儿进程。
app.on('before-quit', () => {
  globalShortcut.unregisterAll();
  if (rpc) {
    rpc.stop();
    rpc = null;
  }
});
