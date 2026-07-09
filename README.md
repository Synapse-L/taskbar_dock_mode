# Taskbar Dock Mode (macOS-style overlay taskbar)

A [Windhawk](https://windhawk.net/) mod for **Windows 11** that makes the taskbar behave like the macOS Dock: it stays permanently drawn **on top of all windows** and **stops reserving space** in the monitor's Work Area — while keeping the 100% native Windows look (Mica, Acrylic, rounded corners, and animations are untouched).

> Para a versão em português deste documento, veja **LEIA-ME.md**.

## What it does

- The **Work Area** of every monitor becomes the full monitor resolution.
- **Maximized windows** use the entire screen, including the strip where the taskbar sits.
- The taskbar (`Shell_TrayWnd` and `Shell_SecondaryTrayWnd` on secondary monitors) is enforced as `HWND_TOPMOST`, so it always stays visible above other windows.
- Everything on the taskbar keeps working: Start, search, widgets, system tray, clock, notifications.
- Works with **multiple monitors**, **different DPI scales**, and taskbars docked to **any edge** (bottom, top, left, right).

## What it does NOT change

- No visual changes: appearance, transparency, Mica/Acrylic, rounded corners, and animations are left exactly as Windows renders them.
- No Explorer components are replaced. No external background apps. Everything lives inside this single Windhawk mod.

## How it works (technical summary)

Instead of rewriting the taskbar's internal geometry math, the mod uses the mechanism Windows itself provides for "Work Area = full screen":

1. It programmatically enables taskbar auto-hide via `SHAppBarMessage(ABM_SETSTATE, ABS_AUTOHIDE)` — a public, documented API. With auto-hide on, Windows natively gives the full monitor area as the Work Area on **every** monitor, at any DPI, with the bar on any edge. Maximized windows use 100% of the screen.
2. It then blocks the act of hiding: hooks on `TrayUI::_Hide()` (`taskbar.dll`, Win32 side) and on `winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded` (`Taskbar.View.dll`, XAML side — also covers secondary-monitor taskbars) keep every taskbar permanently expanded and visible.

The result is an auto-hide taskbar that never hides: no reserved space, always on top, fully native visuals. Two event-driven `WinEventHook`s (no polling) re-assert `HWND_TOPMOST` and re-apply the auto-hide state if something (e.g. the Settings app) turns it off, and a short-lived startup thread waits for `Shell_TrayWnd` to exist before applying the state — covering Explorer restarts as well, since Windhawk re-injects the mod into any new `explorer.exe`.

**Visible (and reversible) side effect:** while the mod is active, the "Automatically hide the taskbar" toggle in Windows Settings will show as ON — that's the mechanism in use. The original state is saved and restored when the mod is disabled, which makes Windows itself recalculate the native Work Area.

## Installation

1. Install [Windhawk](https://windhawk.net/) if you don't have it yet.
2. Open Windhawk → click the **profile/menu** button → **New mod** (or, in newer versions, "Create a new mod" from the mod development area).
3. Replace the template with the full contents of `taskbar-dock-mode.wh.cpp`.
4. Save (Ctrl+S). Windhawk compiles the mod and enables it. On first load it may download debug symbols for `taskbar.dll` from Microsoft's public symbol server — this is normal and happens once per Windows build.

## Uninstalling / restoring

Disable or remove the mod in Windhawk. The hook is removed automatically and the mod broadcasts a display-change notification to force the Work Area to be recalculated. If maximized windows do not immediately return to their original size, restart Explorer (`explorer.exe`) once to guarantee full restoration.

## Known limitations

- **Exclusive-fullscreen apps** (most games in exclusive DirectX/OpenGL mode, some video players) use a composition path that bypasses the normal window manager. Windows itself suspends topmost windows in that mode, so those apps can cover the taskbar. This is outside the reach of any hook in `explorer.exe`.
- Legacy apps that query `SHAppBarMessage(ABM_GETTASKBARPOS)` to avoid overlapping the taskbar may draw underneath it — which is effectively the macOS Dock behavior anyway.
- `TrayUI::_Hide` and `ViewCoordinator::ShouldTaskbarBeExpanded` are **undocumented Microsoft symbols**. Their signatures can change between Windows builds. If the mod fails to load after a Windows update, check the Windhawk log; the [Windhawk Symbol Helper](https://github.com/ramensoftware/windhawk-symbol-helper) tool can show the current symbol names in `taskbar.dll` and `Taskbar.View.dll` so you can update the strings in the hook arrays.

## Files in this folder

| File | Description |
|---|---|
| `taskbar-dock-mode.wh.cpp` | The Windhawk mod source code (C++) |
| `README.md` | This document (English) |
| `LEIA-ME.md` | Portuguese version of this document |

## License

GNU General Public License v3.0.
