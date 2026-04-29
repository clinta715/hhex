#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <cstdio>
#include <memory>
#include <io.h>
#include "resource.h"
#include "file_access.h"
#include "memory_mapped_file_access.h"
#include "editor_state.h"
#include "edit_controller.h"
#include "dirty_bitmap.h"
#include "save_manager.h"
#include "recent_files.h"
#include "recent_files_menu.h"
#include "recent_files_ids.h"
#include "search_ids.h"
#include "search_types.h"
#include "search_engine.h"
#include "search_dialog.h"
#include "goto_dialog.h"

// Forward declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void UpdateScrollBar(HWND hwnd);
void EnsureCursorVisible(HWND hwnd);
void RenderHexView(HDC hdc, RECT& rc);
bool OpenFileWithMapping(const std::string& filePath);
bool SaveToFile(const char* filePath, const std::vector<unsigned char>& buffer);

// Legacy forward declaration kept (deprecated)
bool OpenAndReadFile(const char* filePath, std::vector<unsigned char>& buffer);

// Helper: format last error into string
static std::string FormatLastErrorStr(DWORD code) {
    LPWSTR msgBuf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&msgBuf, 0, NULL);
    std::string result;
    if (len && msgBuf) {
        // Convert to narrow string
        int nChars = WideCharToMultiByte(CP_ACP, 0, msgBuf, len, NULL, 0, NULL, NULL);
        if (nChars > 0) {
            result.resize(nChars);
            WideCharToMultiByte(CP_ACP, 0, msgBuf, len, &result[0], nChars, NULL, NULL);
            // Trim trailing newlines
            while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
                result.pop_back();
            }
        }
        LocalFree(msgBuf);
    } else {
        result = "Unknown error";
    }
    result += " (code ";
    result += std::to_string(code);
    result += ")";
    return result;
}

// Configuration
static const int BYTES_PER_LINE = 16;
 // Globals (transitional)
 // TODO: Integrate DirtyBitmap and remove direct flush reliance (see EDITOR_STATE_SPEC.md).
 static std::vector<unsigned char> g_buffer; // legacy unused storage; mapping replaces this
 static std::unique_ptr<FileAccess> g_fileAccess;
 static std::string g_currentFilePath;
 static HFONT g_hFont = nullptr;
 static EditorState g_editorState(BYTES_PER_LINE);
 // TODO: Integrate selective flush using g_dirtyBitmap.forEachDirtyRange in save_manager (future task).
 DirtyBitmap g_dirtyBitmap;
// Recent files globals
static RecentFiles g_recentFiles;
static std::wstring g_appDir; // application directory for persistence

// Search engine globals
static std::unique_ptr<SearchEngine> g_searchEngine;
static SearchRequest g_lastSearchRequest; // last successful/attempted request (pattern + mode)
static HWND g_hStatusBar = nullptr;


// Helper: Show open file dialog (wide-char variant for reliability)
bool OpenFileDialog(HWND hwnd, std::string& outPath) {
    wchar_t fileNameW[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd;
    ofn.lpstrFile    = fileNameW;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = L"All Files (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle   = L"Open File";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        // Convert wide path to narrow ANSI (lossy but sufficient baseline)
        std::wstring wide(fileNameW);
        outPath.assign(wide.begin(), wide.end());
        return true;
    } else {
        DWORD err = CommDlgExtendedError();
        if (err != 0) {
            char buf[128];
            sprintf(buf, "Open dialog failed (code %lu).", (unsigned long)err);
            MessageBox(hwnd, buf, "Open File", MB_OK | MB_ICONERROR);
        }
    }
    return false;
}

