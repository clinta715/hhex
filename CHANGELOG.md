# Hex Editor - Change Log

## Session: Large File Support & Bug Fixes (2026-04-29)

## Problem Identified
The program crashed when opening large files (>1-2 GB) because `MemoryMappedFileAccess` attempted to map the entire file into memory with a single `MapViewOfFile()` call, which fails due to:
- Virtual address space limitations
- Address space fragmentation
- Insufficient contiguous virtual memory

## Changes Implemented

### 1. Windowed Mapping Implementation

**Files Modified:**
- `memory_mapped_file_access.h`
- `memory_mapped_file_access.cpp`
- `file_access.h`
- `hex_editor.cpp`

**Details:**

#### `memory_mapped_file_access.h`
- Added `MAPPING_WINDOW_SIZE` constant (256 MB) for windowed mapping
- Added windowed mapping state variables (made `mutable` for const method access):
  - `_useWindowedMapping` - tracks if using windowed vs full mapping
  - `_windowOffset` - current window start offset
  - `_windowSize` - current window size
  - `_allocationGranularity` - system allocation granularity (default 64KB)
- Added methods:
  - `remapWindow(uint64_t offset, std::wstring& error) const`
  - `remapWindowForRange(uint64_t offset, uint64_t length, std::wstring& error) const`
  - `prepareAccess(uint64_t offset, uint64_t length)` - inline helper
- Made `_view` mutable to allow remapping in const `readByte()`

#### `memory_mapped_file_access.cpp`
- **`open()`**: Now tries full-file mapping first, falls back to windowed mapping if that fails
- **`readByte()`**: Changed to non-const (can remap window if offset is outside current window)
- **`writeByte()`**: Now remaps window if needed before writing
- **`flushRange()`**: Now iterates through multiple windows if the range spans them
- **`remapWindowForRange()`**: New method that:
  1. Calculates aligned window start
  2. Unmaps current view if any
  3. Maps new window covering the requested range
- **`alignDown()`**: Aligns offset down to allocation granularity

#### `file_access.h`
- Added virtual `prepareAccess(uint64_t offset, uint64_t length)` method with default no-op implementation

#### `hex_editor.cpp`
- Added file size limit check (10 GB) in `OpenFileWithMapping()` per ARCHITECTURE.md
- Added `FormatLastErrorStr()` helper function for error formatting
- Added `prepareAccess()` call before rendering to pre-load the window for visible bytes (optimization)
- Added proper file size check before attempting to map

### 2. Build Fixes
- Fixed const-correctness issues with `readByte()` (made members mutable)
- Fixed syntax errors from duplicate code in `flushRange()`
- Resolved compilation errors with `std::unique_ptr` and abstract class instantiation

---

## Bugs Found & Planned Fixes

### Critical Priority

#### Bug #1: `flush()` Fallback Doesn't Flush All Dirty Pages
**Location:** `memory_mapped_file_access.cpp:213-239`, `hex_editor.cpp:821-830`

**Problem:** In windowed mode, `flush()` only flushes the current window. When `SaveToFile()` falls back to `flush()` after `flushDirtyPages()` fails, it won't flush dirty pages in other windows.

**Fix Implemented:** Removed the fallback to `flush()` in `SaveToFile()`. If `flushDirtyPages()` fails, we now report the error directly. This is correct because:
- `flushDirtyPages()` uses `flushRange()` which properly handles windowed mode
- If selective flush fails, full flush would likely also fail
- The dirty bitmap is cleared by `flushDirtyPages()` only on success

**Status:** FIXED (2026-04-29)

---

#### Bug #2: Dirty Bitmap Not Cleared on Fallback Flush
**Location:** `hex_editor.cpp:821-830`

**Problem:** When `SaveToFile()` falls back to `flush()` after `flushDirtyPages()` fails, the dirty bitmap is never cleared. Next save attempt will try to flush the same (possibly failed) pages again.

**Fix Implemented:** Removed the fallback in `SaveToFile()`. The dirty bitmap is cleared by `SaveManager::flushDirtyPages()` only after successful selective flush. Since we no longer attempt a fallback flush that doesn't clear the bitmap, this issue is resolved.

**Status:** FIXED (2026-04-29)

---

#### Bug #3: Inconsistent State on Remap Failure
**Location:** `memory_mapped_file_access.cpp:42-86`

**Problem:** In `remapWindowForRange()`, if `UnmapViewOfFile()` succeeds but `MapViewOfFile()` fails:
- `_view` is set to `nullptr`
- But `_useWindowedMapping`, `_windowOffset`, `_windowSize` retain stale values
- Subsequent calls might incorrectly think a window is mapped

**Fix Implemented:** Reset state BEFORE attempting remap:
```cpp
// Reset state before attempting remap to avoid inconsistent state
_windowOffset = 0;
_windowSize = 0;
_useWindowedMapping = false;

// Then attempt remap
if (!_view) {
    UnmapViewOfFile(_view);
    _view = nullptr;
}
// Map new view...
// Only update state after successful mapping
```

**Status:** FIXED (2026-04-29)

---

### Medium Priority

