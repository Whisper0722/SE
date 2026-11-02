// =============================================================================
// App.tsx - 前端根组件（状态中心）
//
// 这里是整个渲染层最“重”的文件，职责是：
//   1. 维护当前视图（全部/文件夹/专辑/播放列表）与曲目列表
//   2. 通过 api.request 调核心 RPC，通过 api.onEvent 接收推送
//   3. 把状态和回调分发给 Sidebar/TrackTable/PlayerBar/QueuePanel 等纯组件
//
// 视图模型 View：一次只显示一种内容；搜索是对当前视图的过滤。
// 播放状态由核心推送，App 只做镜像；用户操作（播放/暂停/seek）发出后
// 用返回的快照或后续通知更新，不在前端自己算播放进度。
// =============================================================================

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties } from 'react';
import { api, errorMessage } from './api';
import { AddTracksDialog } from './components/AddTracksDialog';
import { AlbumGrid } from './components/AlbumGrid';
import { ConfirmDialog } from './components/ConfirmDialog';
import { EmptyState } from './components/EmptyState';
import { Icon } from './components/Icon';
import { PlayerBar } from './components/PlayerBar';
import { PromptDialog } from './components/PromptDialog';
import { QueuePanel } from './components/QueuePanel';
import { Sidebar } from './components/Sidebar';
import { TrackTable } from './components/TrackTable';
import type {
  AlbumInfo,
  AlbumsResponse,
  AppInfo,
  CoreEvent,
  LibraryPage,
  LibraryStats,
  PlaylistInfo,
  PlaylistsResponse,
  PlaylistTracksResponse,
  PlayerState,
  RootsResponse,
  ScanProgress,
  Track,
} from './types';
import { artVars, basename, greeting } from './utils';

// 当前页面视图。album 视图带 artist 只是为了显示上下文，后端过滤只用专辑名。
type View =
  | { kind: 'all' }
  | { kind: 'root'; root: string }
  | { kind: 'albums' }
  | { kind: 'album'; album: string; artist: string }
  | { kind: 'playlist'; id: number; name: string };

// 核心还没返回状态时使用的占位快照。
const EMPTY_PLAYER: PlayerState = {
  trackId: 0,
  playing: false,
  paused: true,
  positionMs: 0,
  durationMs: 0,
  volume: 0.8,
  queue: [],
  queueIndex: 0,
  audioMode: '',
  error: '',
};

