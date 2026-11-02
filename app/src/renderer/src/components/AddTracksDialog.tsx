// =============================================================================
// AddTracksDialog.tsx - “添加歌曲到播放列表”对话框
//
// 打开时并行拉取：
//   * 曲库前 200 首（初始列表）
//   * 该播放列表已有曲目（用于把按钮标成“已添加”）
// 搜索框有 220ms 防抖，重新请求 library.page。
// 每次添加成功后 onChanged() 让 App 刷新侧边栏计数和当前播放列表。
// =============================================================================

import { useEffect, useState } from 'react';
import { api, errorMessage } from '../api';
import type { LibraryPage, PlaylistInfo, PlaylistTracksResponse, Track } from '../types';
import { Art } from './Art';
import { Icon } from './Icon';

interface AddTracksDialogProps {
  open: boolean;
  playlist: PlaylistInfo | null;  // 目标播放列表
  onClose: () => void;
  onChanged: () => void;          // 添加成功后通知 App 刷新
}

export function AddTracksDialog({
  open,
  playlist,
  onClose,
  onChanged,
}: AddTracksDialogProps) {
  const playlistId = playlist?.id ?? 0;
  const [query, setQuery] = useState('');
  const [results, setResults] = useState<Track[]>([]);
  // existing：打开时已在列表里的曲目；added：本次会话新加的曲目。
  const [existing, setExisting] = useState<Set<number>>(new Set());
  const [added, setAdded] = useState<Set<number>>(new Set());
  const [loading, setLoading] = useState(false);
  const [busyId, setBusyId] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);

  // 打开时初始化：拉曲库首页 + 播放列表已有曲目。
  // cancelled 标志防止对话框快速关闭后 setState。
  useEffect(() => {
    if (!open || !playlistId) return;
    let cancelled = false;
    setQuery('');
    setAdded(new Set());
    setError(null);
    setLoading(true);

    (async () => {
      try {
        const [page, playlistResponse] = await Promise.all([
          api.request<LibraryPage>('library.page', { offset: 0, limit: 200, query: '' }),
          api.request<PlaylistTracksResponse>('playlists.tracks', { id: playlistId }),
        ]);
        if (cancelled) return;
        setResults(page.tracks);
        setExisting(new Set(playlistResponse.tracks.map((track) => track.id)));
      } catch (err) {
        if (!cancelled) setError(errorMessage(err));
      } finally {
        if (!cancelled) setLoading(false);
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [open, playlistId]);

  // 搜索防抖：停止输入 220ms 后再请求，避免每个字符一次 RPC。
  useEffect(() => {
    if (!open) return;
    const timer = window.setTimeout(async () => {
      setLoading(true);
      try {
        const page = await api.request<LibraryPage>('library.page', {
          offset: 0,
          limit: 200,
          query,
        });
        setResults(page.tracks);
      } catch (err) {
        setError(errorMessage(err));
      } finally {
        setLoading(false);
      }
    }, 220);
    return () => window.clearTimeout(timer);
  }, [query, open]);

  // Escape 关闭。
  useEffect(() => {
    if (!open) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onClose]);

  if (!open || !playlist) return null;

  // 添加一首：核心对重复添加会 IGNORE，这里用 added 集合立即更新按钮状态。
  const add = async (track: Track) => {
    setBusyId(track.id);
    setError(null);
    try {
      await api.request('playlists.addTrack', {
        playlistId: playlist.id,
        trackId: track.id,
      });
      setAdded((previous) => new Set(previous).add(track.id));
      onChanged();
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusyId(null);
    }
  };

  return (
    <div
      className="dialog-backdrop"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onClose();
      }}
    >
      <div
        className="dialog add-tracks-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={`添加歌曲到 ${playlist.name}`}
      >
        {/* 标题区 */}
        <div className="dialog-head">
          <div>
            <h3>添加歌曲</h3>
            <p className="dialog-sub">添加到「{playlist.name}」</p>
          </div>
          <button className="icon-btn" type="button" onClick={onClose} aria-label="关闭">
            <Icon name="close" />
          </button>
        </div>

        <label className="dialog-search">
          <Icon name="search" />
          <input
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="搜索曲库"
            aria-label="搜索曲库"
            spellCheck={false}
            autoFocus
          />
        </label>

        {error ? <p className="dialog-error">{error}</p> : null}

        {/* 搜索结果列表：已添加的按钮禁用 */}
        <div className="add-track-list">
          {loading && !results.length ? (
            <p className="dialog-empty">正在载入…</p>
          ) : results.length ? (
            results.map((track) => {
              const isAdded = existing.has(track.id) || added.has(track.id);
              return (
                <div className="add-track-row" key={track.id}>
                  <span className="add-track-art">
                    <Art seed={`${track.album} ${track.artist}`} />
                  </span>
                  <span className="add-track-text">
                    <span className="queue-title">{track.title}</span>
                    <span className="queue-artist">
                      {track.artist || '未知艺人'} · {track.album || '未知专辑'}
                    </span>
                  </span>
                  <button
                    className={`add-track-btn${isAdded ? ' is-added' : ''}`}
                    type="button"
                    disabled={isAdded || busyId === track.id}
                    onClick={() => void add(track)}
                  >
                    {isAdded ? '已添加' : '添加'}
                  </button>
                </div>
              );
            })
          ) : (
            <p className="dialog-empty">没有找到歌曲。</p>
          )}
        </div>

        <div className="dialog-actions">
          <button className="ghost-btn dialog-btn" type="button" onClick={onClose}>
            完成
          </button>
        </div>
      </div>
    </div>
  );
}