#### Bug #4: `writeByte()` Return Value Ignored
**Location:** `hex_editor.cpp:606, 635`

**Problem:** In paste and cut operations, `writeByte()` return value isn't checked. If the write fails (e.g., remap failure), the dirty bitmap is still marked, causing potential issues during save.

**Planned Fix:**
```cpp
if (g_fileAccess->writeByte(g_editorState.cursorIndex, (uint8_t)byteVal)) {
    g_dirtyBitmap.mark(g_editorState.cursorIndex);
    g_editorState.cursorIndex++;
} else {
    // Handle error - maybe beep or show message
}
```

**Status:** Planned

---

#### Bug #5: Silent Failures in `readByte()`
**Location:** `memory_mapped_file_access.cpp:172-188`

**Problem:** If `remapWindow()` fails, `readByte()` returns `0` silently. The caller can't distinguish between a valid `0x00` byte and a failure. The `error` string in `remapWindow()` is discarded.

**Planned Fix:**
- Option A: Change `readByte()` signature to return success/failure (breaks interface)
- Option B: Store last error in class member that can be queried
- Option C: Assert/debug output on remap failure

**Status:** Planned - Need to decide approach

---

#### Bug #6: `prepareAccess()` Return Value Ignored
**Location:** `hex_editor.cpp:247`

**Problem:** The pre-load before rendering ignores the return value. If it fails, rendering continues and each `readByte()` call will individually attempt (and potentially fail) to remap.

**Planned Fix:**
```cpp
if (g_fileAccess && g_fileAccess->isOpen()) {
    uint64_t startOffset = (uint64_t)startLine * BYTES_PER_LINE;
    uint64_t bytesToAccess = (uint64_t)linesToDraw * BYTES_PER_LINE;
    if (startOffset + bytesToAccess > totalBytes64) {
        bytesToAccess = totalBytes64 - startOffset;
    }
    if (!g_fileAccess->prepareAccess(startOffset, bytesToAccess)) {
        // Log error or handle gracefully
    }
}
```

**Status:** Planned

---

### Design Issues

#### Issue #7: `flushRange()` Not in Base Class
**Location:** `file_access.h`, `save_manager.cpp`

**Problem:** `flushRange()` is only in `MemoryMappedFileAccess`, not in `FileAccess`. The `SaveManager` works by `dynamic_cast`-ing to `MemoryMappedFileAccess*`, which is fragile and not polymorphic.

**Planned Fix:**
- Add `flushRange()` to `FileAccess` base class as virtual method
- Provide default implementation that calls `flush()`

**Status:** Planned

---

#### Issue #8: Error Handling Inconsistency
**Problem:** `memory_mapped_file_access.cpp` has `FormatLastError()` returning `std::wstring`, while `hex_editor.cpp` has `FormatLastErrorStr()` returning `std::string`. Two separate implementations for the same purpose.

**Planned Fix:**
- Create a single utility function (maybe in a shared utils header)
- Or consolidate to use one consistently

**Status:** Planned - Low priority

---

### Minor Issues

#### Issue #9: `alignDown()` Assumes Power-of-2 Granularity
**Location:** `memory_mapped_file_access.cpp:32-34`

**Problem:** Uses bitmask operation `offset & ~(_allocationGranularity - 1)` which only works correctly if `_allocationGranularity` is a power of 2.

**Planned Fix:**
```cpp
uint64_t MemoryMappedFileAccess::alignDown(uint64_t offset) const {
    return (offset / _allocationGranularity) * _allocationGranularity;
}
```

**Status:** Planned - Low priority (granularity is usually 64KB which is power-of-2)

---

#### Issue #10: Potential Negative `linesToDraw`
**Location:** `hex_editor.cpp:208`

**Problem:** If `startLine` is calculated incorrectly (e.g., negative or > `totalLines`), `totalLines - startLine` could be negative or very large.

**Planned Fix:**
```cpp
int linesToDraw = max(0, min(g_editorState.visibleLines, totalLines - startLine));
```

**Status:** Planned - Low priority

---

## Testing Recommendations

After implementing fixes, test with:
1. Small files (< 100 MB) - should use full mapping
2. Medium files (100 MB - 1 GB) - should use full mapping
3. Large files (1 GB - 10 GB) - should use windowed mapping
4. Files > 10 GB - should be rejected with clear error
5. Edit and save operations on large files
6. Scroll through entire large file
7. Save after editing in different windows

---

## Files Modified Summary

| File | Changes Made | Status |
|------|---------------|--------|
| `memory_mapped_file_access.h` | Added windowed mapping support | Complete |
| `memory_mapped_file_access.cpp` | Implemented windowed mapping | Complete |
| `file_access.h` | Added `prepareAccess()` method | Complete |
| `hex_editor.cpp` | Added file size limit, pre-load optimization | Complete |
| `CHANGELOG.md` | This document | New |

---

## Next Steps

1. Fix Bug #1 and #2 (flush correctness) - **Critical**
2. Fix Bug #3 (remap state consistency) - **Critical**
3. Fix Bug #4 and #5 (error handling) - **Medium**
4. Address design issues #7 and #8 - **Low-Medium**
5. Test thoroughly with large files
