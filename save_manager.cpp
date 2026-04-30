// [flushDirtyPages()](save_manager.cpp:9) implementation.
#include "save_manager.h"
#include "file_access.h"
#include "dirty_bitmap.h"
#include <string>
#include <cstdint>
namespace SaveManager {
bool flushDirtyPages(FileAccess& access, DirtyBitmap& bitmap, std::wstring& error, uint64_t& flushedPages) {
    flushedPages = 0;
    if (bitmap.dirtyPageCount() == 0) {
        return true;
    }
    bool allOk = true;
    bitmap.forEachDirtyRange([&](uint64_t startPage, uint64_t endPageExclusive) {
        if (!allOk) return;
        uint64_t pages = endPageExclusive - startPage;
        uint64_t offset = startPage * DIRTY_PAGE_SIZE;
        uint64_t length = pages * DIRTY_PAGE_SIZE;
        if (!access.flushRange(offset, length, error)) {
            allOk = false;
            return;
        }
        flushedPages += pages;
    });
    if (allOk) {
        bitmap.clear(); // Clear only after successful selective flush
    }
    return allOk;
}
}