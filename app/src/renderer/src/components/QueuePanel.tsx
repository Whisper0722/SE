// =============================================================================
// QueuePanel.tsx - 右侧播放队列
//
// 显示当前曲目与接下来的若干首。队列本身由核心维护，
// App 把 queue 中的 id 映射成 Track（queueTracks）后传进来。
// 点击队列项会直接播放（keepQueue=true，不重置队列）。
// =============================================================================

import type { KeyboardEvent } from 'react';
import type { Track } from '../types';
import { formatDuration } from '../utils';
import { Art } from './Art';
import { Icon } from './Icon';

interface QueuePanelProps {
  open: boolean;
  current: Track | null;      // 当前曲目（App 已缓存）
  queue: Track[];             // 队列中所有能找到元数据的曲目
  currentId: number;
  onClose: () => void;
  onPlay: (track: Track) => void;
}

// 队列项键盘支持：Enter/空格播放。
function playOnKey(event: KeyboardEvent<HTMLDivElement>, track: Track, onPlay: (track: Track) => void) {
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault();
    onPlay(track);
  }
}

export function QueuePanel({
  open,
  current,
  queue,
  currentId,
  onClose,
  onPlay,
}: QueuePanelProps) {
  if (!open) return null;

  // 当前曲目可能不在 queue 缓存中（刚切换/元数据还没加载），用 current 兜底。
  const currentTrack = current ?? queue.find((track) => track.id === currentId) ?? null;
  const index = queue.findIndex((track) => track.id === currentId);
  // 只展示接下来 8 首，避免面板过长。
  const upcoming = index >= 0 ? queue.slice(index + 1, index + 9) : queue.slice(0, 8);

  return (
    <aside className="queue-panel panel" aria-label="播放队列">
      <div className="queue-head">
        <h2>播放队列</h2>
        <button className="icon-btn" type="button" onClick={onClose} aria-label="关闭播放队列" title="关闭">
          <Icon name="close" />
        </button>
      </div>

      {/* 正在播放卡片 */}
      {currentTrack ? (
        <div className="queue-now">
          <span className="queue-now-art">
            <Art seed={`${currentTrack.album} ${currentTrack.artist}`} />
          </span>
          <span className="queue-now-text">
            <span className="queue-label">正在播放</span>
            <span className="queue-title">{currentTrack.title}</span>
            <span className="queue-artist">{currentTrack.artist || '未知艺人'}</span>
          </span>
        </div>
      ) : (
        <p className="queue-empty">还没有正在播放的歌曲。</p>
      )}

      <div className="queue-sub">接下来播放</div>
      {/* 队列项：点击切歌；整行可键盘操作 */}
      <div className="queue-list">
        {upcoming.length ? (
          upcoming.map((track) => (
            <div
              className="queue-item"
              key={track.id}
              role="button"
              tabIndex={0}
              aria-label={`播放 ${track.title}`}
              onClick={() => onPlay(track)}
              onKeyDown={(event) => playOnKey(event, track, onPlay)}
            >
              <span className="queue-art">
                <Art seed={`${track.album} ${track.artist}`} />
              </span>
              <span className="queue-text">
                <span className="queue-title">{track.title}</span>
                <span className="queue-artist">{track.artist || '未知艺人'}</span>
              </span>
              <span className="queue-duration">{formatDuration(track.durationMs)}</span>
            </div>
          ))
        ) : (
          <p className="queue-empty">队列已到最后。回到曲库挑一首吧。</p>
        )}
      </div>
    </aside>
  );
}