void UpdateStatusBar(HWND hwnd) {
    if (!g_hStatusBar) return;
    
    char statusText[256] = "No file";
    uint64_t fileSize = (g_fileAccess && g_fileAccess->isOpen()) ? g_fileAccess->size() : 0;
    
    if (fileSize > 0) {
        uint64_t cursor = g_editorState.cursorIndex;
        if (cursor >= fileSize) cursor = fileSize - 1;
        unsigned char byteVal = g_fileAccess->readByte(cursor);
        char fileSizeStr[32];
        char cursorStr[32];
        
        if (fileSize >= 1024 * 1024 * 1024) {
            sprintf(fileSizeStr, "%.2f GB", (double)fileSize / (1024.0 * 1024.0 * 1024.0));
        } else if (fileSize >= 1024 * 1024) {
            sprintf(fileSizeStr, "%.2f MB", (double)fileSize / (1024.0 * 1024.0));
        } else if (fileSize >= 1024) {
            sprintf(fileSizeStr, "%.2f KB", (double)fileSize / 1024.0);
        } else {
            sprintf(fileSizeStr, "%llu bytes", (unsigned long long)fileSize);
        }
        
        sprintf(cursorStr, "Offset: 0x%08llX  |  Byte: %02X  |  Size: %s", 
            (unsigned long long)cursor, byteVal, fileSizeStr);
        strcpy(statusText, cursorStr);
    }
    
    SendMessage(g_hStatusBar, SB_SETTEXT, 0, (LPARAM)statusText);
}

