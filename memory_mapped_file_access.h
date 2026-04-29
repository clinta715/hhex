#pragma once

#include <windows.h>
#include <string>
#include <cstdint>
#include "file_access.h"

// Window size for large file mapping (256 MB)
constexpr uint64_t MAPPING_WINDOW_SIZE = 256ULL * 1024 * 1024;

// Windows memory-mapped implementation of FileAccess.
// Supports both full-file mapping for small/medium files and windowed mapping for large files.
// TODO: Replace usage with EditorState (see EDITOR_STATE_SPEC.md)
class MemoryMappedFileAccess : public FileAccess {
private:
    HANDLE _hFile = INVALID_HANDLE_VALUE;
    HANDLE _hMapping = NULL;
    mutable uint8_t* _view = nullptr;  // mutable for const readByte remapping
    uint64_t _size = 0;

    // Windowed mapping state (mutable for const readByte remapping)
    mutable bool _useWindowedMapping = false;
    mutable uint64_t _windowOffset = 0;           // Current window start offset
    mutable uint64_t _windowSize = 0;             // Current window size (may be less than MAPPING_WINDOW_SIZE at EOF)
    mutable uint64_t _allocationGranularity = 65536; // System allocation granularity (default 64KB)

    // Remap window to cover the given offset (mutable for const method use)
    bool remapWindow(uint64_t offset, std::wstring& error) const;
    // Remap window to cover a range (offset to offset+length)
    bool remapWindowForRange(uint64_t offset, uint64_t length, std::wstring& error) const;
    uint64_t alignDown(uint64_t offset) const;

    // Prepare access for a range - ensures window covers the range (call before sequential access)
    bool prepareAccess(uint64_t offset, uint64_t length) {
        std::wstring error;
        return remapWindowForRange(offset, length, error);
    }

public:
    MemoryMappedFileAccess() = default;
    ~MemoryMappedFileAccess() override { close(); }

    bool open(const std::wstring& path, std::wstring& error) override;
    bool isOpen() const override { return _hMapping != NULL; }
    uint64_t size() const override { return _size; }
    uint8_t readByte(uint64_t offset) const override;  // const but can remap window (uses mutable)
    bool writeByte(uint64_t offset, uint8_t value) override;
    bool flush(std::wstring& error) override;
    bool flushRange(uint64_t offset, uint64_t length, std::wstring& error);
    void close() override;
};