#pragma once

#include <string>
#include <cstdint>

// FileAccess abstraction for hex editor; transitional layer before EditorState refactor.
// TODO: Replace usage with EditorState (see EDITOR_STATE_SPEC.md)
class FileAccess {
public:
    virtual ~FileAccess() = default;

    // Open a file for read/write access using supplied path.
    // On failure returns false and populates 'error'.
    virtual bool open(const std::wstring& path, std::wstring& error) = 0;

    // Returns true if the file is currently open/mapped.
    virtual bool isOpen() const = 0;

    // Size of underlying file in bytes.
    virtual uint64_t size() const = 0;

    // Read single byte at offset; if out of range may return 0.
    virtual uint8_t readByte(uint64_t offset) const = 0;

    // Write single byte at offset; returns false if out of range or failure.
    virtual bool writeByte(uint64_t offset, uint8_t value) = 0;

    // Flush any modified bytes to disk; error populated on failure.
    virtual bool flush(std::wstring& error) = 0;

    // Close/unmap any resources.
    virtual void close() = 0;

    // Prepare access for a range - hint to implementation to prefetch/remap (optional optimization).
    // Default implementation does nothing; override in subclasses that benefit from prefetching.
    virtual bool prepareAccess(uint64_t /*offset*/, uint64_t /*length*/) { return true; }

    // Get last error from read/write operations (empty string if no error)
    virtual std::string getLastError() const { return ""; }

    // Flush a range of bytes to disk (optional - only meaningful for memory-mapped implementations).
    // Default implementation calls flush() for backward compatibility.
    virtual bool flushRange(uint64_t /*offset*/, uint64_t /*length*/, std::wstring& error) {
        return flush(error);
    }
};