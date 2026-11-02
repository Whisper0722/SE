// =============================================================================
// Icon.tsx - 统一图标组件
//
// 图标全部定义在 index.html 的隐藏 <svg> sprite 中（id 形如 i-play），
// 这里通过 <use href="#i-xxx"> 引用，避免每个组件重复写 SVG path。
// =============================================================================

export function Icon({
  name,
  className = 'icon',
}: {
  name: string;    // 不带 "i-" 前缀，例如 "play"
  className?: string;
}) {
  return (
    <svg className={className} aria-hidden="true" focusable="false">
      <use href={`#i-${name}`} />
    </svg>
  );
}
