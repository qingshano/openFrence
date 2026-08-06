# openFences (C++)

## 目标

Stardock Fences 6 的开源替代。C++ + Win32 API + Direct2D，单进程 + Shell Extension DLL。

## 核心教训（来自 Rust 原型）

1. **Z-order**：`WS_EX_LAYERED` 窗口由 DWM 独立合成，永远浮在最上。正确做法是 Shell Extension DLL 注入 explorer，直接在桌面 ListView 上绘制。
2. **图标交互**：必须通过 `IFolderView2` COM 接口读写桌面图标。跨进程 `WriteProcessMemory` 会崩溃 explorer。
3. **Hook 注入**：`SetWindowsHookEx(WH_GETMESSAGE)` + 导出的 hook proc，DLL 在 explorer 进程内运行。
4. **逐像素 alpha**：WIC bitmap + D2D WicBitmapRenderTarget + UpdateLayeredWindow 是唯一可行的方案。

## 架构

```
┌─────────────────────────────────┐
│  openfences.exe (主进程)        │
│  ┌───────────────────────────┐  │
│  │ 配置管理 (JSON)            │  │
│  │ Fence 管理 (创建/删除)     │  │
│  │ 系统托盘                   │  │
│  │ 主题/样式                  │  │
│  └───────────────────────────┘  │
│            │                    │
│    WM_COPYDATA (共享内存 IPC)   │
│            ↓                    │
└─────────────────────────────────┘
             │
┌─────────────────────────────────┐
│  openfences_hook.dll (explorer) │
│  ┌───────────────────────────┐  │
│  │ Hook 入口 → GetMsgProc    │  │
│  │ 子类化 SysListView32      │  │
│  │ D2D 覆盖层绘制             │  │
│  │ LVM_SETITEMPOSITION 拦截  │  │
│  │ IFolderView2 COM 枚举     │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

## 技术栈

| 项 | 选择 |
|----|------|
| 语言 | C++20 |
| 构建 | CMake 3.20+ |
| 包管理 | vcpkg (manifest mode) |
| 渲染 | Direct2D + DirectWrite + WIC |
| Shell | IFolderView2 COM (shobjidl.h) |
| 注入 | SetWindowsHookEx(WH_GETMESSAGE) |
| JSON | nlohmann/json (header-only) |
| 测试 | GoogleTest |

## 项目结构

```
openfences/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp              # 入口、COM 初始化
│   ├── app.h/cpp             # 应用生命周期
│   ├── fence_manager.h/cpp   # Fence 数据管理
│   ├── tray.h/cpp            # 系统托盘
│   ├── config.h/cpp          # JSON 持久化
│   ├── theme.h/cpp           # 主题常量
│   └── render/
│       ├── d2d_context.h/cpp   # D2D 渲染上下文
│       └── fence_renderer.h/cpp# Fence 容器绘制
├── hook/
│   ├── CMakeLists.txt
│   ├── dllmain.cpp           # DllMain + GetMsgProc 导出
│   ├── listview_subclass.h/cpp # SysListView32 子类化
│   ├── icon_manager.h/cpp    # IFolderView2 图标枚举
│   └── ipc.h/cpp             # WM_COPYDATA 通信协议
└── tests/
    ├── CMakeLists.txt
    └── test_config.cpp
```

## 数据流

```
1. 用户启动 openfences.exe
2. exe 加载 config.json → 创建 Fence 数据
3. exe LoadLibrary("openfences_hook.dll") → SetWindowsHookEx → 注入 explorer
4. Hook DLL GetMsgProc 首次触发 → find SysListView32 → 子类化
5. exe → WM_COPYDATA → DLL: "这是 Fence 区域 [(x,y,w,h), ...]"
6. DLL 子类化过程拦截 LVM_SETITEMPOSITION:
   - 检查目标坐标是否在 Fence 内
   - 在 Fence 内 → 约束到范围内
   - 不在 Fence 内 → 放行
