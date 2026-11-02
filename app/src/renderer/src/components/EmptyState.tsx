// =============================================================================
// EmptyState.tsx - 通用空状态
//
// 用于：曲库为空、搜索无结果、空播放列表、空专辑等。
// 由调用方决定文案、图标和可选操作按钮。
// =============================================================================

import { Icon } from './Icon';

interface EmptyStateProps {
  icon: string;              // sprite 图标名
  title: string;
  description: string;
  actionLabel?: string;      // 有 actionLabel + onAction 时才显示按钮
  actionIcon?: string;
  onAction?: () => void;
}

export function EmptyState({
  icon,
  title,
  description,
  actionLabel,
  actionIcon,
  onAction,
}: EmptyStateProps) {
  return (
    <div className="empty">
      <span className="empty-icon">
        <Icon name={icon} />
      </span>
      <h3>{title}</h3>
      <p>{description}</p>
      {actionLabel && onAction ? (
        <button className="pill-btn" type="button" onClick={onAction}>
          <Icon name={actionIcon ?? (icon === 'search' ? 'close' : 'plus')} />
          {actionLabel}
        </button>
      ) : null}
    </div>
  );
}
