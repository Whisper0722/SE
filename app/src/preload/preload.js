'use strict';

// =============================================================================
// preload.js - 渲染进程与主进程之间唯一的安全桥
//
// 渲染进程运行在沙箱中，没有 Node/Electron 完整权限；
// 这里只暴露最小、明确的 API：
//   request(method, params)  调用核心 RPC
//   onEvent(callback)        订阅核心通知，返回取消函数
//   pickFolder()             打开文件夹选择器
//   getInfo()                获取启动/排错信息
// =============================================================================

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('nightfall', {
  // 请求核心，最终由主进程的 'core:request' 转发给 player-core。
  request: (method, params) => ipcRenderer.invoke('core:request', method, params),

  // 订阅主进程转发过来的核心通知（scan.progress/playback.state/library.changed）。
  // 返回的函数用于取消订阅，避免 React 卸载后继续收到事件。
  onEvent: (callback) => {
    const listener = (_event, message) => callback(message);
    ipcRenderer.on('core:event', listener);
    return () => ipcRenderer.removeListener('core:event', listener);
  },

  // 系统文件夹选择对话框。
  pickFolder: () => ipcRenderer.invoke('dialog:pickFolder'),

  // 启动信息：核心路径、数据库路径、核心是否启动失败等。
  getInfo: () => ipcRenderer.invoke('app:info'),
});