// Helper: Update scrollbar based on mapped file & window size (delegates to EditorState viewport fields)
void UpdateScrollBar(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    
    // Account for status bar height
    int statusBarHeight = 0;
    if (g_hStatusBar) {
        RECT sbRect;
        GetWindowRect(g_hStatusBar, &sbRect);
        statusBarHeight = sbRect.bottom - sbRect.top;
    }
    
    int height = rc.bottom - rc.top - statusBarHeight;
    // Recompute visible lines using current line height metric.
    int lineHeight = (g_editorState.lineHeight > 0) ? g_editorState.lineHeight : 16;
    g_editorState.visibleLines = max(1, (height - 24) / lineHeight); // 24 for header

    uint64_t fileSize = (g_fileAccess && g_fileAccess->isOpen()) ? g_fileAccess->size() : 0;
    int totalLines = (int)((fileSize + BYTES_PER_LINE - 1) / BYTES_PER_LINE);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = max(0, totalLines - 1);
    si.nPage = g_editorState.visibleLines;
    if (g_editorState.firstVisibleLine > si.nMax - (int)si.nPage + 1) {
        g_editorState.firstVisibleLine = max(0, si.nMax - (int)si.nPage + 1);
    }
    si.nPos = g_editorState.firstVisibleLine;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

// Helper: Ensure cursor line is visible (wrapper delegates to EditorState)
void EnsureCursorVisible(HWND hwnd) {
    uint64_t fileSize = (g_fileAccess && g_fileAccess->isOpen()) ? g_fileAccess->size() : 0;
    int before = g_editorState.firstVisibleLine;
    g_editorState.ensureCursorVisible(fileSize);
    if (g_editorState.firstVisibleLine != before) {
        UpdateScrollBar(hwnd);
    }
    UpdateStatusBar(hwnd);
}

// Helper: Render hex view (uses scrolling state)
void RenderHexView(HDC hdc, RECT& rc) {
    char textBuf[128];
    const int headerHeight = 24;
    int y = headerHeight;

    // Draw header background
    HBRUSH headerBrush = CreateSolidBrush(RGB(240, 240, 245));
    FillRect(hdc, &rc, headerBrush);
    
    // Draw header row
    SetBkColor(hdc, RGB(240, 240, 245));
    SetTextColor(hdc, RGB(80, 80, 80));
    LOGFONT lfHeader = {0};
    lfHeader.lfHeight = -12;
    lfHeader.lfWeight = FW_BOLD;
    strcpy(lfHeader.lfFaceName, "Segoe UI");
    HFONT headerFont = CreateFontIndirect(&lfHeader);
    HFONT oldFont = (HFONT)SelectObject(hdc, headerFont);
    
    TextOutA(hdc, 10, 6, "Offset", 6);
    TextOutA(hdc, 90, 6, "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F", 48);
    int asciiX = 90 + (BYTES_PER_LINE * g_editorState.charWidth * 3) + 20;
    TextOutA(hdc, asciiX, 6, "ASCII", 5);
    
    // Draw header line
    HPEN headerLinePen = CreatePen(PS_SOLID, 1, RGB(180, 180, 190));
    HPEN oldPen = (HPEN)SelectObject(hdc, headerLinePen);
    MoveToEx(hdc, 0, headerHeight, NULL);
    LineTo(hdc, rc.right, headerHeight);
    SelectObject(hdc, oldPen);
    DeleteObject(headerLinePen);
    
    SelectObject(hdc, oldFont);
    DeleteObject(headerFont);

    if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) {
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        const char* msg = "No file loaded. Use File -> Open or press Ctrl+O";
        TextOut(hdc, 10, y + 20, msg, lstrlen(msg));
        DeleteObject(headerBrush);
        return;
    }

    uint64_t totalBytes64 = g_fileAccess->size();
    size_t totalBytes = (size_t)totalBytes64;
    int totalLines = (int)((totalBytes + BYTES_PER_LINE - 1) / BYTES_PER_LINE);
    int startLine = g_editorState.firstVisibleLine;
    int linesToDraw = min(g_editorState.visibleLines, totalLines - startLine);

    // Pre-load window to cover all bytes that will be rendered (optimization for windowed mapping)
    if (g_fileAccess && g_fileAccess->isOpen()) {
        uint64_t startOffset = (uint64_t)startLine * BYTES_PER_LINE;
        uint64_t bytesToAccess = (uint64_t)linesToDraw * BYTES_PER_LINE;
        if (startOffset + bytesToAccess > totalBytes64) {
            bytesToAccess = totalBytes64 - startOffset;
        }
        g_fileAccess->prepareAccess(startOffset, bytesToAccess);
    }

    // Set default colors
    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
    SetTextColor(hdc, RGB(0, 80, 0));  // Dark green for hex bytes

    for (int line = 0; line < linesToDraw; ++line) {
        size_t baseIndex = (size_t)((startLine + line) * BYTES_PER_LINE);

        // Offset column - gray text
        SetTextColor(hdc, RGB(100, 100, 180));
        sprintf(textBuf, "%08X:", (unsigned int)baseIndex);
        TextOut(hdc, 10, y, textBuf, 9);

        // Hex column
        SetTextColor(hdc, RGB(0, 80, 0));
        int hexX = 90;
        for (int i = 0; i < BYTES_PER_LINE; ++i) {
            size_t idx = baseIndex + i;
            if (idx >= totalBytes) break;
            unsigned char b = g_fileAccess->readByte(idx);
            sprintf(textBuf, "%02X", b);

            if (idx == g_editorState.cursorIndex) {
                SetBkColor(hdc, RGB(51, 153, 255));  // Blue highlight
                SetTextColor(hdc, RGB(255, 255, 255));
                TextOut(hdc, hexX, y, textBuf, 2);
                SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                SetTextColor(hdc, RGB(0, 80, 0));
            } else {
                TextOut(hdc, hexX, y, textBuf, 2);
            }
            hexX += g_editorState.charWidth * 3;
        }

        // ASCII column - use a different color for visibility
        SetTextColor(hdc, RGB(180, 0, 0));  // Dark red for ASCII
        int asciiX = 90 + (BYTES_PER_LINE * g_editorState.charWidth * 3) + 20;
        for (int i = 0; i < BYTES_PER_LINE; ++i) {
            size_t idx = baseIndex + i;
            if (idx >= totalBytes) break;
            unsigned char c = g_fileAccess->readByte(idx);
            char ch = (c >= 32 && c < 127) ? (char)c : '.';
            textBuf[0] = ch;
            textBuf[1] = '\0';

            if (idx == g_editorState.cursorIndex) {
                SetBkColor(hdc, RGB(51, 204, 102));  // Green highlight
                SetTextColor(hdc, RGB(0, 0, 0));
                TextOut(hdc, asciiX, y, textBuf, 1);
                SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                SetTextColor(hdc, RGB(180, 0, 0));
            } else {
                TextOut(hdc, asciiX, y, textBuf, 1);
            }
            asciiX += g_editorState.charWidth;
        }

        y += g_editorState.lineHeight;
    }
    
    DeleteObject(headerBrush);
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd) {
    const char CLASS_NAME[] = "HexEditorWindowClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        "Hex Editor",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr,
        LoadMenu(hInstance, MAKEINTRESOURCE(IDR_MAINMENU)),
        hInstance,
        nullptr);

    if (!hwnd) return 0;

    HACCEL hAccel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDR_ACCEL));

    ShowWindow(hwnd, nShowCmd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return 0;
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        LOGFONT lf = {};
        lf.lfHeight = -16;
        lf.lfWeight = FW_NORMAL;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpy(lf.lfFaceName, "Consolas");
        g_hFont = CreateFontIndirect(&lf);

        HDC hdc = GetDC(hwnd);
        HFONT old = (HFONT)SelectObject(hdc, g_hFont);
        TEXTMETRIC tm;
        GetTextMetrics(hdc, &tm);
        g_editorState.lineHeight = tm.tmHeight + tm.tmExternalLeading;
        SIZE sz;
        GetTextExtentPoint32(hdc, "0", 1, &sz);
        g_editorState.charWidth = sz.cx;
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);

        // Determine user data directory for persistence (AppData is always writable)
        wchar_t appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
            g_appDir = std::wstring(appDataPath) + L"\\HexEditor";
            // Create directory if it doesn't exist
            CreateDirectoryW(g_appDir.c_str(), NULL);
        } else {
            // Fallback to app directory
            wchar_t modulePath[MAX_PATH];
            DWORD pathLen = GetModuleFileNameW(NULL, modulePath, MAX_PATH);
            if (pathLen > 0) {
                std::wstring full(modulePath);
                size_t slashPos = full.find_last_of(L"\\/");
                g_appDir = (slashPos != std::wstring::npos) ? full.substr(0, slashPos) : full;
            }
        }

        // Load recent files (silent failure per spec)
        std::wstring rfError;
        g_recentFiles.load(g_appDir, rfError);

        // Build initial Recent Files menu
        BuildRecentFilesMenu(GetMenu(hwnd), g_recentFiles);

        UpdateScrollBar(hwnd);

        // Create status bar
        g_hStatusBar = CreateWindowExA(
            0,
            STATUSCLASSNAMEA,
            "",
            WS_CHILD | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hwnd,
            (HMENU)0,
            (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
            nullptr);
        if (g_hStatusBar) {
            ShowWindow(g_hStatusBar, SW_SHOW);
            UpdateStatusBar(hwnd);
        }

        // Initialize search engine (FileAccess may be null until a file is opened)
        if (!g_searchEngine) {
            g_searchEngine = std::make_unique<SearchEngine>(g_fileAccess.get());
            g_searchEngine->setNotifyWindow(hwnd);
        } else {
            g_searchEngine->setNotifyWindow(hwnd);
            g_searchEngine->setAccess(g_fileAccess.get());
        }
    } return 0;

    case WM_SIZE:
        UpdateScrollBar(hwnd);
        if (g_hStatusBar) {
            SendMessage(g_hStatusBar, WM_SIZE, 0, 0);
        }
        return 0;

    case WM_DESTROY:
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
        if (g_hStatusBar) { DestroyWindow(g_hStatusBar); g_hStatusBar = nullptr; }
        PostQuitMessage(0);
        return 0;

    case WM_VSCROLL: {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        uint64_t fileSize = (g_fileAccess && g_fileAccess->isOpen()) ? g_fileAccess->size() : 0;
        int totalLines = (int)((fileSize + BYTES_PER_LINE - 1) / BYTES_PER_LINE);
        int oldPos = g_editorState.firstVisibleLine;

        switch (LOWORD(wParam)) {
        case SB_LINEUP:     g_editorState.firstVisibleLine = max(0, g_editorState.firstVisibleLine - 1); break;
        case SB_LINEDOWN:   g_editorState.firstVisibleLine = min(totalLines - 1, g_editorState.firstVisibleLine + 1); break;
        case SB_PAGEUP:     g_editorState.firstVisibleLine = max(0, g_editorState.firstVisibleLine - g_editorState.visibleLines); break;
        case SB_PAGEDOWN:   g_editorState.firstVisibleLine = min(totalLines - 1, g_editorState.firstVisibleLine + g_editorState.visibleLines); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: g_editorState.firstVisibleLine = si.nTrackPos; break;
        default: break;
        }

        if (g_editorState.firstVisibleLine != oldPos) {
            UpdateScrollBar(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
    } return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        HBRUSH bg = (HBRUSH)(COLOR_WINDOW+1);
        FillRect(memDC, &rc, bg);

        HFONT oldFont = nullptr;
        if (g_hFont) oldFont = (HFONT)SelectObject(memDC, g_hFont);

        RenderHexView(memDC, rc);

        if (oldFont) SelectObject(memDC, oldFont);
        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
    } return 0;

    case WM_COMMAND: {
        UINT cmd = LOWORD(wParam);

        // Recent Files dynamic command handling
        if (IsRecentFileCommand(cmd)) {
            int idx = CommandToRecentIndex(cmd);
            const auto& entries = g_recentFiles.entries();
            if (idx >= 0 && (size_t)idx < entries.size()) {
                const RecentFileEntry& e = entries[idx];
                if (_waccess(e.path.c_str(), 0) != 0) {
                    int mb = MessageBoxW(hwnd, L"File missing. Remove from list? Yes=Remove, No=Retry", L"Missing File", MB_ICONQUESTION | MB_YESNO);
                    if (mb == IDYES) {
                        g_recentFiles.removeAt(idx);
                        std::wstring saveError;
                        g_recentFiles.save(g_appDir, saveError); // failure ignored here
                        BuildRecentFilesMenu(GetMenu(hwnd), g_recentFiles);
                    }
                    return 0;
                }
                std::string narrow(e.path.begin(), e.path.end());
                if (OpenFileWithMapping(narrow)) {
                    // Restore editor state
                    g_editorState.cursorIndex = e.cursorIndex;
                    g_editorState.firstVisibleLine = e.firstVisibleLine;
                    if (g_fileAccess && g_fileAccess->isOpen()) {
                        uint64_t sz = g_fileAccess->size();
                        if (g_editorState.cursorIndex >= sz) {
                            g_editorState.cursorIndex = (sz > 0) ? (sz - 1) : 0;
                        }
                        int totalLines = (int)((sz + BYTES_PER_LINE - 1) / BYTES_PER_LINE);
                        if (g_editorState.firstVisibleLine > totalLines - 1) {
                            g_editorState.firstVisibleLine = max(0, totalLines - 1);
                        }
                    }

                    // Rebind search engine to new file
                    if (g_searchEngine) {
                        g_searchEngine->setAccess(g_fileAccess.get());
                    } else {
                        g_searchEngine = std::make_unique<SearchEngine>(g_fileAccess.get());
                        g_searchEngine->setNotifyWindow(hwnd);
                    }

                    EnsureCursorVisible(hwnd);
                    UpdateScrollBar(hwnd);
                    InvalidateRect(hwnd, NULL, TRUE);

                    // Re-record to move entry to front with updated timestamp & clipped state
                    g_recentFiles.recordOpen(e.path, g_editorState.cursorIndex, g_editorState.firstVisibleLine);
                    std::wstring saveError;
                    if (!g_recentFiles.save(g_appDir, saveError)) {
                        std::string narrow(saveError.begin(), saveError.end());
                        MessageBox(hwnd, ("Failed to save recent files: " + narrow).c_str(), "Error", MB_ICONERROR);
                    }
                    BuildRecentFilesMenu(GetMenu(hwnd), g_recentFiles);
                } else {
                    MessageBox(hwnd, "Failed to open file.", "Error", MB_ICONERROR);
                }
            }
            return 0;
        }

        switch (cmd) {
        case ID_FILE_OPEN: {
            std::string path;
            if (OpenFileDialog(hwnd, path)) {
                if (OpenFileWithMapping(path)) {
                    g_editorState.resetOnFileOpen(g_fileAccess->size());
                    UpdateScrollBar(hwnd);
                    UpdateStatusBar(hwnd);
                    InvalidateRect(hwnd, NULL, TRUE);
                    // Persist recent file entry
                    std::wstring wpath(path.begin(), path.end());
                    g_recentFiles.recordOpen(wpath, g_editorState.cursorIndex, g_editorState.firstVisibleLine);
                    std::wstring saveError;
                    if (!g_recentFiles.save(g_appDir, saveError)) {
                        std::string narrow(saveError.begin(), saveError.end());
                        MessageBox(hwnd, ("Failed to save recent files: " + narrow).c_str(), "Error", MB_ICONERROR);
                    }
                    BuildRecentFilesMenu(GetMenu(hwnd), g_recentFiles);

                    // Rebind search engine to new file
                    if (g_searchEngine) {
                        g_searchEngine->setAccess(g_fileAccess.get());
                    } else {
                        g_searchEngine = std::make_unique<SearchEngine>(g_fileAccess.get());
                        g_searchEngine->setNotifyWindow(hwnd);
                    }
                } else {
                    MessageBox(hwnd, "Failed to open file mapping.", "Error", MB_ICONERROR);
                }
            } else {
                // User cancelled or dialog failed
            }
        } break;

        case ID_FILE_SAVE:
            if (g_currentFilePath.empty()) {
                MessageBox(hwnd, "No file loaded to save.", "Info", MB_OK);
            } else {
                SaveToFile(g_currentFilePath.c_str(), g_buffer);
            }
            break;

        case ID_EDIT_COPY: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) break;
            uint8_t byteVal = g_fileAccess->readByte(g_editorState.cursorIndex);
            char hexStr[3];
            sprintf(hexStr, "%02X", byteVal);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 3);
            if (hMem) {
                char* p = (char*)GlobalLock(hMem);
                memcpy(p, hexStr, 3);
                GlobalUnlock(hMem);
                OpenClipboard(hwnd);
                EmptyClipboard();
                SetClipboardData(CF_TEXT, hMem);
                CloseClipboard();
            }
        } break;

        case ID_EDIT_PASTE: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) {
                MessageBox(hwnd, "No file loaded.", "Paste", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (!IsClipboardFormatAvailable(CF_TEXT)) {
                MessageBox(hwnd, "No text in clipboard.", "Paste", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (!OpenClipboard(hwnd)) break;
            HGLOBAL hMem = GetClipboardData(CF_TEXT);
            if (hMem) {
                const char* p = (const char*)GlobalLock(hMem);
                if (p) {
                    std::string clipText(p);
                    GlobalUnlock(hMem);
                    size_t len = clipText.length();
                    size_t i = 0;
                    while (i < len) {
                        while (i < len && !isxdigit((unsigned char)clipText[i])) i++;
                        if (i >= len) break;
                        unsigned int byteVal = 0;
                        int digits = 0;
                        while (i < len && isxdigit((unsigned char)clipText[i]) && digits < 2) {
                            char c = clipText[i];
                            if (c >= '0' && c <= '9') byteVal = byteVal * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f') byteVal = byteVal * 16 + 10 + (c - 'a');
                            else if (c >= 'A' && c <= 'F') byteVal = byteVal * 16 + 10 + (c - 'A');
                            i++;
                            digits++;
                        }
                        if (digits > 0) {
                            if (g_editorState.cursorIndex < g_fileAccess->size()) {
                                g_fileAccess->writeByte(g_editorState.cursorIndex, (uint8_t)byteVal);
                                g_dirtyBitmap.mark(g_editorState.cursorIndex);
                                g_editorState.cursorIndex++;
                            } else {
                                break;
                            }
                        }
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            CloseClipboard();
        } break;

        case ID_EDIT_CUT: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) break;
            uint8_t byteVal = g_fileAccess->readByte(g_editorState.cursorIndex);
            char hexStr[3];
            sprintf(hexStr, "%02X", byteVal);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 3);
            if (hMem) {
                char* p = (char*)GlobalLock(hMem);
                memcpy(p, hexStr, 3);
                GlobalUnlock(hMem);
                OpenClipboard(hwnd);
                EmptyClipboard();
                SetClipboardData(CF_TEXT, hMem);
                CloseClipboard();
            }
            g_fileAccess->writeByte(g_editorState.cursorIndex, 0);
            g_dirtyBitmap.mark(g_editorState.cursorIndex);
            InvalidateRect(hwnd, NULL, FALSE);
        } break;

        case ID_FILE_EXIT:
            PostQuitMessage(0);
            break;
        case ID_EDIT_FIND: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) {
                MessageBox(hwnd, "No file loaded.", "Find", MB_OK | MB_ICONINFORMATION);
                break;
            }
            SearchRequest req;
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            if (ShowFindDialog(hInst, hwnd, req)) {
                req.startOffset = g_editorState.cursorIndex;
                g_lastSearchRequest = req;
                if (g_searchEngine) {
                    // Quick synchronous try first
                    SearchResult sr = g_searchEngine->findNext(req);
                    if (sr.found) {
                        g_editorState.cursorIndex = sr.offset;
                        EnsureCursorVisible(hwnd);
                        InvalidateRect(hwnd, NULL, FALSE);
                    } else {
                        if (!g_searchEngine->startAsyncScan(req)) {
                            MessageBox(hwnd, "Search already running.", "Find", MB_OK | MB_ICONINFORMATION);
                        } else {
                            SetWindowText(hwnd, "Hex Editor - Searching...");
                        }
                    }
                }
            } else {
                // User cancelled dialog
            }
        } break;
        case ID_EDIT_FIND_NEXT: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) {
                MessageBox(hwnd, "No file loaded.", "Find Next", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (g_lastSearchRequest.pattern.empty()) {
                MessageBox(hwnd, "No previous search pattern. Use Find...", "Find Next", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (g_searchEngine && g_searchEngine->isRunning()) {
                MessageBox(hwnd, "Search already running.", "Find Next", MB_OK | MB_ICONINFORMATION);
                break;
            }
            uint64_t nextStart = g_editorState.cursorIndex;
            if (nextStart + 1 < g_fileAccess->size()) nextStart++;
            g_lastSearchRequest.startOffset = nextStart;
            if (g_searchEngine) {
                SearchResult sr = g_searchEngine->findNext(g_lastSearchRequest);
                if (sr.found) {
                    g_editorState.cursorIndex = sr.offset;
                    EnsureCursorVisible(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    if (!g_searchEngine->startAsyncScan(g_lastSearchRequest)) {
                        MessageBox(hwnd, "Unable to start async search.", "Find Next", MB_OK | MB_ICONINFORMATION);
                    } else {
                        SetWindowText(hwnd, "Hex Editor - Searching...");
                    }
                }
            }
        } break;
        case ID_EDIT_GOTO: {
            if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) {
                MessageBox(hwnd, "No file loaded.", "Go To", MB_OK | MB_ICONINFORMATION);
                break;
            }
            uint64_t offset = 0;
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
            if (ShowGoToDialog(hInst, hwnd, offset)) {
                uint64_t fileSize = g_fileAccess->size();
                if (offset >= fileSize) {
                    char msg[128];
                    sprintf(msg, "Offset %llu is beyond file size %llu.", (unsigned long long)offset, (unsigned long long)fileSize);
                    MessageBox(hwnd, msg, "Go To", MB_OK | MB_ICONWARNING);
                    break;
                }
                g_editorState.cursorIndex = offset;
                g_editorState.editHighNibble = true;
                EnsureCursorVisible(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } break;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (!g_fileAccess || !g_fileAccess->isOpen() || g_fileAccess->size() == 0) break;
        switch (wParam) {
        case VK_LEFT:
            if (g_editorState.cursorIndex > 0) g_editorState.cursorIndex--;
            g_editorState.editHighNibble = true;
            EnsureCursorVisible(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_RIGHT:
            if (g_editorState.cursorIndex + 1 < g_fileAccess->size()) g_editorState.cursorIndex++;
            g_editorState.editHighNibble = true;
            EnsureCursorVisible(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_UP:
            if (g_editorState.cursorIndex >= (uint64_t)BYTES_PER_LINE)
                g_editorState.cursorIndex -= BYTES_PER_LINE;
            g_editorState.editHighNibble = true;
            EnsureCursorVisible(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_DOWN:
            if (g_editorState.cursorIndex + BYTES_PER_LINE < g_fileAccess->size())
                g_editorState.cursorIndex += BYTES_PER_LINE;
            g_editorState.editHighNibble = true;
            EnsureCursorVisible(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        default:
            // Hex digit input path delegated to EditController.
            // TODO: Undo/redo integration point.
            if (EditController::applyHexInput(*g_fileAccess, g_editorState, (int)wParam, g_fileAccess->size())) {
                EnsureCursorVisible(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            break;
        }
        break;

    case WM_SEARCH_PROGRESS: {
        if (g_fileAccess && g_fileAccess->isOpen()) {
            uint64_t pos = (uint64_t)lParam;
            uint64_t sz = g_fileAccess->size();
            if (sz > 0) {
                int percent = (int)((pos * 100) / sz);
                if (percent > 99) percent = 99;
                char title[128];
                sprintf(title, "Hex Editor - Searching... %d%%", percent);
                SetWindowText(hwnd, title);
            }
        }
        return 0;
    }

    case WM_SEARCH_COMPLETE: {
        SetWindowText(hwnd, "Hex Editor");
        if (wParam == 1) {
            uint64_t off = (uint64_t)lParam;
            g_editorState.cursorIndex = off;
            EnsureCursorVisible(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            MessageBox(hwnd, "Pattern not found.", "Find", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

 // Save (flush) mapped file (legacy signature retained; buffer ignored - mapping used)
 bool SaveToFile(const char* filePath, const std::vector<unsigned char>& /*buffer*/) {
     // TODO: Integrate DirtyBitmap selective flush into new SaveManager; undo/redo & partial page optimization later.
     if (!g_fileAccess || !g_fileAccess->isOpen()) {
         MessageBox(NULL, "No mapped file to save.", "Info", MB_OK);
         return false;
     }
     std::wstring error;
     uint64_t flushedPages = 0;
     bool success = SaveManager::flushDirtyPages(*g_fileAccess, g_dirtyBitmap, error, flushedPages);
     if (success) {
         if (flushedPages == 0) {
             MessageBox(NULL, "No changes to save.", "Info", MB_OK);
         } else {
             std::string msg = "Saved changes (" + std::to_string(flushedPages) + " pages flushed).";
             MessageBox(NULL, msg.c_str(), "Info", MB_OK);
         }
         return true;
     }
     // flushDirtyPages() failed - report error (no fallback because:
     // 1. In windowed mode, flush() only flushes current window
     // 2. If selective flush failed, full flush likely will too
     std::string narrowError(error.begin(), error.end());
     MessageBox(NULL, ("Failed to save file: " + narrowError).c_str(), "Error", MB_ICONERROR);
     return false;
 }

// Deprecated legacy read function (unused)
bool OpenAndReadFile(const char* filePath, std::vector<unsigned char>& buffer) {
    // Deprecated: memory-mapped implementation now used; return false to signal unused.
    return false;
}

// Open file with memory mapping
bool OpenFileWithMapping(const std::string& filePath) {
    std::wstring wpath(filePath.begin(), filePath.end());
    std::wstring error;
    std::unique_ptr<MemoryMappedFileAccess> access(new MemoryMappedFileAccess());

    // Open the file first to check size before mapping
    HANDLE hTest = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTest == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::string errMsg = "Failed to open file: " + FormatLastErrorStr(err);
        MessageBox(NULL, errMsg.c_str(), "Error", MB_ICONERROR);
        return false;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hTest, &liSize)) {
        DWORD err = GetLastError();
        std::string errMsg = "Failed to get file size: " + FormatLastErrorStr(err);
        MessageBox(NULL, errMsg.c_str(), "Error", MB_ICONERROR);
        CloseHandle(hTest);
        return false;
    }
    CloseHandle(hTest);

    // Check file size limit (10 GB as per ARCHITECTURE.md)
    constexpr uint64_t MAX_FILE_SIZE = 10ULL * 1024 * 1024 * 1024; // 10 GB
    uint64_t fileSize = static_cast<uint64_t>(liSize.QuadPart);
    if (fileSize > MAX_FILE_SIZE) {
        char msg[256];
        sprintf(msg, "File size (%.2f GB) exceeds the 10 GB limit.\nThe file cannot be opened.", (double)fileSize / (1024.0 * 1024.0 * 1024.0));
        MessageBox(NULL, msg, "File Too Large", MB_ICONERROR);
        return false;
    }

    if (!access->open(wpath, error)) {
        std::string narrowError(error.begin(), error.end());
        MessageBox(NULL, ("Failed to open file: " + narrowError).c_str(), "Error", MB_ICONERROR);
        g_dirtyBitmap.clear(); // Clear bitmap on failed open.
        return false;
    }
    g_fileAccess = std::move(access);
    g_currentFilePath = filePath;
    g_editorState.resetOnFileOpen(g_fileAccess->size());
    g_dirtyBitmap.initialize(g_fileAccess->size()); // initialize page-level dirty tracking
    // Scrollbar will be updated by caller after success.
    return true;
}