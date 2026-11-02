// =============================================================================
// utils.ts - 纯函数工具
//
// 不依赖 React/Electron，方便单独理解和测试。
// =============================================================================

// 专辑封面使用的渐变色对（深色底 + 亮色点缀）。
const PALETTES: Array<[string, string]> = [
  ['#0d2740', '#22d3a6'],
  ['#33203f', '#ff7a59'],
  ['#0e1626', '#e0b45c'],
  ['#17261a', '#b8e986'],
  ['#092430', '#6ec6e8'],
  ['#2a1919', '#d98b6a'],
  ['#241b3b', '#9b7bff'],
  ['#2b1f12', '#ffb35c'],
  ['#132c2c', '#5ee0c2'],
  ['#2c1a2a', '#f07ab0'],
];

// 毫秒 -> "m:ss"；非法/非正数显示 0:00。
export function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '0:00';
  const total = Math.floor(ms / 1000);
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${minutes}:${String(seconds).padStart(2, '0')}`;
}

// 按当前时间返回问候语。
export function greeting(): string {
  const hour = new Date().getHours();
  if (hour < 5) return '夜深了';
  if (hour < 11) return '早上好';
  if (hour < 18) return '下午好';
  return '晚上好';
}

// 取 Windows/Unix 路径的最后一段；用于侧边栏显示根目录名。
export function basename(path: string): string {
  const parts = path.split(/[\\/]/).filter(Boolean);
  return parts.length ? parts[parts.length - 1] : path;
}

// FNV-1a 哈希：把任意字符串稳定映射成数字。
// 同一个专辑名/艺人名每次都会得到同样的封面颜色，避免界面抖动。
function hashString(input: string): number {
  let hash = 2166136261;
  for (let i = 0; i < input.length; i += 1) {
    hash ^= input.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return Math.abs(hash);
}

// 根据种子生成封面 CSS 变量：--a/--b 颜色与 v0~v3 图案变体。
export function artVars(seed: string): { a: string; b: string; variant: number } {
  const hash = hashString(seed || 'nightfall');
  const palette = PALETTES[hash % PALETTES.length];
  return { a: palette[0], b: palette[1], variant: hash % 4 };
}
