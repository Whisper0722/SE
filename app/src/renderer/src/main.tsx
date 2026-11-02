// =============================================================================
// main.tsx - React 渲染进程入口
//
// 把 <App /> 挂载到 index.html 的 #root。
// StrictMode 在开发期会双调用 effect，用于发现副作用清理问题；
// 生产构建不会重复挂载。
// =============================================================================

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import './styles.css';

const container = document.getElementById('root');
if (!container) throw new Error('root element missing');

createRoot(container).render(
  <StrictMode>
    <App />
  </StrictMode>
);
