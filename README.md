# FullWorkArea

An ultra-lightweight (**8 KB** binary, **~0.2–1 MB RAM**, **0% idle CPU**) pure Win32 C utility for Windows 11 (including **26H2 Build 26300** with native Top Taskbar) that lets maximized applications extend all the way to the screen edge behind the taskbar — without DLL injection, without hooks inside `explorer.exe`, and without third-party shell modders.

## Why?

On Windows 11 26H2 (Build 26300), Microsoft introduced a native Top Taskbar option (`TaskbarLocation = 1`), but Auto-Hide is not yet supported for non-bottom positions. As a result, Windows reserves the top strip in the monitor's `WorkingArea`, preventing maximized apps from utilizing the full screen height.

**FullWorkArea** solves this cleanly using only official Win32 APIs (`KERNEL32.dll`, `USER32.dll`, `GDI32.dll`) with zero C runtime (`-nostdlib`) and zero background polling.

## Features

- **Full-Screen Work Area:** Expands every monitor's `WorkingArea` (`SPI_SETWORKAREA` without `SPIF_SENDCHANGE`) to match the full monitor bounds so maximized windows extend smoothly to `Y = 0`.
- **Smart Click-Through Pill Clipping & Offset:**
  - Clips away the unused left strip (`0..27%`) of `Shell_TrayWnd` so top-left app menus and browser tabs remain directly clickable.
  - Shifts the internal XAML bridge (`Windows.UI.Composition.DesktopWindowContentBridge`) inward to free top-right window controls (`— □ X`), with automatic fallback verification so icons and the clock are never cut off if the shell locks the bridge position.
  - Automatically adapts rounded inner corners for both Top and Bottom taskbars.
- **Instant Global Hotkeys (Zero-CPU `RegisterHotKey`):**
  - `` Win + ` `` *(fallback: `` Alt + ` ``)* — Instantly hide / show the taskbar.
  - `` Win + Shift + ` `` *(fallback: `` Alt + Shift + ` ``)* — Toggle pill clipping/offset mode on/off.
- **Event-Driven & Self-Healing:** Sleeps in kernel `GetMessageW()` (`0%` idle CPU) and automatically re-applies after display changes, sleep/wake, or `explorer.exe` restarts (`TaskbarCreated`).
- **Clean Exit & Restore:** Running `FullWorkArea.exe --stop` (or `Stop.bat`) cleanly exits the daemon, restores the taskbar layout/region/visibility, and restores Windows' default `WorkingArea`.

## Footprint

| Metric | Value |
|---|---|
| Binary Size | **8 KB** (`8,192 bytes`) |
| Idle CPU | **0.00%** (`0 ms` delta, zero polling) |
| Memory (Working Set) | **~176 KB – 1.3 MB** |
| DLL Dependencies | `KERNEL32.dll`, `USER32.dll`, `GDI32.dll` |
| Explorer Injection | **None** |

## Usage

Download `FullWorkArea.exe` from [Releases](../../releases) (or build it yourself), then run it:

```bat
:: Start with Full WorkArea + Smart Pill + Hotkeys
FullWorkArea.exe

:: Start with Full WorkArea + Hide/Show Hotkey only (no pill clipping)
FullWorkArea.exe --no-center

:: Stop the running instance and restore default Windows taskbar & work area
FullWorkArea.exe --stop
```

## Build from Source

Requires MinGW-w64 (`gcc`):

```bat
build.bat
```

## License

[MIT](LICENSE)
