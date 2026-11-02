// =============================================================================
// TrackTable.tsx - 曲目表格
//
// 两种模式：
//   * 普通模式：序号、标题/艺人、专辑、时长，点击行播放
//   * 播放列表模式：额外显示移除按钮，并支持拖拽排序 / Alt+↑↓
//
// 用 React.memo 包裹：App 每 250ms 收到 playback.state 会重渲染，
// 只要 tracks/currentId/playing/回调没变，这里就跳过重渲染。
// =============================================================================

import { memo, useState } from 'react';
import type { KeyboardEvent } from 'react';
import type { Track } from '../types';
import { formatDuration } from '../utils';
import { Art } from './Art';
import { Eq } from './Eq';
import { Icon } from './Icon';

interface TrackTableProps {
  tracks: Track[];
  currentId: number;          // 当前播放曲目 id
  playing: boolean;
  onPlay: (track: Track) => void;
  onRemove?: (track: Track) => void;              // 传入时才显示播放列表移除按钮
  onReorder?: (from: number, to: number) => void;  // 传入时启用拖拽排序
}

// 行键盘支持：Enter/空格播放。
function handleKey(event: KeyboardEvent<HTMLDivElement>, track: Track, onPlay: (track: Track) => void) {
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault();
    onPlay(track);
  }
}

export const TrackTable = memo(function TrackTable({
  tracks,
  currentId,
  playing,
  onPlay,
  onRemove,
  onReorder,
}: TrackTableProps) {
  const hasActions = Boolean(onRemove);
  // 拖拽排序的 UI 状态：正在拖的行、当前悬停的行。
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [overIndex, setOverIndex] = useState<number | null>(null);

  return (
    <div
      className={`track-table${hasActions ? ' has-actions' : ''}${
        onReorder ? ' can-reorder' : ''
      }`}
    >
      <div className="track-head" aria-hidden="true">
        <span>#</span>
        <span>标题</span>
        <span className="col-album">专辑</span>
        <span className="col-dur">时长</span>
        {hasActions ? <span /> : null}
      </div>
      <div className="track-list">
        {tracks.map((track, index) => {
          const isCurrent = track.id === currentId;
          const isPlaying = isCurrent && playing;
          return (
            <div
              key={track.id}
              // is-drop-target 的方向：向下拖显示下划线，向上拖显示上划线。
              className={`track-row${isCurrent ? ' is-current' : ''}${
                isPlaying ? ' is-playing' : ''
              }${dragIndex === index ? ' is-dragging' : ''}${
                overIndex === index && dragIndex !== null && dragIndex !== index
                  ? ` is-drop-target ${dragIndex < index ? 'drop-after' : 'drop-before'}`
                  : ''
              }`}
              role="button"
              tabIndex={0}
              aria-label={`播放 ${track.title}`}
              title={onReorder ? `播放 ${track.title}；拖动排序` : `播放 ${track.title}`}
              // HTML5 拖拽：dragstart 记录源行，dragover 标记目标行，drop 触发排序。
              draggable={Boolean(onReorder)}
              onDragStart={
                onReorder
                  ? (event) => {
                      setDragIndex(index);
                      event.dataTransfer.effectAllowed = 'move';
                      event.dataTransfer.setData('text/plain', String(track.id));
                    }
                  : undefined
              }
              onDragOver={
                onReorder
                  ? (event) => {
                      if (dragIndex === null) return;
                      event.preventDefault();
                      event.dataTransfer.dropEffect = 'move';
                      if (overIndex !== index) setOverIndex(index);
                    }
                  : undefined
              }
              onDrop={
                onReorder
                  ? (event) => {
                      event.preventDefault();
                      const from = dragIndex;
                      setDragIndex(null);
                      setOverIndex(null);
                      if (from !== null && from !== index) onReorder(from, index);
                    }
                  : undefined
              }
              onDragEnd={
                onReorder
                  ? () => {
                      setDragIndex(null);
                      setOverIndex(null);
                    }
                  : undefined
              }
              onClick={() => onPlay(track)}
              onKeyDown={(event) => {
                // 键盘替代：Alt+↑/↓ 把当前行上移/下移一位。
                if (
                  onReorder &&
                  event.altKey &&
                  (event.key === 'ArrowUp' || event.key === 'ArrowDown')
                ) {
                  event.preventDefault();
                  const target = event.key === 'ArrowUp' ? index - 1 : index + 1;
                  if (target >= 0 && target < tracks.length) onReorder(index, target);
                  return;
                }
                handleKey(event, track, onPlay);
              }}
            >
              {/* 序号列：悬停显示播放图标，播放中显示均衡器 */}
              <span className="idx">
                <span className="num">{index + 1}</span>
                <span className="row-play">
                  <Icon name="play" />
                </span>
                {isPlaying ? <Eq /> : null}
              </span>

              {/* 标题 + 艺人（带小封面） */}
              <span className="t-main">
                <span className="t-art">
                  <Art seed={`${track.album} ${track.artist}`} />
                </span>
                <span className="t-text">
                  <span className="t-title">{track.title}</span>
                  <span className="t-artist">{track.artist || '未知艺人'}</span>
                </span>
              </span>

              <span className="t-album col-album">{track.album || '未知专辑'}</span>
              <span className="t-dur col-dur">{formatDuration(track.durationMs)}</span>
              {/* 播放列表模式下的移除按钮；阻止冒泡，避免同时触发播放 */}
              {onRemove ? (
                <button
                  className="row-remove"
                  type="button"
                  aria-label={`从播放列表移除 ${track.title}`}
                  title="从播放列表移除"
                  onClick={(event) => {
                    event.stopPropagation();
                    onRemove(track);
                  }}
                >
                  <Icon name="close" />
                </button>
              ) : null}
            </div>
          );
        })}
      </div>
    </div>
  );
});
