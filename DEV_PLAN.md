# openFences 开发计划

## 当前状态

| 完成 | 模块 | 状态 |
|------|------|------|
| ✅ | 数据模型 (Fence, FenceManager, Rect, IconRef) | 可用 |
| ✅ | 独立 Fence 窗口 + 原生拖拽 | 可用 |
| ✅ | 逐像素 Alpha 渲染 (WIC→D2D→GDI DIB→UpdateLayeredWindow) | 可用 |
| ✅ | 标题文字 (DirectWrite Segoe UI) | 可用 |
| ✅ | 右键菜单 (Delete) | 可用 |
| ✅ | 布局算法 (网格排列, min_fence_size) | 有单测 |
| ✅ | 规则引擎 (OR 匹配, 多条件类型) | 有单测 |
| ✅ | 配置持久化 (JSON, %APPDATA%) | 有单测 |
| ✅ | Theme 主题常量 | Dark 配色 |
| ❌ | Fence Resize | WS_EX_LAYERED 限制，待 DXGI SwapChain |
| ❌ | 桌面交互 (图标枚举、拖入拖出) | 未实现 |
| ❌ | 系统托盘 / 生命周期 | 未实现 |
| ❌ | 快速隐藏/显示 | 未实现 |

## TODO: Fence Resize

- **问题：** `WS_EX_LAYERED` 窗口不支持系统原生 resize（`WM_NCLBUTTONDOWN` + HT 码和 `WM_SYSCOMMAND` + `SC_SIZE` 均不生效）。
  而手动 `SetWindowPos` / `UpdateLayeredWindow` 在 `WM_MOUSEMOVE` 中因 DWM 每帧合成导致卡顿。
- **根因：** `ID2D1HwndRenderTarget` / `UpdateLayeredWindow` + GDI DIB 方案与分层窗口的 resize 天然不兼容。
- **解决方案：** 改用 DXGI SwapChain 渲染后端（`DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` + `DXGI_ALPHA_MODE_PREMULTIPLIED`），
  直接渲染到窗口的 DXGI 交换链，窗口可以同时拥有 `WS_EX_LAYERED` 的逐像素 alpha 和完整的原生窗口行为（resize、DWM 合成）。

---

## 后续计划

### Phase 4: 应用外壳
- 4.1 系统托盘 (Shell_NotifyIcon + 右键菜单)
- 4.2 快速隐藏/显示
- 4.3 配置持久化集成 (启动加载 + 退出保存)
- 4.4 开机自启 (注册表 Run key)

### Phase 5: 桌面图标交互
- 5.1 图标枚举 (SysListView32)
- 5.2 Fence 内图标绘制
- 5.3 图标拖入/拖出

### Phase 6: 高级功能
- 多显示器 / 布局快照 / 主题跟随 / 规则编辑器 UI / Resize (DXGI SwapChain)
