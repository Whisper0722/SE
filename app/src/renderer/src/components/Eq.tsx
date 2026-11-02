// =============================================================================
// Eq.tsx - 正在播放的均衡器动画
//
// 三根小竖条，CSS 动画驱动；paused=true 时冻结动画，
// small=true 用于队列面板里的“正在播放”标签。
// =============================================================================

export function Eq({
  paused = false,
  small = false,
}: {
  paused?: boolean;
  small?: boolean;
}) {
  return (
    <span
      className={`eq${small ? ' small' : ''}${paused ? ' is-paused' : ''}`}
      aria-hidden="true"
    >
      <i />
      <i />
      <i />
    </span>
  );
}