export default function App() {
  // ---- 视图与列表数据 ----
  const [view, setView] = useState<View>({ kind: 'all' });
  const [tracks, setTracks] = useState<Track[]>([]);
  const [albums, setAlbums] = useState<AlbumInfo[]>([]);
  const [playlists, setPlaylists] = useState<PlaylistInfo[]>([]);
  const [total, setTotal] = useState(0);
  const [stats, setStats] = useState<LibraryStats>({ tracks: 0, albums: 0 });
  const [roots, setRoots] = useState<string[]>([]);
  // searchInput 是输入框的即时值；activeQuery 是防抖后真正提交给后端的查询。
  const [searchInput, setSearchInput] = useState('');
  const [activeQuery, setActiveQuery] = useState('');

  // ---- 播放器状态（核心推送的镜像）----
  const [player, setPlayer] = useState<PlayerState>(EMPTY_PLAYER);
  const [nowPlaying, setNowPlaying] = useState<Track | null>(null);
  const [queueTracks, setQueueTracks] = useState<Track[]>([]);
  // ---- 扫描进度与 UI 开关 ----
  const [scan, setScan] = useState({ active: false, scanned: 0, total: 0 });
  const [showQueue, setShowQueue] = useState(true);
  const [theme, setTheme] = useState<'dark' | 'light'>(() => {
    try {
      return localStorage.getItem('nightfall-theme') === 'light' ? 'light' : 'dark';
    } catch (error) {
      return 'dark';
    }
  });
  const [info, setInfo] = useState<AppInfo | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  // ---- 对话框状态：有值则打开 ----
  const [rootToRemove, setRootToRemove] = useState<string | null>(null);
  const [playlistToDelete, setPlaylistToDelete] = useState<PlaylistInfo | null>(null);
  const [playlistToRename, setPlaylistToRename] = useState<PlaylistInfo | null>(null);
  const [createPlaylistOpen, setCreatePlaylistOpen] = useState(false);
  const [addTracksFor, setAddTracksFor] = useState<PlaylistInfo | null>(null);

  // ---- refs ----
  // queryRef/viewRef 保存最新值，供异步回调和事件处理器读取（避免闭包旧值）。
  // trackCache 避免重复请求同一首曲目的元数据。
  // playerRef/tracksRef 供键盘快捷键和队列操作同步读取。
  const searchRef = useRef<HTMLInputElement>(null);
  const contentRef = useRef<HTMLDivElement>(null);
  const queryRef = useRef('');
  const viewRef = useRef<View>({ kind: 'all' });
  const trackCache = useRef(new Map<number, Track>());
  const tracksRef = useRef<Track[]>([]);
  const playerRef = useRef<PlayerState>(EMPTY_PLAYER);
  const nowPlayingIdRef = useRef(0);

  const greetingText = useMemo(() => greeting(), []);

  // 每次曲目列表变化：同步 tracksRef 并填充曲目元数据缓存。
  useEffect(() => {
    tracksRef.current = tracks;
    for (const track of tracks) trackCache.current.set(track.id, track);
  }, [tracks]);

  // 同步播放器快照，供不依赖 React 渲染的键盘处理器使用。
  useEffect(() => {
    playerRef.current = player;
  }, [player]);

  // 加载当前视图内容。
  // albums：取专辑聚合，搜索在客户端过滤；
  // playlist：取播放列表曲目，搜索在客户端过滤；
  // all/root/album：走 library.page，搜索和过滤都在 SQLite 完成。
  const loadView = useCallback(async (nextView: View, query: string) => {
    try {
      if (nextView.kind === 'albums') {
        const response = await api.request<AlbumsResponse>('library.albums');
        const q = query.trim().toLowerCase();
        const filtered = q
          ? response.albums.filter((album) =>
              `${album.album} ${album.artist}`.toLowerCase().includes(q)
            )
          : response.albums;
        setAlbums(filtered);
        setTracks([]);
        setTotal(filtered.length);
        setLoading(false);
        return;
      }

      if (nextView.kind === 'playlist') {
        const response = await api.request<PlaylistTracksResponse>('playlists.tracks', {
          id: nextView.id,
        });
        const q = query.trim().toLowerCase();
        const filtered = q
          ? response.tracks.filter((track) =>
              `${track.title} ${track.artist} ${track.album}`.toLowerCase().includes(q)
            )
          : response.tracks;
        for (const track of filtered) trackCache.current.set(track.id, track);
        setTracks(filtered);
        setTotal(filtered.length);
        setLoading(false);
        return;
      }

      const page = await api.request<LibraryPage>('library.page', {
        offset: 0,
        limit: 500,
        query,
        root: nextView.kind === 'root' ? nextView.root : '',
        album: nextView.kind === 'album' ? nextView.album : '',
      });
      for (const track of page.tracks) trackCache.current.set(track.id, track);
      setTracks(page.tracks);
      setTotal(page.total);
      setLoading(false);
    } catch (err) {
      setError(errorMessage(err));
      setLoading(false);
    }
  }, []);

  // 刷新侧边栏元信息：曲库统计、根目录、播放列表。
  // 与 loadView 分开，因为切视图不一定需要重新拉这些数据。
  const refreshMeta = useCallback(async () => {
    try {
      const [nextStats, rootResponse, playlistResponse] = await Promise.all([
        api.request<LibraryStats>('library.stats'),
        api.request<RootsResponse>('library.roots'),
        api.request<PlaylistsResponse>('playlists.list'),
      ]);
      setStats(nextStats);
      setRoots(rootResponse.roots);
      setPlaylists(playlistResponse.playlists);
    } catch (err) {
      setError(errorMessage(err));
    }
  }, []);

  // 重新加载“当前视图 + 元信息”。
  // library.changed（扫描完成/目录移除）和播放列表变更后都用它。
  const reloadCurrent = useCallback(async () => {
    await Promise.all([loadView(viewRef.current, queryRef.current), refreshMeta()]);
  }, [loadView, refreshMeta]);

  // 按 id 取曲目元数据，优先用缓存。
  // 自动切歌时前端只有 trackId，需要靠它补全标题/艺人/封面。
  const loadTrack = useCallback(async (id: number): Promise<Track | null> => {
    const cached = trackCache.current.get(id);
    if (cached) return cached;
    try {
      const track = await api.request<Track>('library.track', { id });
      trackCache.current.set(track.id, track);
      return track;
    } catch (err) {
      return null;
    }
  }, []);

  // 应用一份播放快照（来自 RPC 响应或 playback.state 通知）。
  // trackId 变化时异步补全 nowPlaying 元数据；这是自动切歌更新的关键。
  const applyState = useCallback(
    (next: PlayerState) => {
      setPlayer(next);
      playerRef.current = next;
      if (next.trackId && next.trackId !== nowPlayingIdRef.current) {
        nowPlayingIdRef.current = next.trackId;
        void loadTrack(next.trackId).then((track) => {
          if (track) setNowPlaying(track);
        });
      } else if (!next.trackId) {
        nowPlayingIdRef.current = 0;
        setNowPlaying(null);
      }
      if (next.error) setError(next.error);
    },
    [loadTrack]
  );

  // 核心通知总入口：
  //   playback.state -> 更新播放器
  //   scan.progress  -> 更新扫描进度
  //   scan.error     -> 提示错误
  //   library.changed-> 重新加载当前视图与侧边栏
  //   core.exit      -> 核心退出提示
  const onCoreEvent = useCallback(
    (event: CoreEvent) => {
      if (event.method === 'playback.state' && event.params) {
        applyState(event.params as unknown as PlayerState);
      } else if (event.method === 'scan.progress' && event.params) {
        const progress = event.params as unknown as ScanProgress;
        setScan({ active: true, scanned: progress.scanned, total: progress.total });
      } else if (event.method === 'scan.error') {
        const message = (event.params as { message?: string } | undefined)?.message;
        if (message) setError(message);
        setScan((previous) => ({ ...previous, active: false }));
      } else if (event.method === 'library.changed') {
        setScan((previous) => ({ ...previous, active: false }));
        void reloadCurrent();
      } else if (event.method === 'core.exit') {
        setError('音频核心已退出。请检查 player-core 是否已构建并可运行。');
      }
    },
    [applyState, reloadCurrent]
  );

  // 订阅核心通知；onCoreEvent 变化时重新订阅（其依赖都是稳定 useCallback）。
  useEffect(() => {
    const unsubscribe = api.onEvent(onCoreEvent);
    return unsubscribe;
  }, [onCoreEvent]);

  // 首次挂载：拿启动信息 -> 加载全部歌曲和侧边栏 -> 拉一次播放状态。
  // coreError 非空说明核心没起来（例如缺 DLL），界面会显示错误面板。
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const appInfo = await api.getInfo();
        if (cancelled) return;
        setInfo(appInfo);
        if (appInfo.coreError) {
          setError(appInfo.coreError);
          setLoading(false);
          return;
        }
        await Promise.all([loadView({ kind: 'all' }, ''), refreshMeta()]);
        const state = await api.request<PlayerState>('playback.state');
        if (!cancelled) applyState(state);
      } catch (err) {
        if (!cancelled) {
          setError(errorMessage(err));
          setLoading(false);
        }
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [applyState, loadView, refreshMeta]);

  // 搜索防抖：输入停止 220ms 后才请求，避免每个按键都打一次 RPC。
  useEffect(() => {
    const timer = window.setTimeout(() => {
      if (searchInput === queryRef.current) return;
      queryRef.current = searchInput;
      setActiveQuery(searchInput);
      void loadView(viewRef.current, searchInput);
    }, 220);
    return () => window.clearTimeout(timer);
  }, [searchInput, loadView]);

  // 主题：写到 <html data-theme> 并持久化到 localStorage。
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    try {
      localStorage.setItem('nightfall-theme', theme);
    } catch (err) {
      // localStorage may be unavailable.
    }
  }, [theme]);

  const queueKey = player.queue.join(',');

  // 把核心队列的 id 映射成 Track 对象，供 QueuePanel 显示。
  // 先用缓存立即渲染；缺失的 id 再逐个请求（最多 50 个，避免一次打太多）。
  useEffect(() => {
    const ids = player.queue;
    if (!ids.length) {
      setQueueTracks([]);
      return;
    }

    const cached = ids
      .map((id) => trackCache.current.get(id))
      .filter((track): track is Track => Boolean(track));
    setQueueTracks(cached);

    const missing = ids.filter((id) => !trackCache.current.has(id)).slice(0, 50);
    if (!missing.length) return;

    let cancelled = false;
    (async () => {
      for (const id of missing) {
        try {
          const track = await api.request<Track>('library.track', { id });
          trackCache.current.set(track.id, track);
        } catch (err) {
          // A queue entry may have been removed from the library.
        }
      }
      if (!cancelled) {
        setQueueTracks(
          ids
            .map((id) => trackCache.current.get(id))
            .filter((track): track is Track => Boolean(track))
        );
      }
    })();

    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [queueKey]);

  // 错误提示 7 秒后自动消失。
  useEffect(() => {
    if (!error) return;
    const timer = window.setTimeout(() => setError(null), 7000);
    return () => window.clearTimeout(timer);
  }, [error]);

  // 播放一首歌。
  // keepQueue=false：把当前视图的曲目列表设为队列（表格/专辑/播放列表点击）。
  // keepQueue=true ：保留核心已有队列（队列面板点击，不重置上下文）。
  const playTrack = useCallback(
    async (track: Track, keepQueue = false) => {
      try {
        trackCache.current.set(track.id, track);
        setNowPlaying(track);
        nowPlayingIdRef.current = track.id;

        let ids: number[];
        if (keepQueue && playerRef.current.queue.includes(track.id)) {
          ids = playerRef.current.queue;
        } else {
          ids = tracksRef.current.map((item) => item.id);
          if (!ids.includes(track.id)) ids = [...ids, track.id];
          await api.request('queue.set', { ids });
        }

        const state = await api.request<PlayerState>('playback.play', {
          trackId: track.id,
        });
        applyState(state);
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [applyState]
  );

  // 表格点击：以当前列表为队列开始播放。
  const playFromLibrary = useCallback(
    (track: Track) => {
      void playTrack(track, false);
    },
    [playTrack]
  );

  // 队列点击：保留队列，只切换当前曲目。
  const playFromQueue = useCallback(
    (track: Track) => {
      void playTrack(track, true);
    },
    [playTrack]
  );

  // 播放全部：从当前视图第一首开始，队列即当前曲目列表。
  const playAll = useCallback(() => {
    const first = tracksRef.current[0];
    if (first) void playTrack(first, false);
  }, [playTrack]);

  // 播放/暂停；没有当前曲目时播放列表第一首。
  const togglePlay = useCallback(async () => {
    try {
      if (!playerRef.current.trackId) {
        const first = tracksRef.current[0];
        if (first) await playTrack(first, false);
        return;
      }
      applyState(await api.request<PlayerState>('playback.toggle'));
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [applyState, playTrack]);

  // 下一首 / 上一首：交给核心处理队列。
  const next = useCallback(async () => {
    try {
      applyState(await api.request<PlayerState>('playback.next'));
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [applyState]);

  const prev = useCallback(async () => {
    try {
      applyState(await api.request<PlayerState>('playback.prev'));
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [applyState]);

  // 跳转：先按当前时长裁剪，再发给核心；核心返回的 snapshot 会立即更新进度。
  const seek = useCallback(
    async (positionMs: number) => {
      try {
        const duration = playerRef.current.durationMs;
        const clamped =
          duration > 0
            ? Math.max(0, Math.min(positionMs, duration))
            : Math.max(0, positionMs);
        applyState(await api.request<PlayerState>('playback.seek', { positionMs: clamped }));
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [applyState]
  );

  // 音量：0.0 ~ 1.0。
  const setVolume = useCallback(
    async (volume: number) => {
      try {
        applyState(await api.request<PlayerState>('playback.setVolume', { volume }));
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [applyState]
  );

  // 切换视图的统一入口：更新 viewRef/state、清搜索、加载内容、滚回顶部。
  const navigate = useCallback(
    (nextView: View) => {
      viewRef.current = nextView;
      setView(nextView);
      setSearchInput('');
      queryRef.current = '';
      setActiveQuery('');
      setLoading(true);
      void loadView(nextView, '');
      contentRef.current?.scrollTo({ top: 0, behavior: 'smooth' });
    },
    [loadView]
  );

  // 各视图的快捷跳转。
  const goHome = useCallback(() => navigate({ kind: 'all' }), [navigate]);
  const selectRoot = useCallback(
    (root: string) => navigate({ kind: 'root', root }),
    [navigate]
  );
  const openAlbums = useCallback(() => navigate({ kind: 'albums' }), [navigate]);
  const openAlbum = useCallback(
    (album: AlbumInfo) => navigate({ kind: 'album', album: album.album, artist: album.artist }),
    [navigate]
  );
  const openPlaylist = useCallback(
    (playlist: PlaylistInfo) =>
      navigate({ kind: 'playlist', id: playlist.id, name: playlist.name }),
    [navigate]
  );

  // 添加文件夹：选择目录 -> 核心入库并扫描 -> 刷新侧边栏 -> 打开该文件夹视图。
  // 用 normalize 做大小写/斜杠归一化，找到核心规范化后的根目录路径。
  const addFolder = useCallback(async () => {
    try {
      const folder = await api.pickFolder();
      if (!folder) return;
      setScan({ active: true, scanned: 0, total: 0 });
      await api.request('library.addRoot', { path: folder });

      const { roots: nextRoots } = await api.request<RootsResponse>('library.roots');
      const normalize = (value: string) => value.replace(/\//g, '\\').toLowerCase();
      const stored = nextRoots.find((root) => normalize(root) === normalize(folder)) ?? null;
      await refreshMeta();
      if (stored) navigate({ kind: 'root', root: stored });
      else navigate({ kind: 'all' });
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [navigate, refreshMeta]);

  // 手动重新扫描所有根目录（增量扫描）。
  const rescan = useCallback(async () => {
    try {
      setScan((previous) => ({ ...previous, active: true }));
      await api.request('library.scan');
    } catch (err) {
      setError(errorMessage(err));
    }
  }, []);

  // 确认移除文件夹：只删索引，不动磁盘文件。
  // 如果当前就在该文件夹视图，先切回“全部歌曲”，再刷新。
  const confirmRemoveRoot = useCallback(async () => {
    if (!rootToRemove) return;
    const path = rootToRemove;
    setRootToRemove(null);

    if (viewRef.current.kind === 'root' && viewRef.current.root === path) {
      viewRef.current = { kind: 'all' };
      setView({ kind: 'all' });
    }

    try {
      await api.request('library.removeRoot', { path });
      await Promise.all([loadView(viewRef.current, queryRef.current), refreshMeta()]);
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [loadView, refreshMeta, rootToRemove]);

  // 新建播放列表后直接进入该列表。
  const createPlaylist = useCallback(
    async (name: string) => {
      setCreatePlaylistOpen(false);
      try {
        const created = await api.request<{ id: number; name: string }>('playlists.create', {
          name,
        });
        await refreshMeta();
        navigate({ kind: 'playlist', id: created.id, name: created.name });
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [navigate, refreshMeta]
  );

  // 删除播放列表。如果正在查看它，切回全部歌曲；否则保持当前视图。
  const confirmDeletePlaylist = useCallback(async () => {
    if (!playlistToDelete) return;
    const playlist = playlistToDelete;
    setPlaylistToDelete(null);
    try {
      await api.request('playlists.delete', { id: playlist.id });
      if (viewRef.current.kind === 'playlist' && viewRef.current.id === playlist.id) {
        viewRef.current = { kind: 'all' };
        setView({ kind: 'all' });
      }
      await Promise.all([loadView(viewRef.current, queryRef.current), refreshMeta()]);
    } catch (err) {
      setError(errorMessage(err));
    }
  }, [loadView, playlistToDelete, refreshMeta]);

  // 重命名播放列表；同步更新当前 view 中的 name 和侧边栏。
  const renamePlaylist = useCallback(
    async (name: string) => {
      if (!playlistToRename) return;
      const playlist = playlistToRename;
      setPlaylistToRename(null);
      try {
        await api.request('playlists.rename', { id: playlist.id, name });
        if (viewRef.current.kind === 'playlist' && viewRef.current.id === playlist.id) {
          const next = { ...viewRef.current, name };
          viewRef.current = next;
          setView(next);
        }
        await refreshMeta();
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [playlistToRename, refreshMeta]
  );

  // 拖拽排序：先乐观更新 UI，再把完整 id 顺序发给核心持久化。
  // 只在播放列表视图且没有搜索过滤时允许（否则 tracks 只是子集，顺序会错）。
  // 失败时重新加载当前视图，恢复到服务器状态。
  const reorderPlaylist = useCallback(
    async (from: number, to: number) => {
      if (viewRef.current.kind !== 'playlist') return;
      const current = tracksRef.current;
      if (
        from === to ||
        from < 0 ||
        to < 0 ||
        from >= current.length ||
        to >= current.length
      ) {
        return;
      }

      const next = [...current];
      const [moved] = next.splice(from, 1);
      next.splice(to, 0, moved);
      tracksRef.current = next;
      setTracks(next);

      try {
        await api.request('playlists.reorder', {
          id: viewRef.current.id,
          trackIds: next.map((track) => track.id),
        });
      } catch (err) {
        setError(errorMessage(err));
        await loadView(viewRef.current, queryRef.current);
      }
    },
    [loadView]
  );

  // 从播放列表移除一首歌，然后刷新列表和侧边栏计数。
  const removeFromPlaylist = useCallback(
    async (track: Track) => {
      if (viewRef.current.kind !== 'playlist') return;
      try {
        await api.request('playlists.removeTrack', {
          playlistId: viewRef.current.id,
          trackId: track.id,
        });
        await Promise.all([loadView(viewRef.current, queryRef.current), refreshMeta()]);
      } catch (err) {
        setError(errorMessage(err));
      }
    },
    [loadView, refreshMeta]
  );

  // 清除搜索，重新加载当前视图。
  const clearSearch = useCallback(() => {
    setSearchInput('');
    queryRef.current = '';
    setActiveQuery('');
    void loadView(viewRef.current, '');
  }, [loadView]);

  // 聚焦搜索框（侧边栏“搜索”和快捷键 “/” 使用）。
  const focusSearch = useCallback(() => {
    searchRef.current?.focus();
    searchRef.current?.select();
  }, []);

  // 全局快捷键：空格播放/暂停、/ 聚焦搜索、q 开关队列、
  // 左右方向键前后跳 5 秒。输入框/滑块/按钮聚焦时不拦截。
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      if (target?.closest('input, textarea, [contenteditable], [role="slider"], button')) {
        return;
      }
      if (event.key === ' ') {
        event.preventDefault();
        void togglePlay();
      } else if (event.key === '/') {
        event.preventDefault();
        focusSearch();
      } else if (event.key === 'q' || event.key === 'Q') {
        setShowQueue((open) => !open);
      } else if (event.key === 'ArrowRight') {
        void seek(playerRef.current.positionMs + 5000);
      } else if (event.key === 'ArrowLeft') {
        void seek(Math.max(0, playerRef.current.positionMs - 5000));
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [focusSearch, seek, togglePlay]);

  // 当前曲目元数据：优先 nowPlaying，其次队列缓存（应对自动切歌后的短暂空档）。
  const currentTrack =
    nowPlaying ?? queueTracks.find((track) => track.id === player.trackId) ?? null;
  const scanLabel = scan.total > 0 ? `扫描 ${scan.scanned}/${scan.total}` : '扫描中…';
  const themeIcon = theme === 'dark' ? 'sun' : 'moon';
  const nowArt = currentTrack
    ? artVars(`${currentTrack.album} ${currentTrack.artist}`)
    : null;
  const appStyle = nowArt
    ? ({ '--now-a': nowArt.a, '--now-b': nowArt.b } as CSSProperties)
    : undefined;

  // ---- 当前视图的派生状态（给 Sidebar/标题/空状态使用）----
  const activeRoot = view.kind === 'root' ? view.root : null;
  const activePlaylistId = view.kind === 'playlist' ? view.id : null;
  const albumsActive = view.kind === 'albums' || view.kind === 'album';
  const currentPlaylist =
    view.kind === 'playlist'
      ? playlists.find((playlist) => playlist.id === view.id) ?? {
          id: view.id,
          name: view.name,
          trackCount: total,
          createdAt: 0,
        }
      : null;

  // 标题/上下文/搜索占位符/计数/空状态都根据当前 view 决定。
  const viewTitle = activeQuery
    ? '搜索结果'
    : view.kind === 'albums'
      ? '专辑'
      : view.kind === 'album'
        ? view.album
        : view.kind === 'playlist'
          ? currentPlaylist?.name ?? view.name
          : view.kind === 'root'
            ? basename(view.root)
            : '全部歌曲';

  const contextLabel =
    view.kind === 'root'
      ? `当前：${basename(view.root)}`
      : view.kind === 'playlist'
        ? `播放列表：${currentPlaylist?.name ?? view.name}`
        : view.kind === 'album'
          ? `专辑：${view.album}`
          : view.kind === 'albums'
            ? '专辑视图'
            : '';

  const searchPlaceholder =
    view.kind === 'albums'
      ? '搜索专辑或艺人'
      : view.kind === 'playlist'
        ? '搜索当前播放列表'
        : view.kind === 'album'
          ? '搜索这张专辑'
          : view.kind === 'root'
            ? '搜索当前文件夹'
            : '搜索歌曲、艺人或专辑';

  const countLabel = view.kind === 'albums' ? `${total} 张` : `${total} 首`;

  // 空状态文案与动作：搜索无结果、空专辑、空播放列表、空曲库。
  const emptyProps = (() => {
    if (activeQuery) {
      return {
        icon: 'search',
        title: '没有找到匹配的内容',
        description: '换个关键词试试，或者清除搜索。',
        actionLabel: '清除搜索',
        actionIcon: 'close',
        onAction: clearSearch,
      };
    }
    if (view.kind === 'albums') {
      return {
        icon: 'disc',
        title: '还没有专辑',
        description: '添加音乐文件夹并扫描后，专辑会出现在这里。',
        actionLabel: '添加音乐文件夹',
        actionIcon: 'plus',
        onAction: addFolder,
      };
    }
    if (view.kind === 'album') {
      return {
        icon: 'music',
        title: '这张专辑还没有可显示的歌曲',
        description: '曲库可能已经变化，返回专辑列表看看。',
        actionLabel: '返回专辑',
        actionIcon: 'disc',
        onAction: openAlbums,
      };
    }
    if (view.kind === 'playlist') {
      return {
        icon: 'music',
        title: '这个播放列表还是空的',
        description: '从曲库选一些歌曲加进来。',
        actionLabel: '添加歌曲',
        actionIcon: 'plus',
        onAction: () => {
          if (currentPlaylist) setAddTracksFor(currentPlaylist);
        },
      };
    }
    return {
      icon: 'folder',
      title: '曲库还是空的',
      description: '选择一个本地音乐文件夹，Nightfall 会扫描其中的音频文件并建立索引。',
      actionLabel: '添加音乐文件夹',
      actionIcon: 'plus',
      onAction: addFolder,
    };
  })();

  return (
    <div className={`app${showQueue ? '' : ' queue-hidden'}`} style={appStyle}>
      {/* 左侧导航/曲库/播放列表 */}
      <Sidebar
        roots={roots}
        playlists={playlists}
        total={stats.tracks}
        activeRoot={activeRoot}
        activePlaylistId={activePlaylistId}
        albumsActive={albumsActive}
        scanActive={scan.active}
        scanLabel={scanLabel}
        audioMode={info?.coreError ? '未连接' : player.audioMode || info?.audioMode || ''}
        onHome={goHome}
        onSearch={focusSearch}
        onAlbums={openAlbums}
        onAddFolder={addFolder}
        onScan={rescan}
        onSelectRoot={selectRoot}
        onRemoveRoot={(root) => setRootToRemove(root)}
        onCreatePlaylist={() => setCreatePlaylistOpen(true)}
        onSelectPlaylist={openPlaylist}
        onDeletePlaylist={(playlist) => setPlaylistToDelete(playlist)}
      />

      {/* 主内容区：顶栏 + 滚动内容 */}
      <main className="main panel">
        <header className="topbar">
          <div className="topbar-brand">
            <span className="brand-mark small">
              <Icon name="music" />
            </span>
            <span>Nightfall</span>
          </div>

          <label className="search">
            <Icon name="search" />
            <input
              ref={searchRef}
              value={searchInput}
              onChange={(event) => setSearchInput(event.target.value)}
              placeholder={searchPlaceholder}
              aria-label={searchPlaceholder}
              spellCheck={false}
            />
            <kbd>/</kbd>
          </label>

          <div className="topbar-actions">
            <button
              className="icon-btn"
              type="button"
              onClick={rescan}
              disabled={scan.active}
              aria-label="重新扫描曲库"
              title="重新扫描"
            >
              <Icon name="refresh" />
            </button>
            <button
              className="icon-btn"
              type="button"
              onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}
              aria-label={theme === 'dark' ? '切换到浅色主题' : '切换到深色主题'}
              title="切换主题"
            >
              <Icon name={themeIcon} />
            </button>
            <button
              className={`icon-btn${showQueue ? ' is-active' : ''}`}
              type="button"
              onClick={() => setShowQueue((open) => !open)}
              aria-label="播放队列"
              aria-pressed={showQueue}
              title="播放队列"
            >
              <Icon name="queue" />
            </button>
          </div>
        </header>

        <div className="content" ref={contentRef}>
          {/* 问候语、曲库统计、扫描状态 */}
          <section className="hero">
            <p className="eyebrow">本地曲库 · 离线可用</p>
            <h1>{greetingText}</h1>
            <p className="sub">
              {stats.tracks} 首歌曲 · {stats.albums} 张专辑
              {contextLabel ? ` · ${contextLabel}` : ''}
              {activeQuery ? ` · 搜索 “${activeQuery}”` : ''}
            </p>
            <div className="hero-actions">
              <button className="pill-btn" type="button" onClick={addFolder}>
                <Icon name="plus" />
                添加音乐文件夹
              </button>
              {scan.active ? (
                <span className="scan-status">
                  <span className="spinner" aria-hidden="true" />
                  {scan.total ? `正在扫描 ${scan.scanned}/${scan.total}` : '正在扫描…'}
                </span>
              ) : null}
            </div>
          </section>

          {/* 视图标题：专辑详情带“← 专辑”返回 */}
          <div className="section-head">
            <div className="section-title">
              {view.kind === 'album' ? (
                <button className="back-link" type="button" onClick={openAlbums}>
                  ← 专辑
                </button>
              ) : null}
              <h2>{viewTitle}</h2>
            </div>
            <span className="count">{countLabel}</span>
          </div>

          {/* 播放列表工具条：播放全部/添加/重命名/删除/拖拽提示 */}
          {view.kind === 'playlist' && currentPlaylist ? (
            <div className="view-actions">
              {tracks.length ? (
                <button className="pill-btn small" type="button" onClick={playAll}>
                  <Icon name="play" />
                  播放全部
                </button>
              ) : null}
              <button
                className="ghost-btn view-btn"
                type="button"
                onClick={() => setAddTracksFor(currentPlaylist)}
              >
                <Icon name="plus" />
                添加歌曲
              </button>
              <button
                className="ghost-btn view-btn"
                type="button"
                onClick={() => setPlaylistToRename(currentPlaylist)}
              >
                <Icon name="edit" />
                重命名
              </button>
              <button
                className="ghost-btn view-btn danger-text"
                type="button"
                onClick={() => setPlaylistToDelete(currentPlaylist)}
              >
                <Icon name="trash" />
                删除播放列表
              </button>
              {!activeQuery ? (
                <span className="view-hint">拖动歌曲排序 · Alt+↑/↓</span>
              ) : null}
            </div>
          ) : view.kind === 'album' && tracks.length ? (
            <div className="view-actions">
              <button className="pill-btn small" type="button" onClick={playAll}>
                <Icon name="play" />
                播放全部
              </button>
            </div>
          ) : null}

          {/* 内容：核心错误 > 加载中 > 专辑网格 > 曲目表 > 空状态 */}
          {info?.coreError ? (
            <div className="fatal">
              <h3>音频核心没有启动</h3>
              <p>{info.coreError}</p>
              <p className="mono">{info.corePath}</p>
            </div>
          ) : loading ? (
            <div className="loading">正在载入曲库…</div>
          ) : view.kind === 'albums' ? (
            albums.length ? (
              <AlbumGrid albums={albums} onOpen={openAlbum} />
            ) : (
              <EmptyState {...emptyProps} />
            )
          ) : tracks.length ? (
            <TrackTable
              tracks={tracks}
              currentId={player.trackId}
              playing={player.playing}
              onPlay={playFromLibrary}
              onRemove={view.kind === 'playlist' ? removeFromPlaylist : undefined}
              onReorder={
                view.kind === 'playlist' && !activeQuery ? reorderPlaylist : undefined
              }
            />
          ) : (
            <EmptyState {...emptyProps} />
          )}
        </div>
      </main>

      {/* 右侧队列 */}
      <QueuePanel
        open={showQueue}
        current={currentTrack}
        queue={queueTracks}
        currentId={player.trackId}
        onClose={() => setShowQueue(false)}
        onPlay={playFromQueue}
      />

      {/* 底部播放器 */}
      <PlayerBar
        track={currentTrack}
        state={player}
        queueOpen={showQueue}
        onToggle={togglePlay}
        onPrev={prev}
        onNext={next}
        onSeek={seek}
        onVolume={setVolume}
        onToggleQueue={() => setShowQueue((open) => !open)}
      />

      {/* 对话框：移除文件夹 / 删除播放列表 / 新建 / 重命名 / 添加歌曲 */}
      <ConfirmDialog
        open={Boolean(rootToRemove)}
        title="移除音乐文件夹？"
        message="只会从曲库移除索引，不会删除磁盘上的文件。"
        path={rootToRemove ?? ''}
        confirmLabel="移除"
        danger
        onConfirm={confirmRemoveRoot}
        onCancel={() => setRootToRemove(null)}
      />

      <ConfirmDialog
        open={Boolean(playlistToDelete)}
        title="删除播放列表？"
        message={`删除「${playlistToDelete?.name ?? ''}」只会移除这个列表，不会删除曲库中的歌曲。`}
        confirmLabel="删除"
        danger
        onConfirm={confirmDeletePlaylist}
        onCancel={() => setPlaylistToDelete(null)}
      />

      <PromptDialog
        open={createPlaylistOpen}
        title="新建播放列表"
        label="名称"
        placeholder="例如：深夜循环"
        confirmLabel="创建"
        onConfirm={(name) => void createPlaylist(name)}
        onCancel={() => setCreatePlaylistOpen(false)}
      />

      <PromptDialog
        open={Boolean(playlistToRename)}
        title="重命名播放列表"
        label="名称"
        placeholder="播放列表名称"
        initialValue={playlistToRename?.name ?? ''}
        confirmLabel="保存"
        onConfirm={(name) => void renamePlaylist(name)}
        onCancel={() => setPlaylistToRename(null)}
      />

      <AddTracksDialog
        open={Boolean(addTracksFor)}
        playlist={addTracksFor}
        onClose={() => setAddTracksFor(null)}
        onChanged={() => void reloadCurrent()}
      />

      {/* 全局错误提示 */}
      {error ? (
        <div className="toast" role="alert">
          <span>{error}</span>
          <button type="button" onClick={() => setError(null)} aria-label="关闭提示">
            <Icon name="close" />
          </button>
        </div>
      ) : null}
    </div>
  );
}
