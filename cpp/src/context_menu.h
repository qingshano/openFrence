#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

struct IContextMenu;

/// A self-drawn, Windows 11–styled context menu.
///
/// Windows 11's modern menu is rendered by Explorer's XAML host and cannot be
/// obtained from a third-party process (every route to an aggregated
/// IEnumExplorerCommand returns E_NOINTERFACE). So we take the fully-populated
/// legacy HMENU (whose contents are identical to what the system shows), read
/// each item's text/icon/state, and re-render it ourselves in the Win11 dark
/// Fluent style: rounded corners, acrylic backdrop, hover highlight, submenu
/// chevrons and separators.
class FluentMenu {
public:
    /// Show the menu built from `menu` (already populated by
    /// `cm->QueryContextMenu`) at screen point `ptScreen`. Blocks until the
    /// user picks an item or dismisses the menu.
    ///
    /// Returns the chosen command id (>= 1), or 0 if the menu was dismissed
    /// without a selection. Returns -1 when the Fluent menu could not be
    /// created at all (empty model, D2D failure), in which case the caller
    /// should fall back to TrackPopupMenu. `filePath` is only used to supply
    /// the file-type icon for the default "Open" verb.
    static int Run(HWND owner, IContextMenu* cm, HMENU menu,
                   const std::wstring& filePath, POINT ptScreen);

    /// Same contract as above (>= 1 chosen / 0 dismissed / -1 fall back),
    /// but for a plain HMENU that carries no shell verb list — the tray
    /// menu and the fence title-bar menu. Items are rendered exactly as
    /// they appear in the HMENU (text, MF_CHECKED state, separators,
    /// submenus); there is no default-verb icon logic.
    static int Run(HWND owner, HMENU menu, POINT ptScreen);
};