7. DLL 通过 IFolderView2 枚举图标 (名称、路径、图标图)
8. exe 可发送新的 Fence 布局 → DLL 更新约束区域
9. 退出：exe → WM_COPYDATA 清空 Fence → unhook → 保存 config
```

## 实现顺序

### Phase 1: 骨架 (exe 进程)
- CMake 项目 + vcpkg manifest
- Win32 消息循环 + 窗口创建
- D2D 渲染上下文 + Fence 圆角矩形
- 逐像素 alpha (WIC + UpdateLayeredWindow)
- 系统托盘 + 右键菜单

### Phase 2: Fence 交互
- 拖拽移动 (WM_NCLBUTTONDOWN + HTCAPTION)
- 右键菜单 (外观调整)
- 双击重命名
- JSON 配置持久化

### Phase 3: Hook DLL
- DLL 项目 → DllMain + GetMsgProc 导出
- GetMsgProc → 首次触发时 find SysListView32 → SetWindowSubclass
- 子类 proc 拦截 LVM_SETITEMPOSITION → 约束到 Fence 区域
- WM_COPYDATA 接收 → 更新 Fence 坐标列表
- IFolderView2 COM → 枚举图标 (名称+图标图)

### Phase 4: 图标渲染
- SHGetFileInfo → 获取图标 HICON
- D2D bitmap 转换 → 在 Fence 内绘制图标+名称
- 网格自动排列

### Phase 5: 高级功能
- 规则引擎 UI (自动归类)
- 多显示器支持
- 系统主题跟随 (Light/Dark)
- 布局快照/恢复

---

## 关键代码片段

### Hook DLL 导出

```cpp
// hook/dllmain.cpp
HINSTANCE g_hInst;
HWND g_hListView = NULL;
HHOOK g_hHook = NULL;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_hListView == NULL) {
        // 首次调用：找到桌面 ListView 并子类化
        HWND progman = FindWindow(L"Progman", NULL);
        HWND defview = FindWindowEx(progman, NULL, L"SHELLDLL_DefView", NULL);
        g_hListView = FindWindowEx(defview, NULL, L"SysListView32", NULL);
        if (g_hListView) {
            SetWindowSubclass(g_hListView, ListViewSubclassProc, 0, 0);
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}
```

### 子类化拦截图标位置

```cpp
// hook/listview_subclass.cpp
LRESULT CALLBACK ListViewSubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR)
{
    if (msg == WM_COPYDATA) {
        auto* cds = (COPYDATASTRUCT*)lp;
        if (cds->dwData == 1) { // SET_FENCES
            // 更新 Fence 坐标
            g_fences.assign((FenceRect*)cds->lpData,
                (FenceRect*)cds->lpData + cds->cbData / sizeof(FenceRect));
        }
        return TRUE;
    }
    if (msg == LVM_SETITEMPOSITION) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        ConstrainToFences(x, y); // 约束到所属 Fence 内
        lp = MAKELPARAM(x, y);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
```

### exe 安装 Hook

```cpp
// src/app.cpp
HMODULE hDll = LoadLibrary(L"openfences_hook.dll");
auto* GetMsgProc = (HOOKPROC)GetProcAddress(hDll, "GetMsgProc");
DWORD tid = GetWindowThreadProcessId(hListView, NULL);
HHOOK hook = SetWindowsHookEx(WH_GETMESSAGE, GetMsgProc, hDll, tid);
```

### IPC: 发送 Fence 布局

```cpp
// src/app.cpp
COPYDATASTRUCT cds;
cds.dwData = 1; // SET_FENCES
cds.cbData = fences.size() * sizeof(FenceRect);
cds.lpData = fences.data();
SendMessage(hListView, WM_COPYDATA, 0, (LPARAM)&cds);
```

## vcpkg 依赖

```json
{
  "name": "openfences",
  "dependencies": [
    "nlohmann-json"
  ]
}
```

其余全部系统 API：D2D (d2d1.h)、WIC (wincodec.h)、DirectWrite (dwrite.h)、Shell (shobjidl.h)。

## 单进程 vs 双进程

最终形态是双进程（exe + hook DLL），但开发阶段先从单进程 exe 开始，Hook DLL 在 Phase 3 再加入。Phase 1-2 的 exe 可以独立运行和测试。