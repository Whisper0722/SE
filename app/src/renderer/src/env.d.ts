// =============================================================================
// env.d.ts - 给 TypeScript 声明 preload 注入的全局 API
//
// 运行时由 app/src/preload/preload.js 通过 contextBridge 提供。
// =============================================================================

import type { AppInfo, CoreEvent } from './types';

declare global {
  interface Window {
    nightfall: {
      // 调用核心 RPC，返回 result。
      request<T>(method: string, params?: Record<string, unknown>): Promise<T>;
      // 订阅核心通知，返回取消函数。
      onEvent(callback: (event: CoreEvent) => void): () => void;
      // 系统文件夹选择；取消返回 null。
      pickFolder(): Promise<string | null>;
      // 主进程启动信息。
      getInfo(): Promise<AppInfo>;
    };
  }
}

export {};
