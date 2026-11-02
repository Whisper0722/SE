// =============================================================================
// Sidebar.tsx - 左侧导航 + 音乐库 + 播放列表
//
// 纯展示组件：不直接调用 API，所有动作（切换视图/增删）通过 props 回调
// 交给 App 处理。activeRoot / activePlaylistId / albumsActive 决定高亮。
// =============================================================================

import type { PlaylistInfo } from '../types';
import { basename } from '../utils';
import { Icon } from './Icon';

interface SidebarProps {
  roots: string[];                 // 已添加的音乐文件夹
  playlists: PlaylistInfo[];
  total: number;                   // 曲库总曲目数
  activeRoot: string | null;       // 当前选中的文件夹（null=全部）
  activePlaylistId: number | null; // 当前选中的播放列表
  albumsActive: boolean;           // 当前是否在专辑视图
  scanActive: boolean;
  scanLabel: string;
  audioMode: string;               // wasapi / null
  onHome: () => void;              // 回到全部歌曲
  onSearch: () => void;            // 聚焦搜索框
  onAlbums: () => void;            // 打开专辑网格
  onAddFolder: () => void;
  onScan: () => void;
  onSelectRoot: (root: string) => void;
  onRemoveRoot: (root: string) => void;
  onCreatePlaylist: () => void;
  onSelectPlaylist: (playlist: PlaylistInfo) => void;
  onDeletePlaylist: (playlist: PlaylistInfo) => void;
}

export function Sidebar({
  roots,
  playlists,
  total,
  activeRoot,
  activePlaylistId,
  albumsActive,
  scanActive,
  scanLabel,
  audioMode,
  onHome,
  onSearch,
  onAlbums,
  onAddFolder,
  onScan,
  onSelectRoot,
  onRemoveRoot,
  onCreatePlaylist,
  onSelectPlaylist,
  onDeletePlaylist,
}: SidebarProps) {
  // 不在任何文件夹/播放列表/专辑视图时，“全部歌曲”高亮。
  const allActive = activeRoot === null && activePlaylistId === null && !albumsActive;

  return (
    <aside className="sidebar">
      {/* 顶部导航：首页 / 搜索 / 专辑 */}
      <div className="side-nav panel">
        <div className="brand">
          <span className="brand-mark">
            <Icon name="music" />
          </span>
          <span className="brand-name">Nightfall</span>
        </div>
        <button
          className={`nav-item${allActive ? ' is-active' : ''}`}
          type="button"
          data-nav="home"
          onClick={onHome}
        >
          <Icon name="home" />
          <span>首页</span>
        </button>
        <button className="nav-item" type="button" data-nav="search" onClick={onSearch}>
          <Icon name="search" />
          <span>搜索</span>
        </button>
        <button
          className={`nav-item${albumsActive ? ' is-active' : ''}`}
          type="button"
          data-nav="albums"
          onClick={onAlbums}
        >
          <Icon name="disc" />
          <span>专辑</span>
        </button>
      </div>

      {/* 音乐库面板：根目录 + 播放列表，中间区域独立滚动 */}
      <div className="side-library panel">
        <div className="side-scroll">
          <div className="panel-head">
            <span>你的音乐库</span>
            <span className="count">{total}</span>
          </div>

          <button
            className={`root-item${allActive ? ' is-active' : ''}`}
            type="button"
            onClick={onHome}
          >
            <Icon name="music" />
            <span className="root-name">全部歌曲</span>
            <span className="count">{total}</span>
          </button>

          {/* 每个根目录：点击切换视图，右侧垃圾桶移除索引 */}
          {roots.map((root) => (
            <div
              className={`root-row${activeRoot === root ? ' is-active' : ''}`}
              key={root}
            >
              <button
                className="root-select"
                type="button"
                title={root}
                onClick={() => onSelectRoot(root)}
              >
                <Icon name="folder" />
                <span className="root-name">{basename(root)}</span>
              </button>
              <button
                className="root-remove"
                type="button"
                title="从曲库移除"
                aria-label={`移除 ${basename(root)}`}
                onClick={() => onRemoveRoot(root)}
              >
                <Icon name="trash" />
              </button>
            </div>
          ))}

          {/* 播放列表区：+ 新建；每行可选中/删除 */}
          <div className="panel-subhead">
            <span>播放列表</span>
            <button
              className="mini-btn"
              type="button"
              onClick={onCreatePlaylist}
              aria-label="新建播放列表"
              title="新建播放列表"
            >
              <Icon name="plus" />
            </button>
          </div>

          {playlists.length ? (
            playlists.map((playlist) => (
              <div
                className={`playlist-row${activePlaylistId === playlist.id ? ' is-active' : ''}`}
                key={playlist.id}
              >
                <button
                  className="playlist-select"
                  type="button"
                  title={playlist.name}
                  onClick={() => onSelectPlaylist(playlist)}
                >
                  <Icon name="music" />
                  <span className="root-name">{playlist.name}</span>
                  <span className="count">{playlist.trackCount}</span>
                </button>
                <button
                  className="playlist-remove"
                  type="button"
                  title="删除播放列表"
                  aria-label={`删除播放列表 ${playlist.name}`}
                  onClick={() => onDeletePlaylist(playlist)}
                >
                  <Icon name="trash" />
                </button>
              </div>
            ))
          ) : (
            <p className="side-empty">还没有播放列表</p>
          )}
        </div>

        {/* 固定在底部的添加文件夹/重新扫描 */}
        <div className="side-actions">
          <button className="pill-btn" type="button" onClick={onAddFolder}>
            <Icon name="plus" />
            添加文件夹
          </button>
          <button className="ghost-btn" type="button" onClick={onScan} disabled={scanActive}>
            <Icon name="refresh" />
            {scanActive ? scanLabel : '重新扫描'}
          </button>
        </div>

        <div className="side-foot">
          音频输出 · {audioMode === 'wasapi' ? 'WASAPI' : audioMode || '未知'}
        </div>
      </div>
    </aside>
  );
}
