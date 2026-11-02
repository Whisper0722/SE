// =============================================================================
// Art.tsx - 程序化专辑封面
//
// 曲库暂时不提取内嵌封面，所以用“专辑+艺人”的哈希选一组渐变色和图案，
// 生成稳定且好看的占位封面。同一个专辑每次渲染颜色一致。
// =============================================================================

import type { CSSProperties } from 'react';
import { artVars } from '../utils';

export function Art({ seed, className = '' }: { seed: string; className?: string }) {
  const { a, b, variant } = artVars(seed);
  // 用 CSS 自定义属性把颜色传给 styles.css 里的 .art 渐变。
  const style = { '--a': a, '--b': b } as CSSProperties;
  return (
    <span
      className={`art v${variant}${className ? ` ${className}` : ''}`}
      style={style}
      aria-hidden="true"
    />
  );
}
