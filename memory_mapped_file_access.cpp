#include "memory_mapped_file_access.h"
#include <windows.h>
#include <string>
#include <cstdint>
#include <stdexcept>

// Helper: format last error into wstring
static std::wstring FormatLastError(DWORD code) {
    LPWSTR msgBuf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&msgBuf, 0, NULL);
    std::wstring result;
    if (len && msgBuf) {
        // Trim trailing newlines
        while (len > 0 && (msgBuf[len - 1] == L'\r' || msgBuf[len - 1] == L'\n')) {
            msgBuf[len - 1] = 0;
            --len;
        }
        result.assign(msgBuf, len);
    } else {
        result = L"Unknown error";
    }
    if (msgBuf) LocalFree(msgBuf);
    result += L" (code ";
    result += std::to_wstring(code);
    result += L")";
    return result;
}

// Align offset down to allocation granularity
uint64_t MemoryMappedFileAccess::alignDown(uint64_t offset) const {
    return offset & ~(_allocationGranularity - 1);
}

// Remap window to cover the given offset (const method - modifies mutable members)
bool MemoryMappedFileAccess::remapWindow(uint64_t offset, std::wstring& error) const {
    return remapWindowForRange(offset, 1, error);
}

// Remap window to cover a range (offset to offset+length) (const method - modifies mutable members)
bool MemoryMappedFileAccess::remapWindowForRange(uint64_t offset, uint64_t length, std::wstring& error) const {
    if (offset >= _size) {
        error = L"remapWindow: offset beyond file size";
        return false;
    }

    // Calculate new window start (aligned)
    uint64_t newWindowOffset = alignDown(offset);
    uint64_t newWindowSize = MAPPING_WINDOW_SIZE;

    // Adjust window size if near EOF
    if (newWindowOffset + newWindowSize > _size) {
        newWindowSize = _size - newWindowOffset;
    }

    // Check if current window already covers this range
    if (_view != nullptr && _useWindowedMapping &&
        offset >= _windowOffset && (offset + length) <= (_windowOffset + _windowSize)) {
        return true; // Already mapped
    }

    // Unmap current view if any
    if (_view) {
        UnmapViewOfFile(_view);
        _view = nullptr;
    }

    // Map new window
    DWORD offsetHigh = (DWORD)(newWindowOffset >> 32);
    DWORD offsetLow = (DWORD)(newWindowOffset & 0xFFFFFFFF);

    _view = static_cast<uint8_t*>(MapViewOfFile(_hMapping,
                                                FILE_MAP_READ | FILE_MAP_WRITE,
                                                offsetHigh, offsetLow,
                                                (SIZE_T)newWindowSize));
    if (!_view) {
        error = L"MapViewOfFile (windowed) failed: " + FormatLastError(GetLastError());
        return false;
    }

    _windowOffset = newWindowOffset;
    _windowSize = newWindowSize;
    _useWindowedMapping = true;
    return true;
}

