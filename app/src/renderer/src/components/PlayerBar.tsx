// =============================================================================
// PlayerBar.tsx - 底部播放器
//
// 进度条拖动的关键设计：
//   dragMs    正在拖动的值（受控 range 的 value）
//   pendingMs 已提交 seek、等待核心状态追上的目标值
//   draggingRef/dragRef 用 ref 保存同步值，避免 pointerup 时读到旧 state
//
// 之前的 bug：指针在滑块外释放时 input 收不到 pointerup，dragMs 永远不提交，
// 进度条停在拖动位置而音频继续播放。现在在 window 上监听 pointerup/
// pointercancel，并配合 setPointerCapture，保证释放一定会被捕获。
// =============================================================================

import { useCallback, useEffect, useRef, useState } from 'react';
import type { CSSProperties } from 'react';
import type { PlayerState, Track } from '../types';
import { formatDuration } from '../utils';
import { Art } from './Art';
import { Icon } from './Icon';

interface PlayerBarProps {
  track: Track | null;       // 当前曲目元数据（可能暂时为 null）
  state: PlayerState;        // 核心推送的播放状态
  queueOpen: boolean;
  onToggle: () => void;
  onPrev: () => void;
  onNext: () => void;
  onSeek: (positionMs: number) => void;
  onVolume: (volume: number) => void;
  onToggleQueue: () => void;
}

