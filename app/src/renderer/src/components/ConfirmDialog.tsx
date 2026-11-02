// =============================================================================
// ConfirmDialog.tsx - 危险操作确认框
//
// 用于“移除音乐文件夹”“删除播放列表”等不可撤销（至少会改数据）的操作。
// 支持 Escape 取消、点击遮罩取消；默认焦点放在“取消”上，避免误按回车。
// =============================================================================

import { useEffect } from 'react';

interface ConfirmDialogProps {
  open: boolean;
  title: string;
  message: string;
  path?: string;             // 可选：以等宽字体显示路径/名称
  confirmLabel: string;
  cancelLabel?: string;
  danger?: boolean;          // true 时确认按钮为红色
  onConfirm: () => void;
  onCancel: () => void;
}

export function ConfirmDialog({
  open,
  title,
  message,
  path,
  confirmLabel,
  cancelLabel = '取消',
  danger = false,
  onConfirm,
  onCancel,
}: ConfirmDialogProps) {
  // 打开时监听 Escape；关闭或卸载时移除监听。
  useEffect(() => {
    if (!open) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onCancel();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onCancel]);

  if (!open) return null;

  return (
    <div
      className="dialog-backdrop"
      onMouseDown={(event) => {
        // 只有点在遮罩本身（不是对话框内部）才取消。
        if (event.target === event.currentTarget) onCancel();
      }}
    >
      <div className="dialog" role="dialog" aria-modal="true" aria-label={title}>
        <h3>{title}</h3>
        <p>{message}</p>
        {path ? <p className="mono dialog-path">{path}</p> : null}
        <div className="dialog-actions">
          <button className="ghost-btn dialog-btn" type="button" onClick={onCancel} autoFocus>
            {cancelLabel}
          </button>
          <button
            className={`pill-btn${danger ? ' danger' : ''}`}
            type="button"
            onClick={onConfirm}
          >
            {confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
