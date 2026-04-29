# Hex Editor Project

## Overview
This project implements a simple Win32-based hexadecimal editor in C++. Current functionality includes window creation, menu system (File / Open / Save / Exit), binary file loading, hex + ASCII rendering, cursor navigation, in-place nibble editing, and saving changes back to disk.

## Source Files
- [hex_editor.cpp](hex_editor.cpp): Main application logic (WinMain, message handling, rendering, editing, file I/O).
- [resource.h](resource.h): Menu and command identifier definitions.
- [resource.rc](resource.rc): Resource script defining the application menu.

## Build Instructions (Visual Studio Developer Command Prompt)
Ensure you open a "x64 Native Tools Command Prompt for VS 2022" or run vcvars64.bat, then:
cl /EHsc hex_editor.cpp /link /out:hex_editor.exe User32.lib Gdi32.lib Comdlg32.lib

## Running
Execute: hex_editor.exe

## Features Implemented
1. File Open (standard Open File dialog) loads entire file into an in-memory byte buffer.
2. Hex view displays offset, 16 bytes per line (configurable), and ASCII representation.
3. Navigation with Arrow keys (Left/Right/Up/Down) moves cursor across bytes and lines.
4. In-place editing: entering hex characters 0-9 A-F edits high nibble then low nibble; byte auto-advances after completing a full byte.
5. Save overwrites original file path with current buffer contents.
6. Basic visual highlighting of current byte (separate highlight for hex vs ASCII columns).

## Editing Model
- Cursor index points to a single byte in the buffer.
- A nibble phase flag tracks whether next hex digit edits the high or low nibble.
- After low nibble edit, cursor advances (unless at end of buffer).
- No insertion/deletion yet (buffer size fixed to file length).

## Rendering
- Performed inside WM_PAINT via RenderHexView: draws offset, hex bytes, ASCII.
- Uses TextOut for simplicity; could replace with DrawText or double-buffered rendering for smoother large-file performance.

## File I/O
- Entire file read into std::vector<unsigned char>.
- Save rewrites file in binary mode.
- Large files (multi-GB) currently not optimized; memory mapping recommended for future improvement.

## Testing Plan
Manual functional tests:
- Open small binary file (e.g., 256 bytes) and verify display formatting.
- Navigate across line boundaries with all arrow keys.
- Edit multiple bytes and confirm hex + ASCII updates immediately.
- Save and diff original vs modified file externally (e.g., fc /b original modified).
- Test behavior at start and end of buffer (boundary navigation and nibble editing).
- Attempt opening non-existent or protected file and confirm error handling.
- Open a larger file (a few MB) and observe performance / responsiveness.
- Edit then reopen file to confirm persistence of changes.

Suggested regression automation (future):
- Abstract logic (render layout calculations, nibble editing) into testable pure functions.
- Add unit tests with a framework (e.g., GoogleTest) for buffer edit operations and cursor math.

## Performance Optimization Roadmap
- Memory map files (CreateFile + CreateFileMapping + MapViewOfFile) to avoid full memory copy and enable partial paging.
- Implement viewport + vertical scrolling; only render visible lines.
- Introduce a backing offscreen bitmap (double buffering) to minimize flicker.
- Precompute formatted line strings to reduce per-paint computations.
- Use a fixed-width font (e.g., Consolas) set via CreateFont for alignment improvements.
- Defer repaint on rapid key repeat (aggregate updates).

## Planned Feature Enhancements
- Insert / delete bytes.
- Selection, copy, paste (hex and ASCII).
- Search (forward/backward) for byte sequences or ASCII substrings.
- Go to offset dialog.
- Undo/redo stack.
- Status bar (current offset, total size, overwrite mode).
- ASCII direct editing (switch focus between hex/ASCII panes).
- Configurable bytes per line.
- Read-only mode for protected files.
- Recent Files MRU (see [RECENT_FILES_SPEC.md](RECENT_FILES_SPEC.md))

## Code Documentation Notes
- Global state kept intentionally minimal; consider encapsulating editor state in a struct/class.
- Separation of concerns: future refactor would move rendering, input handling, file I/O into distinct modules.
- Add comments for each major block (rendering, editing, file operations) and document invariants (cursor < buffer.size()).

## User Guide
Menu: File -> Open to select a file; File -> Save to write changes; File -> Exit to quit.
Navigation: Arrow keys.
Editing: Type hex digits (0-9 A-F). First digit edits high nibble, second edits low nibble, then advances.
View: Hex portion shows bytes; ASCII portion shows printable characters or '.' for non-printables.

## Known Limitations
- No scrolling: large files truncated to window height.
- No error messages for partial read/write beyond generic MessageBox.
- No undo/redo; edits are immediate.
- Entire file loaded into memory (RAM usage scales with file size).

## Maintenance Recommendations
- Add logging (OutputDebugString) for file operations and editing in debug builds.
- Wrap file I/O in RAII helpers.
- Provide configuration (bytes per line, font) via an INI or registry settings.

## License
(Add license information here if distributing.)

## Contribution Guidelines
- Fork, create feature branch, submit PR with before/after screenshots for UI changes.

## Summary
The current implementation establishes a functional foundation for a hex editor suitable for extension with advanced editing, navigation, and performance features.

End of document."# hhex" 