export function PlayerBar({
  track,
  state,
  queueOpen,
  onToggle,
  onPrev,
  onNext,
  onSeek,
  onVolume,
  onToggleQueue,
}: PlayerBarProps) {
  const [dragMs, setDragMs] = useState<number | null>(null);
  const [pendingMs, setPendingMs] = useState<number | null>(null);
  const dragRef = useRef<number | null>(null);      // 拖动值（同步）
  const draggingRef = useRef(false);               // 是否处于拖动中
  const duration = state.durationMs || track?.durationMs || 0;
  // 显示优先级：正在拖 > 等待 seek 生效 > 核心实时位置
  const rawPosition = dragMs ?? pendingMs ?? state.positionMs;
  const position = duration > 0 ? Math.min(rawPosition, duration) : rawPosition;
  const progress = duration > 0 ? Math.min(100, (position / duration) * 100) : 0;
  const volume = Math.round(state.volume * 100);

  // 切歌或时长变化时，清掉拖动/等待状态，避免把旧位置带到新歌。
  useEffect(() => {
    dragRef.current = null;
    draggingRef.current = false;
    setDragMs(null);
    setPendingMs(null);
  }, [state.trackId, duration]);

  // pendingMs 的作用：seek 请求发出后，核心状态可能还要几百毫秒才更新。
  // 这段时间继续显示目标位置，避免进度条先跳回旧位置再跳过去。
  // 当实时位置接近目标（或 1.5 秒后）就交还给实时状态。
  useEffect(() => {
    if (pendingMs === null) return;
    if (Math.abs(state.positionMs - pendingMs) < 1500) {
      setPendingMs(null);
      return;
    }
    const timer = window.setTimeout(() => setPendingMs(null), 1500);
    return () => window.clearTimeout(timer);
  }, [pendingMs, state.positionMs]);

  // range 的 onChange：同时写 ref（同步）和 state（触发渲染）。
  const updateDrag = (value: number) => {
    dragRef.current = value;
    setDragMs(value);
  };

  // 指针释放：提交拖动值。由 window 的 pointerup/pointercancel 调用，
  // 因此即使指针在滑块外释放也能提交。draggingRef 防止重复提交。
  const finishDrag = useCallback(() => {
    if (!draggingRef.current) return;
    draggingRef.current = false;
    const value = dragRef.current;
    dragRef.current = null;
    setDragMs(null);
    if (value !== null) {
      setPendingMs(value);
      onSeek(value);
    }
  }, [onSeek]);

  // 键盘调整（方向键/Home/End）后提交。
  const commitKeyboard = () => {
    const value = dragRef.current;
    dragRef.current = null;
    setDragMs(null);
    if (value !== null) {
      setPendingMs(value);
      onSeek(value);
    }
  };

  // 全局监听指针释放：这是修复“滑块外释放导致进度条卡住”的关键。
  useEffect(() => {
    const onPointerEnd = () => finishDrag();
    window.addEventListener('pointerup', onPointerEnd);
    window.addEventListener('pointercancel', onPointerEnd);
    return () => {
      window.removeEventListener('pointerup', onPointerEnd);
      window.removeEventListener('pointercancel', onPointerEnd);
    };
  }, [finishDrag]);

  const seekStyle = { '--progress': `${progress}%` } as CSSProperties;
  const volumeStyle = { '--vol': `${volume}%` } as CSSProperties;

  return (
    <footer className="player" aria-label="播放控制">
      {/* 左侧：当前曲目信息 */}
      <div className="now-playing">
        <span className="np-art">
          <Art seed={track ? `${track.album} ${track.artist}` : 'nightfall'} />
        </span>
        <span className="np-meta">
          <span className="np-title">{track ? track.title : '未在播放'}</span>
          <span className="np-artist">{track ? track.artist || '未知艺人' : '选择一首歌开始'}</span>
        </span>
      </div>

      {/* 中间：播放控制 + 进度条 */}
      <div className="center-console">
        <div className="transport">
          <button className="icon-btn" type="button" onClick={onPrev} aria-label="上一首" title="上一首">
            <Icon name="prev" />
          </button>
          <button
            className="play-btn"
            type="button"
            onClick={onToggle}
            aria-label={state.playing ? '暂停' : '播放'}
            title={state.playing ? '暂停' : '播放'}
          >
            <Icon name={state.playing ? 'pause' : 'play'} />
          </button>
          <button className="icon-btn" type="button" onClick={onNext} aria-label="下一首" title="下一首">
            <Icon name="next" />
          </button>
        </div>

        <div className="progress-row">
          <span className="time">{formatDuration(position)}</span>
          <input
            className="seek"
            type="range"
            min={0}
            max={Math.max(duration, 1)}
            step={100}
            value={Math.min(position, Math.max(duration, 1))}
            style={seekStyle}
            disabled={!track}
            aria-label="播放进度"
            onChange={(event) => updateDrag(Number(event.target.value))}
            // 按下时开启拖动并捕获指针，确保释放事件回到这个元素。
            onPointerDown={(event) => {
              draggingRef.current = true;
              try {
                event.currentTarget.setPointerCapture(event.pointerId);
              } catch (error) {
                // 某些环境不支持 pointer capture，仍有 window 监听兜底。
              }
            }}
            onPointerUp={finishDrag}
            onPointerCancel={finishDrag}
            onLostPointerCapture={finishDrag}
            onBlur={finishDrag}
            onKeyUp={(event) => {
              if (
                ['ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown'].includes(
                  event.key
                )
              ) {
                commitKeyboard();
              }
            }}
          />
          <span className="time">{formatDuration(duration)}</span>
        </div>
      </div>

      {/* 右侧：队列开关 + 音量 */}
      <div className="player-right">
        <button
          className={`icon-btn${queueOpen ? ' is-active' : ''}`}
          type="button"
          onClick={onToggleQueue}
          aria-label="播放队列"
          aria-pressed={queueOpen}
          title="播放队列"
        >
          <Icon name="queue" />
        </button>
        <span className="volume-wrap">
          <button
            className="icon-btn"
            type="button"
            onClick={() => onVolume(state.volume > 0 ? 0 : 0.8)}
            aria-label={state.volume > 0 ? '静音' : '取消静音'}
            title={state.volume > 0 ? '静音' : '取消静音'}
          >
            <Icon name={state.volume > 0 ? 'volume' : 'volume-x'} />
          </button>
          <input
            className="vol"
            type="range"
            min={0}
            max={100}
            step={1}
            value={volume}
            style={volumeStyle}
            aria-label="音量"
            onChange={(event) => onVolume(Number(event.target.value) / 100)}
          />
        </span>
      </div>
    </footer>
  );
}
