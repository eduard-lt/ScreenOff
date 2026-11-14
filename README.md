# ScreenOff

**Turn off the monitors you're not using with one click, and keep using the one you are.**

ScreenOff is a tiny Windows tray app for multi-monitor setups. Click its tray icon (or press **Win + Alt + O**) and every monitor *except the one your cursor is on* goes into standby. Move the cursor back onto a sleeping screen and it wakes up.

Windows never sees the monitor disconnect, so your windows stay exactly where they were. There's no "PC screen only" shuffle.

👉 **[Website](https://eduard-lt.github.io/ScreenOff/)** · **[Download](https://github.com/eduard-lt/ScreenOff/releases/latest)** · **[♥ Support on Ko-fi](https://ko-fi.com/eduardolteanu)**

## Features

- **Left-click the tray icon:** puts every other screen to sleep. Click again to wake them all.
- **Global hotkey:** Win + Alt + O. It can be switched off from the menu.
- **Wake on cursor:** move the mouse onto a sleeping screen and it turns back on.
- **Real standby:** uses the monitor's DDC/CI power control (VCP `0xD6`), not a black overlay.
- **Start with Windows:** one click in the tray menu.
- **Single small `.exe`:** no installer, no background services, no telemetry.

## Requirements

- Windows 10 or 11
- A monitor that supports **DDC/CI** (most do; it's usually a toggle in the monitor's on-screen menu). Monitors that don't support it are skipped, and ScreenOff tells you.

## Usage

1. Download `ScreenOff.exe` from [Releases](https://github.com/eduard-lt/ScreenOff/releases/latest) and run it.
2. The icon appears in the tray. If it lands in the `^` overflow, drag it onto the taskbar.
3. With the cursor on the screen you want to keep, click the icon.

Right-click the icon for options: wake on cursor, hotkey, Start with Windows, and Exit. Exiting always wakes every screen.

## Build from source

Any MinGW-w64 clang/gcc works, for example the one bundled with [Windhawk](https://windhawk.net):

```bash
clang++ -std=c++23 -O2 -static -mwindows -municode -DUNICODE -D_UNICODE ScreenOff.cpp -o ScreenOff.exe -ldxva2 -lshell32 -lgdi32 -luser32 -ladvapi32
```

## Support

ScreenOff is free. If it's useful to you, you can [buy me a coffee on Ko-fi](https://ko-fi.com/eduardolteanu) ♥

## License

[MIT](LICENSE)
