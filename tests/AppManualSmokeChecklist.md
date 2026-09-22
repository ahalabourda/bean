# Application smoke checklist

These checks cover behavior that the headless CTest suite cannot verify:
Win32 message ordering, native controls, GDI resources, rendering, and
shutdown while asynchronous work is active.

## Startup and navigation

- Launch with a fresh configuration directory.
- Confirm the main window opens, all eight tabs are reachable, and no controls
  are missing or visually corrupted.
- Resize, minimize, restore, and maximize the window.
- Switch tabs repeatedly while the live-status timer is active.
- Close the application immediately after startup.

## Configuration and lifecycle

- Edit output and WoW paths and confirm status indicators update without UI
  freezing.
- Type rapidly in autosaved fields, then close immediately; verify the latest
  value persists.
- Change theme, resize the window, close, and relaunch; verify the theme and
  dimensions persist.
- Start and stop a manual recording.
- Close while a recording is active and verify the shutdown path completes.
- Trigger Windows shutdown/restart behavior if available and verify Bean does
  not veto the session.

## Feature interaction

- Open Chat Privacy, switch image modes, import an image, resize the overlay,
  and verify the preview repaints cleanly.
- Open Clipmaker, load a recording, seek, change volume, play/pause, and export
  both fast and precise clips.
- Open Recordings, sort columns, filter by type/key/name, select a run, and
  open it in Clipmaker.
- Open YouTube, link/unlink if credentials are available, refresh media, and
  verify upload progress and completion handling.
- Rebind, unbind, reset, and use each global hotkey.

## Async and shutdown stress

- Change recording paths while folder availability is probing.
- Finish or relocate a recording while Recordings and YouTube tabs are open.
- Switch Retail/PTR detection while monitoring is active.
- Close during an FFmpeg probe, clip export, YouTube operation, or recording
  reconciliation and verify the process exits without hanging or crashing.