bool MemoryMappedFileAccess::open(const std::wstring& path, std::wstring& error) {
    close(); // ensure clean state

    // Get system allocation granularity
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    _allocationGranularity = sysInfo.dwAllocationGranularity;

    _hFile = CreateFileW(path.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ,
                         NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (_hFile == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW failed: " + FormatLastError(GetLastError());
        close();
        return false;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(_hFile, &liSize)) {
        error = L"GetFileSizeEx failed: " + FormatLastError(GetLastError());
        close();
        return false;
    }
    _size = static_cast<uint64_t>(liSize.QuadPart);

    if (_size == 0) {
        // Empty file; create mapping but no view
        _hMapping = CreateFileMappingW(_hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
        if (!_hMapping) {
            error = L"CreateFileMappingW failed: " + FormatLastError(GetLastError());
            close();
            return false;
        }
        _view = nullptr;
        _useWindowedMapping = false;
        return true;
    }

    // Create mapping for entire file
    _hMapping = CreateFileMappingW(_hFile, NULL, PAGE_READWRITE,
                                    (DWORD)(liSize.QuadPart >> 32),
                                    (DWORD)(liSize.QuadPart & 0xFFFFFFFF),
                                    NULL);
    if (!_hMapping) {
        error = L"CreateFileMappingW failed: " + FormatLastError(GetLastError());
        close();
        return false;
    }

    // Try full-file mapping first
    _view = static_cast<uint8_t*>(MapViewOfFile(_hMapping,
                                                FILE_MAP_READ | FILE_MAP_WRITE,
                                                0, 0, 0));
    if (_view) {
        // Full mapping succeeded
        _useWindowedMapping = false;
        _windowOffset = 0;
        _windowSize = _size;
        return true;
    }

    // Full mapping failed, fall back to windowed mode
    // Map initial window at offset 0
    _useWindowedMapping = true;
    _windowOffset = 0;
    _windowSize = (_size < MAPPING_WINDOW_SIZE) ? _size : MAPPING_WINDOW_SIZE;

    _view = static_cast<uint8_t*>(MapViewOfFile(_hMapping,
                                                FILE_MAP_READ | FILE_MAP_WRITE,
                                                0, 0,
                                                (SIZE_T)_windowSize));
    if (!_view) {
        error = L"MapViewOfFile (windowed fallback) failed: " + FormatLastError(GetLastError());
        close();
        return false;
    }

    return true;
}

uint8_t MemoryMappedFileAccess::readByte(uint64_t offset) const {
    if (offset >= _size) return 0;

    if (!_useWindowedMapping) {
        // Full mapping - direct access
        return _view[offset];
    } else {
        // Windowed mapping - remap if needed
        if (offset < _windowOffset || offset >= _windowOffset + _windowSize) {
            std::wstring error;
            if (!remapWindow(offset, error)) {
                return 0; // Failed to remap
            }
        }
        return _view[offset - _windowOffset];
    }
}

bool MemoryMappedFileAccess::writeByte(uint64_t offset, uint8_t value) {
    if (offset >= _size) return false;

    if (!_useWindowedMapping) {
        // Full mapping - direct access
        _view[offset] = value;
        return true;
    } else {
        // Windowed mapping - check if offset is in current window
        if (offset >= _windowOffset && offset < _windowOffset + _windowSize) {
            _view[offset - _windowOffset] = value;
            return true;
        }
        // Need to remap - but this is non-const, so we can do it
        std::wstring error;
        if (!remapWindow(offset, error)) {
            return false;
        }
        _view[offset - _windowOffset] = value;
        return true;
    }
}

bool MemoryMappedFileAccess::flush(std::wstring& error) {
    if (!_view) {
        return true; // Nothing mapped
    }

    if (!_useWindowedMapping) {
        // Full mapping - flush entire view
        if (!FlushViewOfFile(_view, 0)) {
            error = L"FlushViewOfFile failed: " + FormatLastError(GetLastError());
            return false;
        }
    } else {
        // Windowed mapping - flush current window only
        if (!FlushViewOfFile(_view, (SIZE_T)_windowSize)) {
            error = L"FlushViewOfFile (windowed) failed: " + FormatLastError(GetLastError());
            return false;
        }
    }

    if (_hFile != INVALID_HANDLE_VALUE) {
        if (!FlushFileBuffers(_hFile)) {
            error = L"FlushFileBuffers failed: " + FormatLastError(GetLastError());
            return false;
        }
    }
    return true;
}

bool MemoryMappedFileAccess::flushRange(uint64_t offset, uint64_t length, std::wstring& error) {
    if (!_view || length == 0) {
        return true;
    }
    if (offset >= _size) {
        error = L"flushRange invalid offset";
        return false;
    }

    // Clamp length to remaining file size
    if (offset + length > _size) {
        length = _size - offset;
    }

    if (!_useWindowedMapping) {
        // Full mapping - flush specified range
        if (!FlushViewOfFile(_view + offset, length)) {
            error = L"FlushViewOfFile(range) failed: " + FormatLastError(GetLastError());
            return false;
        }
    } else {
        // Windowed mapping - iterate through windows covering the range
        uint64_t currentOffset = offset;
        uint64_t remainingLength = length;

        while (remainingLength > 0) {
            // Remap window to cover current offset
            if (!remapWindowForRange(currentOffset, remainingLength, error)) {
                return false;
            }

            // Calculate how much we can flush in current window
            uint64_t windowEnd = _windowOffset + _windowSize;
            uint64_t flushEnd = currentOffset + remainingLength;
            if (flushEnd > windowEnd) {
                flushEnd = windowEnd;
            }
            uint64_t flushLength = flushEnd - currentOffset;

            // Flush this portion
            if (!FlushViewOfFile(_view + (currentOffset - _windowOffset), flushLength)) {
                error = L"FlushViewOfFile(windowed range) failed: " + FormatLastError(GetLastError());
                return false;
            }

            // Move to next portion
            currentOffset += flushLength;
            remainingLength -= flushLength;
        }
    }
    return true;
}

void MemoryMappedFileAccess::close() {
    if (_view) {
        UnmapViewOfFile(_view);
        _view = nullptr;
    }
    if (_hMapping) {
        CloseHandle(_hMapping);
        _hMapping = NULL;
    }
    if (_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(_hFile);
        _hFile = INVALID_HANDLE_VALUE;
    }
    _size = 0;
    _windowOffset = 0;
    _windowSize = 0;
    _useWindowedMapping = false;
}