// =============================================================================
// api.ts - 对 preload 暴露的 window.nightfall 做一层薄封装
//
// 组件不直接调用 window.nightfall，统一走这里，方便类型约束和以后替换实现。
// =============================================================================

import type { AppInfo, CoreEvent } from './types';

export const api = {
  // 调用核心 RPC；泛型 T 是期望的 result 类型。
  request<T>(method: string, params?: Record<string, unknown>): Promise<T> {
    return window.nightfall.request<T>(method, params);
  },

  // 订阅核心通知，返回取消订阅函数。
  onEvent(callback: (event: CoreEvent) => void): () => void {
    return window.nightfall.onEvent(callback);
  },

  // 打开系统文件夹选择器；取消返回 null。
  pickFolder(): Promise<string | null> {
    return window.nightfall.pickFolder();
  },

  // 启动信息（核心路径、数据库路径、错误等）。
  getInfo(): Promise<AppInfo> {
    return window.nightfall.getInfo();
  },
};

// 把 unknown 异常转成可展示的字符串。
export function errorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}
