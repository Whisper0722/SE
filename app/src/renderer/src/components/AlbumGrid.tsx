// =============================================================================
// AlbumGrid.tsx - 专辑网格
//
// 输入 library.albums 的聚合结果，渲染成可点击的专辑卡片。
// 点击由 App 处理：切换到 album 视图并加载该专辑曲目。
// =============================================================================

import type { AlbumInfo } from '../types';
import { formatDuration } from '../utils';
import { Art } from './Art';

interface AlbumGridProps {
  albums: AlbumInfo[];
  onOpen: (album: AlbumInfo) => void;
}

export function AlbumGrid({ albums, onOpen }: AlbumGridProps) {
  return (
    <div className="album-grid">
      {albums.map((album) => (
        <button
          className="album-card"
          type="button"
          key={album.album}
          onClick={() => onOpen(album)}
          aria-label={`打开专辑 ${album.album}`}
        >
          <span className="album-art">
            <Art seed={`${album.album} ${album.artist}`} />
          </span>
          <span className="album-name">{album.album}</span>
          <span className="album-meta">
            {album.artist || '未知艺人'} · {album.trackCount} 首 ·{' '}
            {formatDuration(album.durationMs)}
          </span>
        </button>
      ))}
    </div>
  );
}
