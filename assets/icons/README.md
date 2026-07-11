Put your custom Windows icon file in this folder.

Required filename (recommended):
- `bean_idle.ico` - primary app icon used for the executable, title bar, taskbar button, and shortcuts.

Also supported (fallback names):
- `bean-16.ico`
- `bean-32.ico`
- `bean-48.ico`
- `bean-256.ico`

Notes:
- Use `.ico` files (not `.png` or `.jpg`).
- Each `.ico` should include multiple sizes (at least 16, 32, 48, and 256 px).
- The app loads this file from `<exe folder>/assets/icons/`.
- `bean_idle.ico` is embedded into the built `.exe` icon resource when present. If it is missing, CMake falls back to `bean-icon-256.ico` (then 48/32/16).
- CMake is configured to copy `assets/icons` into the output folder after each `bean_app` build.
- Runtime status indicators use native Windows taskbar overlay badges (green/red/yellow dots).
- If no icon files are found, the app falls back to the embedded icon (if available), then the system default icon.
- After changing `bean_idle.ico`, rebuild `bean_app` so the executable resource icon is refreshed.
