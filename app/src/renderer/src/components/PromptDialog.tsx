// =============================================================================
// PromptDialog.tsx - 单行文本输入对话框
//
// 用于新建播放列表、重命名播放列表。
// 回车提交；输入为空时确认按钮禁用；Escape/遮罩点击取消。
// =============================================================================

import { useEffect, useState } from 'react';
import type { FormEvent } from 'react';

interface PromptDialogProps {
  open: boolean;
  title: string;
  label: string;
  placeholder?: string;
  confirmLabel: string;
  cancelLabel?: string;
  initialValue?: string;     // 重命名时预填当前名称
  onConfirm: (value: string) => void;
  onCancel: () => void;
}

export function PromptDialog({
  open,
  title,
  label,
  placeholder,
  confirmLabel,
  cancelLabel = '取消',
  initialValue = '',
  onConfirm,
  onCancel,
}: PromptDialogProps) {
  const [value, setValue] = useState(initialValue);

  // 每次打开都重置为 initialValue（例如切换到另一个播放列表重命名）。
  useEffect(() => {
    if (open) setValue(initialValue);
  }, [open, initialValue]);

  // Escape 取消。
  useEffect(() => {
    if (!open) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onCancel();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onCancel]);

  if (!open) return null;

  // 表单提交：去掉首尾空白，空值不提交。
  const submit = (event: FormEvent) => {
    event.preventDefault();
    const trimmed = value.trim();
    if (trimmed) onConfirm(trimmed);
  };

  return (
    <div
      className="dialog-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onCancel();
      }}
    >
      <form className="dialog" onSubmit={submit} role="dialog" aria-modal="true" aria-label={title}>
        <h3>{title}</h3>
        <label className="dialog-field">
          <span>{label}</span>
          <input
            className="text-input"
            value={value}
            onChange={(event) => setValue(event.target.value)}
            placeholder={placeholder}
            maxLength={120}
            autoFocus
          />
        </label>
        <div className="dialog-actions">
          <button className="ghost-btn dialog-btn" type="button" onClick={onCancel}>
            {cancelLabel}
          </button>
          <button className="pill-btn" type="submit" disabled={!value.trim()}>
            {confirmLabel}
          </button>
        </div>
      </form>
    </div>
  );
}
