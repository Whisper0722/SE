'use strict';

// =============================================================================
// rpc-client.js - 主进程侧的核心进程客户端
//
// 与 player-core.exe 通过 stdin/stdout 通信，帧格式：
//   Content-Length: <字节数>\r\n
//   \r\n
//   <UTF-8 JSON>
//
// 请求用 id 关联 Promise；核心主动推送的通知（scan.progress、
// playback.state 等）通过 onNotification 注册的监听器转发给窗口。
// =============================================================================

const { spawn } = require('node:child_process');

class RpcClient {
  constructor(corePath, args = [], options = {}) {
    this.corePath = corePath;          // 可执行文件路径
    this.args = args;                  // --db/--audio 等参数
    this.env = options.env;            // 子进程环境（包含 DLL PATH）
    this.listeners = new Set();        // 通知监听器
    this.pending = new Map();          // id -> {resolve, reject, timer}
    this.nextId = 1;
    this.buffer = Buffer.alloc(0);     // 尚未组成完整帧的字节
    this.proc = null;
  }

  // 启动核心进程。resolve 只表示 spawn 成功，不代表业务已就绪；
  // 调用方通常还会发一次 ping。
  start() {
    return new Promise((resolve, reject) => {
      let settled = false;
      this.proc = spawn(this.corePath, this.args, {
        stdio: ['pipe', 'pipe', 'pipe'],
        env: this.env,
        windowsHide: true,  // 不弹控制台窗口
      });

      this.proc.once('spawn', () => {
        if (!settled) {
          settled = true;
          resolve();
        }
      });

      this.proc.once('error', (error) => {
        if (!settled) {
          settled = true;
          reject(error);
        }
        this._failAll(error);
      });

      // stdout 是协议数据，交给帧解析器；stderr 是日志。
      this.proc.stdout.on('data', (chunk) => this._onData(chunk));
      this.proc.stderr.on('data', (chunk) => {
        for (const line of chunk.toString('utf8').split(/\r?\n/)) {
          if (line.trim()) console.error('[core]', line);
        }
      });

      // 核心退出：所有未完成请求立即失败，并通知界面。
      // 例如缺少 DLL 时退出码是 3221225781（0xC0000135）。
      this.proc.once('exit', (code, signal) => {
        this._failAll(new Error(`player-core exited (${code ?? signal})`));
        this._emit({ method: 'core.exit', params: { code, signal } });
      });
    });
  }

  // 累积 stdout 字节，尽可能解析出完整帧。
  // 一帧可能跨多个 data 事件，也可能一次读到多帧，因此用循环。
  _onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);

    for (;;) {
      const headerEnd = this.buffer.indexOf('\r\n\r\n');
      if (headerEnd === -1) return;  // 头还没收全

      const header = this.buffer.subarray(0, headerEnd).toString('ascii');
      const match = /content-length:\s*(\d+)/i.exec(header);
      if (!match) {
        // 无法识别的帧头：丢弃这段头，继续找下一个。
        this.buffer = this.buffer.subarray(headerEnd + 4);
        continue;
      }

      const length = Number(match[1]);
      const bodyStart = headerEnd + 4;
      if (this.buffer.length < bodyStart + length) return;  // body 还没收全

      const body = this.buffer.subarray(bodyStart, bodyStart + length).toString('utf8');
      this.buffer = this.buffer.subarray(bodyStart + length);

      let message;
      try {
        message = JSON.parse(body);
      } catch (error) {
        console.error('[rpc] invalid JSON from core', error);
        continue;
      }

      if (message.id !== undefined && this.pending.has(message.id)) {
        // 响应：兑现对应的 Promise。
        const pending = this.pending.get(message.id);
        this.pending.delete(message.id);
        clearTimeout(pending.timer);
        if (message.error) {
          pending.reject(new Error(message.error.message || 'RPC error'));
        } else {
          pending.resolve(message.result);
        }
      } else if (message.method) {
        // 通知（没有 id）：交给监听器。
        this._emit(message);
      }
    }
  }

  // 安全地调用所有通知监听器；单个监听器抛错不影响其他监听器。
  _emit(message) {
    for (const listener of this.listeners) {
      try {
        listener(message);
      } catch (error) {
        console.error('[rpc] listener failed', error);
      }
    }
  }

  // 发送请求并等待同 id 的响应。默认 15 秒超时。
  request(method, params = {}, timeoutMs = 15000) {
    if (!this.proc || this.proc.killed) {
      return Promise.reject(new Error('player-core is not running'));
    }

    const id = this.nextId++;
    const payload = Buffer.from(
      JSON.stringify({ jsonrpc: '2.0', id, method, params }),
      'utf8'
    );
    const frame = Buffer.concat([
      Buffer.from(`Content-Length: ${payload.length}\r\n\r\n`, 'ascii'),
      payload,
    ]);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`RPC timeout: ${method}`));
      }, timeoutMs);

      this.pending.set(id, { resolve, reject, timer });
      this.proc.stdin.write(frame, (error) => {
        if (error) {
          clearTimeout(timer);
          this.pending.delete(id);
          reject(error);
        }
      });
    });
  }

  // 注册通知监听器，返回取消订阅函数。
  onNotification(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  // 让所有未完成请求失败（进程退出/启动失败时）。
  _failAll(error) {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }

  // 关闭 stdin 并终止子进程。
  stop() {
    if (!this.proc) return;
    try {
      this.proc.stdin.end();
    } catch (error) {
      // ignore
    }
    try {
      this.proc.kill();
    } catch (error) {
      // ignore
    }
    this.proc = null;
  }
}

module.exports = { RpcClient };
