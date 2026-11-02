// =============================================================================
// types.ts - 前端与核心 RPC 的数据契约
//
// 这些字段名必须与 C++ 端 rpc.cpp 的 trackJson/snapshotJson 保持一致。
// 修改任一字段时，两边要同步。
// =============================================================================

// 一首曲目（核心 library.page / library.track 返回）。
export interface Track {
  id: number;
  path: string;
  title: string;
  artist: string;
  album: string;
  codec: string;
  trackNo: number;
  discNo: number;
  durationMs: number;
  sampleRate: number;
  channels: number;
  bitrate: number;
}

// playback.state 通知 / 播放控制接口返回的播放器快照。
export interface PlayerState {
  trackId: number;    // 0 表示当前没有曲目
  playing: boolean;   // 是否处于播放语义（暂停时为 false）
  paused: boolean;
  positionMs: number;
  durationMs: number;
  volume: number;     // 0.0 ~ 1.0
  queue: number[];    // 队列中的曲目 id
  queueIndex: number;
  audioMode: string;  // "wasapi" 或 "null"
  error: string;      // 最近一次播放错误
}

// scan.progress 通知。
export interface ScanProgress {
  root: string;
  scanned: number;
  total: number;
  indexed: number;    // 实际读取元数据/写入数据库的数量
}

// library.page 的分页结果。
export interface LibraryPage {
  total: number;
  offset: number;
  limit: number;
  tracks: Track[];
}

// library.stats。
export interface LibraryStats {
  tracks: number;
  albums: number;
}

// library.albums 的单项。
export interface AlbumInfo {
  album: string;
  artist: string;
  trackCount: number;
  durationMs: number;
}

// playlists.list 的单项。
export interface PlaylistInfo {
  id: number;
  name: string;
  trackCount: number;
  createdAt: number;
}

export interface AlbumsResponse {
  albums: AlbumInfo[];
}

export interface PlaylistsResponse {
  playlists: PlaylistInfo[];
}

// playlists.tracks：播放列表元信息 + 按 position 排序的曲目。
export interface PlaylistTracksResponse {
  playlist: PlaylistInfo;
  tracks: Track[];
}

// library.roots。
export interface RootsResponse {
  roots: string[];
}

// 主进程 app:info 返回的启动信息，用于排错。
export interface AppInfo {
  corePath: string;
  dbPath: string;
  coreError: string | null;
  audioMode: string;
  version: string;
  packaged: boolean;
}

// 主进程转发的一条核心通知。
export interface CoreEvent {
  method: string;
  params?: Record<string, unknown>;
}
