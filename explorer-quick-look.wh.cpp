// ==WindhawkMod==
// @id              explorer-quick-look
// @name            Explorer quick look
// @description     Shows a quick preview of the selected file in a folder window when Space is pressed
// @version         1.0
// @author          nikitor32
// @github          https://github.com/nikitor32
// @compilerOptions -lgdi32 -lole32 -luuid -lshlwapi -lmsimg32 -ldwmapi
// @include         windhawk.exe
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.

// ==WindhawkModReadme==
/*
# Explorer quick look

Press Space on a selected file in File Explorer to get a quick preview
of it: text files, documents and videos are rendered by the Windows
preview handlers, images fall back to their thumbnail, and everything
else shows the file's icon and name.

The preview window never takes focus: the keyboard stays in File
Explorer, and pressing Space again, Esc, clicking somewhere else or
changing the selection closes the preview.

The mod runs in a dedicated Windhawk process (as a tool mod) and does
not inject into File Explorer.

What shows up in the preview depends on what Windows has registered
for the file type: some formats (for example PDFs, depending on the
installed reader) may fall back to the file's icon. File Explorer
windows running as administrator are not seen by the keyboard hook (a
Windows restriction), so the preview does not open there.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enable quick look
  $description: Turn off to stop reacting to the Space key entirely.
- sizePercent: 70
  $name: Window size
  $description: Preview size as a percentage of the monitor's work area (40-90, default 70).
- showCaption: true
  $name: Show caption
  $description: Show the header strip with the file name and size.
- aspectRatio: 133
  $name: Aspect ratio
  $description: Window width per height, times 100 (100-250, default 133 means 4:3).
*/
// ==/WindhawkModSettings==

#include <stdio.h>
#include <windows.h>
#include <shlobj.h>    // IShellView, IFolderView2, IShellItemArray
#include <servprov.h>  // IServiceProvider
#include <initguid.h>  // from here on DEFINE_GUID defines instead of declares
#include <exdisp.h>    // IShellWindows, IWebBrowser2, CLSID_ShellWindows
#include <shlguid.h>     // SID_STopLevelBrowser, BHID_ThumbnailHandler
#include <shlwapi.h>     // SHCreateStreamOnFileEx
#include <thumbcache.h>  // IThumbnailProvider
#include <dwmapi.h>      // DwmSetWindowAttribute (rounded corners)

#include <atomic>
#include <string>

// The setting is updated on the main thread while the keyboard hook
// reads it on the worker thread, so an atomic type is used to avoid a
// data race.
std::atomic<bool> g_enabled{true};
std::atomic<int> g_sizePercent{70};    // window size, % of the work area
std::atomic<bool> g_showCaption{true}; // header strip with name and size
std::atomic<int> g_aspectRatio{133};   // width per height * 100 (4:3)

// The mod has two threads: a lightweight hook thread that only does
// cheap checks and posts requests, and an STA thread that owns all COM
// and window work. Blocking in a low-level hook callback (a slow
// preview handler render, for example) would stall input for the whole
// session and could get the hook silently removed by Windows.
HANDLE g_stopEvent = nullptr;
HANDLE g_staReadyEvent = nullptr;  // STA thread: queue and window are up
HANDLE g_staThreadHandle = nullptr;
HANDLE g_hookThreadHandle = nullptr;
std::atomic<bool> g_staReady{false};

// Read by the hook thread, written by the STA thread.
std::atomic<bool> g_previewOpen{false};

// Message-only window of the STA thread; the hook posts show/close
// requests here. Created before the hook thread starts.
HWND g_msgWindow = nullptr;

// Owned by the hook thread: callbacks, the repeat filter and the heal
// timer all run there.
HHOOK g_keyboardHook = nullptr;
bool g_spaceDown = false;      // filters Space auto-repeat
bool g_spaceSwallowed = false; // Space keydown this mod has consumed
bool g_escDown = false;        // filters Esc auto-repeat
bool g_escSwallowed = false;   // Esc keydown this mod has consumed

// Owned by the STA thread.
HWND g_previewWindow = nullptr;  // the visible quick look window
IPreviewHandler* g_previewHandler = nullptr;
IStream* g_previewStream = nullptr;  // held until the handler unloads

// Fallback content owned by the STA thread: the file's thumbnail
// (the shell's path B) and the placeholder icon (path C).
HBITMAP g_thumbnail = nullptr;
bool g_thumbnailHasAlpha = false;  // the bitmap carries per-pixel alpha
HICON g_fileIcon = nullptr;

// Full path of the file the preview is showing.
std::wstring g_previewFilePath;

// Explorer window this preview belongs to (STA thread only): compared
// against the foreground window and checked for aliveness by the
// auto-close timer.
HWND g_explorerWindow = nullptr;

// Header line built when the preview opens: file name plus size.
std::wstring g_previewHeaderText;

// The hook callback never does window or COM work itself; it posts one
// of these to the worker's message loop and returns.
constexpr UINT WM_SHOW_PREVIEW = WM_APP + 1;
constexpr UINT WM_CLOSE_PREVIEW = WM_APP + 2;

constexpr wchar_t kPreviewClass[] = L"explorer-quick-look-preview";

// Clamps an integer setting before storing it: the settings UI limits
// the range, but the code must not depend on that.
int ClampSetting(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

void LoadSettings() {
    g_enabled.store(Wh_GetIntSetting(L"enabled") != 0,
                    std::memory_order_relaxed);
    g_sizePercent.store(
        ClampSetting(Wh_GetIntSetting(L"sizePercent"), 40, 90),
        std::memory_order_relaxed);
    g_showCaption.store(Wh_GetIntSetting(L"showCaption") != 0,
                        std::memory_order_relaxed);
    g_aspectRatio.store(
        ClampSetting(Wh_GetIntSetting(L"aspectRatio"), 100, 250),
        std::memory_order_relaxed);
}

// Cheap context check, safe to run inside the hook callback: a folder
// window must be the foreground one, and the focus must not be in a
// text field (rename mode, address bar, search box) where Space is
// regular text input.
bool IsQuickLookContext() {
    HWND fg = GetForegroundWindow();
    WCHAR className[64];
    if (!GetClassNameW(fg, className, ARRAYSIZE(className))) {
        return false;
    }
    if (wcscmp(className, L"CabinetWClass") != 0 &&
        wcscmp(className, L"ExploreWClass") != 0) {
        return false;
    }

    // Query the focus of the foreground thread explicitly: passing 0
    // here would be ambiguous about which thread is meant.
    DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gui{.cbSize = sizeof(GUITHREADINFO)};
    if (fgThread && GetGUIThreadInfo(fgThread, &gui) && gui.hwndFocus) {
        WCHAR focusClass[64];
        if (GetClassNameW(gui.hwndFocus, focusClass,
                          ARRAYSIZE(focusClass)) &&
            wcscmp(focusClass, L"Edit") == 0) {
            return false;
        }
    }
    return true;
}

// A low-level hook can miss a keyup if some other hook earlier in the
// chain eats it; the repeat filter would then stay "down" forever and
// the preview would stop reacting to Space. A timer reconciles the
// internal state with the real key state, so a lost keyup heals itself.
void HealKeyStates() {
    if (g_spaceDown && !(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        g_spaceDown = false;
        g_spaceSwallowed = false;
        Wh_Log(L"Space state healed (keyup was missed)");
    }
    if (g_escDown && !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        g_escDown = false;
        g_escSwallowed = false;
        Wh_Log(L"Esc state healed (keyup was missed)");
    }
}

// Layout of the preview window: header strip on top (file name),
// content area in the middle (the preview handler draws there when one
// is active), hint strip at the bottom. Shared by the paint code and
// the handler's SetRect, so the handler never gets a rectangle that our
// own text would cover.
void GetPreviewLayout(HWND hwnd,
                      RECT* headerRect,
                      RECT* contentRect,
                      RECT* hintRect) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    constexpr int kMargin = 8;
    // The caption can be switched off; the content area then starts
    // right below the top margin.
    const int kHeaderHeight =
        g_showCaption.load(std::memory_order_relaxed) ? 28 : 0;
    constexpr int kHintHeight = 24;
    RECT header{kMargin, kMargin, rc.right - kMargin, kMargin + kHeaderHeight};
    RECT hint{kMargin, rc.bottom - kMargin - kHintHeight, rc.right - kMargin,
              rc.bottom - kMargin};
    RECT content{kMargin, header.bottom + (kHeaderHeight ? 4 : 0),
                 rc.right - kMargin, hint.top - 4};
    if (headerRect) {
        *headerRect = header;
    }
    if (contentRect) {
        *contentRect = content;
    }
    if (hintRect) {
        *hintRect = hint;
    }
}

// Reads a REG_SZ value of a key (valueName = nullptr reads the
// default value); empty on any failure.
std::wstring ReadRegStringValue(HKEY root,
                                const std::wstring& subKey,
                                const wchar_t* valueName) {
    DWORD size = 0;
    LSTATUS status =
        RegGetValueW(root, subKey.c_str(), valueName, RRF_RT_REG_SZ, nullptr,
                     nullptr, &size);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::wstring();
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    status = RegGetValueW(root, subKey.c_str(), valueName, RRF_RT_REG_SZ,
                          nullptr, value.data(), &size);
    if (status != ERROR_SUCCESS) {
        return std::wstring();
    }
    value.resize(wcslen(value.c_str()));
    return value;
}

// Reads the preview handler CLSID registered under a key (an
// extension, a ProgID or a perceived type); empty when there is none.
std::wstring ReadPreviewHandlerClsid(const std::wstring& baseKey) {
    return ReadRegStringValue(HKEY_CLASSES_ROOT,
                              baseKey + L"\\ShellEx\\{8895b1c6-b41f-"
                                        L"4c1c-a562-0d564250836f}",
                              nullptr);
}

// Resolves the preview handler CLSID for a file the way the shell
// does. Every step was verified against this machine's registry:
//  1. the extension itself — works for .pdf/.docx;
//  2. the file's UserChoice ProgID (HKCU FileExts);
//  3. the legacy ProgID of the extension — works for .mp4
//     (WMP11.AssocFile.MP4);
//  4. the perceived type under SystemFileAssociations — this is where
//     .txt and friends get the Windows TXT Previewer (.cpp is
//     PerceivedType=text too and is previewed as plain text).
// .png/.jpg resolve to nothing here and fall through to the thumbnail
// route.
bool FindPreviewHandlerClsid(const std::wstring& filePath, CLSID* clsid) {
    size_t dot = filePath.rfind(L'.');
    size_t slash = filePath.rfind(L'\\');
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
        return false;
    }
    std::wstring ext = filePath.substr(dot);

    std::wstring clsidText = ReadPreviewHandlerClsid(ext);
    if (clsidText.empty()) {
        std::wstring progId = ReadRegStringValue(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
            L"FileExts\\" +
                ext + L"\\UserChoice",
            L"ProgID");
        if (!progId.empty()) {
            clsidText = ReadPreviewHandlerClsid(progId);
        }
    }
    if (clsidText.empty()) {
        std::wstring progId = ReadRegStringValue(HKEY_CLASSES_ROOT, ext, nullptr);
        if (!progId.empty()) {
            clsidText = ReadPreviewHandlerClsid(progId);
        }
    }
    if (clsidText.empty()) {
        std::wstring perceivedType =
            ReadRegStringValue(HKEY_CLASSES_ROOT, ext, L"PerceivedType");
        if (!perceivedType.empty()) {
            clsidText = ReadPreviewHandlerClsid(
                L"SystemFileAssociations\\" + perceivedType);
        }
    }
    if (clsidText.empty()) {
        return false;
    }
    return SUCCEEDED(CLSIDFromString(clsidText.c_str(), clsid));
}

// "1.2 MB"-style size text for the header; empty for directories and
// for paths the filesystem does not know (virtual items).
std::wstring FormatFileSize(const std::wstring& filePath) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard,
                              &attrs) ||
        (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::wstring();
    }
    ULONGLONG bytes = (static_cast<ULONGLONG>(attrs.nFileSizeHigh) << 32) |
                      attrs.nFileSizeLow;
    wchar_t buffer[32];
    if (bytes < 1024) {
        swprintf_s(buffer, L"%llu B", bytes);
    } else if (bytes < 1024ULL * 1024) {
        swprintf_s(buffer, L"%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        swprintf_s(buffer, L"%.1f MB", bytes / (1024.0 * 1024));
    } else {
        swprintf_s(buffer, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buffer;
}

// Fills the content area of the preview window with the file's preview
// handler. Every failure is soft: the icon placeholder takes over.
// Runs on the STA thread, after the hook callback has returned.
void ActivatePreviewHandler(HWND host, const std::wstring& filePath) {
    CLSID clsid{};
    if (!FindPreviewHandlerClsid(filePath, &clsid)) {
        Wh_Log(L"no preview handler registered for this file type");
        return;
    }

    IPreviewHandler* handler = nullptr;
    // CLSCTX_ALL, not INPROC_SERVER: some handlers are registered as
    // LocalServer32 (Word -> WINWORD.EXE, WMP -> wmprph.exe) and work
    // fine out of process.
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL,
                                  __uuidof(IPreviewHandler),
                                  (void**)&handler);
    if (FAILED(hr) || !handler) {
        // REGDB_E_CLASSREG (80040154) means the registered server is
        // orphaned or bitness-mismatched, E_NOINTERFACE (80004002) that
        // the server did not expose IPreviewHandler to this process
        // (observed with Office on one machine): fall through to the
        // placeholder either way.
        Wh_Log(L"CoCreateInstance(preview handler): %08lx",
               static_cast<unsigned long>(hr));
        return;
    }

    bool initialized = false;
    IInitializeWithFile* initWithFile = nullptr;
    if (SUCCEEDED(handler->QueryInterface(__uuidof(IInitializeWithFile),
                                          (void**)&initWithFile))) {
        hr = initWithFile->Initialize(filePath.c_str(), STGM_READ);
        initWithFile->Release();
        initialized = SUCCEEDED(hr);
        if (!initialized) {
            Wh_Log(L"IInitializeWithFile::Initialize: %08lx",
                   static_cast<unsigned long>(hr));
        }
    }
    if (!initialized) {
        IInitializeWithStream* initWithStream = nullptr;
        hr = handler->QueryInterface(__uuidof(IInitializeWithStream),
                                     (void**)&initWithStream);
        if (SUCCEEDED(hr) && initWithStream) {
            IStream* stream = nullptr;
            hr = SHCreateStreamOnFileEx(filePath.c_str(),
                                         STGM_READ | STGM_SHARE_DENY_NONE, 0,
                                         FALSE, nullptr, &stream);
            if (SUCCEEDED(hr) && stream) {
                hr = initWithStream->Initialize(stream, STGM_READ);
                initialized = SUCCEEDED(hr);
                if (initialized) {
                    // Held until Unload(): some handlers keep reading
                    // the stream after Initialize returns.
                    g_previewStream = stream;
                } else {
                    stream->Release();
                    Wh_Log(L"IInitializeWithStream::Initialize: %08lx",
                           static_cast<unsigned long>(hr));
                }
            } else {
                Wh_Log(L"SHCreateStreamOnFileEx: %08lx",
                       static_cast<unsigned long>(hr));
            }
            initWithStream->Release();
        } else {
            Wh_Log(L"handler has no IInitializeWithFile/Stream: %08lx",
                   static_cast<unsigned long>(hr));
        }
    }
    if (!initialized) {
        handler->Release();
        return;
    }

    RECT contentRect;
    GetPreviewLayout(host, nullptr, &contentRect, nullptr);
    // This IPreviewHandler::SetWindow takes the initial rectangle
    // together with the window.
    hr = handler->SetWindow(host, &contentRect);
    if (SUCCEEDED(hr)) {
        hr = handler->SetRect(&contentRect);
        if (SUCCEEDED(hr)) {
            hr = handler->DoPreview();
        }
    }
    if (FAILED(hr)) {
        Wh_Log(L"preview handler failed: %08lx",
               static_cast<unsigned long>(hr));
        handler->Unload();
        handler->Release();
        if (g_previewStream) {
            g_previewStream->Release();
            g_previewStream = nullptr;
        }
        return;
    }

    g_previewHandler = handler;
    Wh_Log(L"Preview handler active");
}

// Unloads the handler while the host window still exists (the host is
// destroyed by ClosePreview right after this returns).
void StopPreviewHandler() {
    if (!g_previewHandler) {
        return;
    }
    g_previewHandler->Unload();
    g_previewHandler->Release();
    g_previewHandler = nullptr;
    if (g_previewStream) {
        g_previewStream->Release();
        g_previewStream = nullptr;
    }
    Wh_Log(L"Preview handler stopped");
}

// Fills the content area with the file's thumbnail (the shell's path
// B). BHID_ThumbnailHandler resolves the registered thumbnail provider
// the same way the shell does (extension key, ProgID, perceived type).
// Every failure is soft: the icon placeholder takes over. Unlike some
// preview handlers, the image thumbnail provider loads fine in this
// 32-bit process (both registry views point at a WOW64 DLL).
void ActivateThumbnail(const std::wstring& filePath) {
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(filePath.c_str(), nullptr,
                                              __uuidof(IShellItem),
                                              (void**)&item);
    if (FAILED(hr) || !item) {
        Wh_Log(L"SHCreateItemFromParsingName: %08lx",
               static_cast<unsigned long>(hr));
        return;
    }

    IThumbnailProvider* provider = nullptr;
    hr = item->BindToHandler(nullptr, BHID_ThumbnailHandler,
                             __uuidof(IThumbnailProvider), (void**)&provider);
    if (FAILED(hr) || !provider) {
        // No thumbnail registered for this file type (zip and friends).
        Wh_Log(L"BindToHandler(BHID_ThumbnailHandler): %08lx",
               static_cast<unsigned long>(hr));
        item->Release();
        return;
    }

    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    hr = provider->GetThumbnail(512, &bitmap, &alpha);
    provider->Release();
    item->Release();
    if (FAILED(hr) || !bitmap) {
        Wh_Log(L"IThumbnailProvider::GetThumbnail: %08lx",
               static_cast<unsigned long>(hr));
        return;
    }

    g_thumbnail = bitmap;
    g_thumbnailHasAlpha = alpha == WTSAT_ARGB;
    Wh_Log(L"Thumbnail active");
}

// The file's icon for the placeholder (the shell's path C).
void ExtractFileIcon(const std::wstring& filePath) {
    SHFILEINFO info{};
    if (SHGetFileInfoW(filePath.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_LARGEICON)) {
        g_fileIcon = info.hIcon;
    }
}

// Frees the content objects this thread owns. Called after the
// handler is stopped, while the host window still exists.
void FreePreviewContent() {
    if (g_thumbnail) {
        DeleteObject(g_thumbnail);
        g_thumbnail = nullptr;
        g_thumbnailHasAlpha = false;
    }
    if (g_fileIcon) {
        DestroyIcon(g_fileIcon);
        g_fileIcon = nullptr;
    }
}

// Returns the full path of the first item selected in the folder window
// behind hwndExplorer, or an empty string when there is no selection or
// the shell refuses to tell (a soft failure: the preview simply stays
// closed). Runs on the worker thread only, after the hook callback has
// returned: COM never happens inside the input path.
//
// The route was verified against this machine's shell with a standalone
// probe: IShellWindows is registered as a local server (CLSCTX_ALL), the
// SID_STopLevelBrowser service answers as IShellBrowser, and its active
// shell view supports IFolderView2.
std::wstring GetFirstSelectedItemPath(HWND hwndExplorer) {
    std::wstring result;

    IShellWindows* shellWindows = nullptr;
    IWebBrowser2* webBrowser = nullptr;
    IServiceProvider* provider = nullptr;
    IShellBrowser* shellBrowser = nullptr;
    IShellView* shellView = nullptr;
    IFolderView2* folderView = nullptr;
    IShellItemArray* selection = nullptr;
    IShellItem* item = nullptr;
    PWSTR displayName = nullptr;

    do {
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                      __uuidof(IShellWindows),
                                      (void**)&shellWindows);
        if (FAILED(hr)) {
            Wh_Log(L"ShellWindows: %08lx", static_cast<unsigned long>(hr));
            break;
        }

        long shellWindowCount = 0;
        shellWindows->get_Count(&shellWindowCount);
        for (long i = 0; i < shellWindowCount && !webBrowser; ++i) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = i;
            IDispatch* dispatch = nullptr;
            if (FAILED(shellWindows->Item(index, &dispatch)) || !dispatch) {
                continue;
            }
            IWebBrowser2* candidate = nullptr;
            dispatch->QueryInterface(__uuidof(IWebBrowser2),
                                     (void**)&candidate);
            dispatch->Release();
            if (!candidate) {
                continue;
            }
            SHANDLE_PTR candidateHwnd = 0;
            candidate->get_HWND(&candidateHwnd);
            if ((HWND)(LONG_PTR)candidateHwnd == hwndExplorer) {
                webBrowser = candidate;
            } else {
                candidate->Release();
            }
        }
        if (!webBrowser) {
            Wh_Log(L"no shell window entry for %p",
                   static_cast<void*>(hwndExplorer));
            break;
        }

        hr = webBrowser->QueryInterface(__uuidof(IServiceProvider),
                                        (void**)&provider);
        if (SUCCEEDED(hr) && provider) {
            hr = provider->QueryService(SID_STopLevelBrowser,
                                        __uuidof(IShellBrowser),
                                        (void**)&shellBrowser);
        }
        if (FAILED(hr) || !shellBrowser) {
            Wh_Log(L"QueryService(IID_IShellBrowser): %08lx",
                   static_cast<unsigned long>(hr));
            break;
        }

        hr = shellBrowser->QueryActiveShellView(&shellView);
        if (FAILED(hr) || !shellView) {
            Wh_Log(L"QueryActiveShellView: %08lx",
                   static_cast<unsigned long>(hr));
            break;
        }

        hr = shellView->QueryInterface(__uuidof(IFolderView2),
                                       (void**)&folderView);
        if (FAILED(hr) || !folderView) {
            Wh_Log(L"QI IFolderView2: %08lx", static_cast<unsigned long>(hr));
            break;
        }

        // FALSE: an empty selection returns an empty array instead of
        // the folder itself.
        hr = folderView->GetSelection(FALSE, &selection);
        if (FAILED(hr) || !selection) {
            Wh_Log(L"GetSelection: %08lx", static_cast<unsigned long>(hr));
            break;
        }

        DWORD itemCount = 0;
        selection->GetCount(&itemCount);
        if (itemCount == 0) {
            Wh_Log(L"selection is empty");
            break;
        }

        hr = selection->GetItemAt(0, &item);
        if (FAILED(hr) || !item) {
            Wh_Log(L"GetItemAt: %08lx", static_cast<unsigned long>(hr));
            break;
        }

        // Virtual items (This PC, libraries) may have no filesystem
        // path; fall back to the display name so something sensible is
        // still shown.
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &displayName)) ||
            !displayName) {
            item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName);
        }
        if (!displayName) {
            Wh_Log(L"GetDisplayName failed");
            break;
        }
        result = displayName;
    } while (false);

    if (displayName) {
        CoTaskMemFree(displayName);
    }
    if (item) {
        item->Release();
    }
    if (selection) {
        selection->Release();
    }
    if (folderView) {
        folderView->Release();
    }
    if (shellView) {
        shellView->Release();
    }
    if (shellBrowser) {
        shellBrowser->Release();
    }
    if (provider) {
        provider->Release();
    }
    if (webBrowser) {
        webBrowser->Release();
    }
    if (shellWindows) {
        shellWindows->Release();
    }
    return result;
}

// Aspect-fits the thumbnail into the content area. The bitmap is a
// 32bpp DIB section; thumbnails with an alpha channel are blended, the
// rest are copied opaque.
void DrawThumbnail(HDC dc, const RECT& contentRect) {
    BITMAP bitmapInfo{};
    if (!GetObject(g_thumbnail, sizeof(bitmapInfo), &bitmapInfo) ||
        bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0) {
        return;
    }

    int contentWidth = contentRect.right - contentRect.left;
    int contentHeight = contentRect.bottom - contentRect.top;
    int width = contentWidth;
    int height =
        MulDiv(bitmapInfo.bmHeight, contentWidth, bitmapInfo.bmWidth);
    if (height > contentHeight) {
        height = contentHeight;
        width =
            MulDiv(bitmapInfo.bmWidth, contentHeight, bitmapInfo.bmHeight);
    }
    int x = contentRect.left + (contentWidth - width) / 2;
    int y = contentRect.top + (contentHeight - height) / 2;

    HDC memoryDc = CreateCompatibleDC(dc);
    if (!memoryDc) {
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(memoryDc, g_thumbnail);
    SetStretchBltMode(memoryDc, HALFTONE);
    SetBrushOrgEx(memoryDc, 0, 0, nullptr);
    BLENDFUNCTION blend{AC_SRC_OVER,
                        0,
                        255,
                        static_cast<BYTE>(g_thumbnailHasAlpha ? AC_SRC_ALPHA
                                                              : 0)};
    AlphaBlend(dc, x, y, width, height, memoryDc, 0, 0, bitmapInfo.bmWidth,
               bitmapInfo.bmHeight, blend);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
}

// Icon, file name and the "no preview" message (the shell's path C),
// centered as one block in the content area.
void DrawPlaceholder(HDC dc,
                     const RECT& contentRect,
                     const wchar_t* fileName) {
    constexpr int kIconSize = 32;
    constexpr int kBlockHeight =
        kIconSize + 12 + 20 + 4 + 24;  // icon + gaps + two lines
    int top = contentRect.top +
              ((contentRect.bottom - contentRect.top) - kBlockHeight) / 2;
    if (top < contentRect.top) {
        top = contentRect.top;
    }

    int centerX = contentRect.left +
                  (contentRect.right - contentRect.left) / 2;
    int textTop = top;
    if (g_fileIcon) {
        DrawIconEx(dc, centerX - kIconSize / 2, top, g_fileIcon, kIconSize,
                   kIconSize, 0, nullptr, DI_NORMAL);
        textTop = top + kIconSize + 12;
    }

    RECT nameRect{contentRect.left, textTop, contentRect.right,
                  textTop + 20};
    DrawTextW(dc, fileName, -1, &nameRect,
              DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT messageRect{contentRect.left, textTop + 24, contentRect.right,
                     textTop + 48};
    DrawTextW(dc, L"No preview available", -1, &messageRect,
              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd,
                                UINT msg,
                                WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // painted in WM_PAINT

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            RECT headerRect;
            RECT contentRect;
            RECT hintRect;
            GetPreviewLayout(hwnd, &headerRect, &contentRect, &hintRect);

            // While a preview handler is active the content area must
            // stay untouched: painting over it would erase the
            // handler's child window, which repaints only on its own
            // terms.
            if (g_previewHandler) {
                ExcludeClipRect(dc, contentRect.left, contentRect.top,
                                contentRect.right, contentRect.bottom);
            }

            HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
            FillRect(dc, &rc, background);
            DeleteObject(background);
            DrawEdge(dc, &rc, EDGE_RAISED, BF_RECT);

            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 240));

            // File name (with the size) in the header.
            const wchar_t* fileName = L"Quick Look!";
            if (!g_previewFilePath.empty()) {
                const wchar_t* slash =
                    wcsrchr(g_previewFilePath.c_str(), L'\\');
                fileName = slash ? slash + 1 : g_previewFilePath.c_str();
            }
            if (g_showCaption.load(std::memory_order_relaxed)) {
                const wchar_t* headerText = !g_previewHeaderText.empty()
                                                ? g_previewHeaderText.c_str()
                                                : fileName;
                DrawTextW(dc, headerText, -1, &headerRect,
                          DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                              DT_NOPREFIX);
            }

            DrawTextW(dc, L"Press Space or Esc to close", -1, &hintRect,
                      DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Content: with an active handler the area was excluded
            // above and the handler's child window owns it; otherwise
            // this thread draws the thumbnail, or the icon placeholder.
            if (!g_previewHandler && g_thumbnail) {
                DrawThumbnail(dc, contentRect);
            } else if (!g_previewHandler &&
                       (g_fileIcon || !g_previewFilePath.empty())) {
                DrawPlaceholder(dc, contentRect, fileName);
            }
            SelectObject(dc, oldFont);

            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowPreview() {
    if (g_previewWindow) {
        return;
    }

    // The foreground window may have changed between the hook and now.
    HWND fg = GetForegroundWindow();
    WCHAR className[64];
    if (!GetClassNameW(fg, className, ARRAYSIZE(className)) ||
        (wcscmp(className, L"CabinetWClass") != 0 &&
         wcscmp(className, L"ExploreWClass") != 0)) {
        return;
    }

    // The selection decides whether anything is shown at all: with an
    // empty selection the preview stays closed (soft failure, the log
    // explains which step failed).
    std::wstring selectedPath = GetFirstSelectedItemPath(fg);
    if (selectedPath.empty()) {
        Wh_Log(L"no file to show; preview stays closed");
        return;
    }

    // Center the window on the monitor of the explorer window. The
    // settings decide the size: an aspect-correct box of sizePercent%
    // of the monitor's work area.
    RECT explorerRect;
    GetWindowRect(fg, &explorerRect);
    MONITORINFO monitorInfo{.cbSize = sizeof(MONITORINFO)};
    if (!GetMonitorInfo(MonitorFromRect(&explorerRect,
                                        MONITOR_DEFAULTTONEAREST),
                        &monitorInfo)) {
        return;
    }
    const RECT& work = monitorInfo.rcWork;
    int sizePercent = g_sizePercent.load(std::memory_order_relaxed);
    int maxWidth = MulDiv(work.right - work.left, sizePercent, 100);
    int maxHeight = MulDiv(work.bottom - work.top, sizePercent, 100);
    int aspect = g_aspectRatio.load(std::memory_order_relaxed);
    int windowWidth = maxWidth;
    int windowHeight = MulDiv(maxWidth, 100, aspect);
    if (windowHeight > maxHeight) {
        windowHeight = maxHeight;
        windowWidth = MulDiv(maxHeight, aspect, 100);
    }
    int x = work.left + ((work.right - work.left) - windowWidth) / 2;
    int y = work.top + ((work.bottom - work.top) - windowHeight) / 2;

    HWND hwnd =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        kPreviewClass, L"", WS_POPUP, x, y, windowWidth,
                        windowHeight, nullptr, nullptr, GetModuleHandle(nullptr),
                        nullptr);
    if (!hwnd) {
        Wh_Log(L"CreateWindowEx failed");
        return;
    }
    g_previewFilePath = std::move(selectedPath);
    g_previewWindow = hwnd;
    g_explorerWindow = fg;

    // Header line: file name plus size.
    const wchar_t* slash = wcsrchr(g_previewFilePath.c_str(), L'\\');
    std::wstring fileName = slash ? slash + 1 : g_previewFilePath;
    std::wstring fileSize = FormatFileSize(g_previewFilePath);
    g_previewHeaderText = fileName;
    if (!fileSize.empty()) {
        g_previewHeaderText += L" — ";
        g_previewHeaderText += fileSize;
    }

    // Rounded corners on Windows 11; fails quietly on older systems.
    const DWORD cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &cornerPreference, sizeof(cornerPreference));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    g_previewOpen.store(true, std::memory_order_relaxed);
    // Content priority: preview handler (richest), then the file's
    // thumbnail, then the icon placeholder. Every step fails softly and
    // the repaint below always has something to show.
    ActivatePreviewHandler(hwnd, g_previewFilePath);
    if (!g_previewHandler) {
        ActivateThumbnail(g_previewFilePath);
    }
    if (!g_previewHandler && !g_thumbnail) {
        ExtractFileIcon(g_previewFilePath);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
    Wh_Log(L"Preview shown");
}

void ClosePreview() {
    if (!g_previewWindow) {
        return;
    }
    StopPreviewHandler();  // unload while the host window still exists
    FreePreviewContent();
    DestroyWindow(g_previewWindow);
    g_previewWindow = nullptr;
    g_previewFilePath.clear();
    g_previewHeaderText.clear();
    g_explorerWindow = nullptr;
    g_previewOpen.store(false, std::memory_order_relaxed);
    Wh_Log(L"Preview closed");
}

// Poll-based auto close (design section 5): the preview dies when the
// user switches windows, changes the selection or navigates, clicks
// outside, or the Explorer window itself goes away. Runs on the STA
// thread on a timer, only ever while a preview is open. The preview
// window is NOACTIVATE (focus never moves to it), which is why this is
// polling rather than WM_ACTIVATE.
void CheckAutoClose() {
    if (!g_previewWindow) {
        return;
    }
    if (!IsWindow(g_explorerWindow)) {
        Wh_Log(L"auto-close: explorer window is gone");
        ClosePreview();
        return;
    }
    if (GetForegroundWindow() != g_explorerWindow) {
        Wh_Log(L"auto-close: foreground window changed");
        ClosePreview();
        return;
    }
    // A click outside: the button is down while the cursor is over some
    // other window. Clicks inside the preview (video controls, scroll)
    // do nothing.
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        POINT point;
        if (GetCursorPos(&point)) {
            HWND under = WindowFromPoint(point);
            if (!under || GetAncestor(under, GA_ROOT) != g_previewWindow) {
                Wh_Log(L"auto-close: clicked outside");
                ClosePreview();
                return;
            }
        }
    }
    // The preview shows the first selected item: another selection, an
    // empty selection or another folder means the user moved on.
    if (GetFirstSelectedItemPath(g_explorerWindow) != g_previewFilePath) {
        Wh_Log(L"auto-close: selection changed");
        ClosePreview();
        return;
    }
}

// Runs on the hook thread, inside the input path, so it only does
// cheap checks and a single PostMessage. No windows are created and no
// COM is called here: work that could block would delay every key
// press in the session and could get the hook silently removed by
// Windows (LowLevelHooksTimeout).
LRESULT CALLBACK LowLevelKeyboardProc(int nCode,
                                      WPARAM wParam,
                                      LPARAM lParam) {
    if (nCode != HC_ACTION) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* info = (const KBDLLHOOKSTRUCT*)lParam;
    const bool isKeyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool isKeyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if ((!isKeyDown && !isKeyUp) || (info->flags & LLKHF_INJECTED)) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (info->vkCode == VK_SPACE) {
        if (isKeyUp) {
            g_spaceDown = false;
            if (g_spaceSwallowed) {
                // Swallow the keyup that matches a consumed keydown, so
                // the system key state stays consistent.
                g_spaceSwallowed = false;
                return 1;
            }
        } else if (!g_spaceDown) {
            g_spaceDown = true;
            if (g_previewOpen.load(std::memory_order_relaxed)) {
                // Closing an open preview works no matter which window
                // is in front: only swallow the key when a folder window
                // is active, so Space keeps typing in other
                // applications. The mod's own window never becomes the
                // foreground one, so this check cannot get stuck here.
                g_spaceSwallowed = IsQuickLookContext();
                Wh_Log(L"Space: close requested");
                PostMessageW(g_msgWindow, WM_CLOSE_PREVIEW, 0, 0);
                if (g_spaceSwallowed) {
                    return 1;
                }
            } else if (g_enabled.load(std::memory_order_relaxed) &&
                       IsQuickLookContext()) {
                // Opening still requires a folder window with the focus
                // outside of text fields (rename mode, address bar).
                g_spaceSwallowed = true;
                Wh_Log(L"Space: open requested");
                PostMessageW(g_msgWindow, WM_SHOW_PREVIEW, 0, 0);
                return 1;
            }
        } else if (g_spaceSwallowed) {
            // Auto-repeat of a keydown we have already handled.
            return 1;
        }
    } else if (info->vkCode == VK_ESCAPE) {
        // Esc only ever interacts with an open preview; when the
        // preview is closed, Esc is never touched.
        if (isKeyUp) {
            g_escDown = false;
            if (g_escSwallowed) {
                g_escSwallowed = false;
                return 1;
            }
        } else if (!g_escDown) {
            g_escDown = true;
            if (g_previewOpen.load(std::memory_order_relaxed)) {
                g_escSwallowed = IsQuickLookContext();
                Wh_Log(L"Esc: close requested");
                PostMessageW(g_msgWindow, WM_CLOSE_PREVIEW, 0, 0);
                if (g_escSwallowed) {
                    return 1;
                }
            }
        } else if (g_escSwallowed) {
            return 1;
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// The STA thread: apartment, preview window and all COM work. Signals
// g_staReady once its message queue and message-only window exist, so
// the hook thread can safely start posting to them.
DWORD WINAPI StaThread(LPVOID /*param*/) {
    auto ready = [](bool ok) {
        g_staReady.store(ok, std::memory_order_relaxed);
        SetEvent(g_staReadyEvent);
    };

    // The preview handlers that render the content live in this mod's
    // own process and are STA objects, so the apartment is set up once
    // on this thread from the start.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Wh_Log(L"CoInitializeEx failed: %08lx", static_cast<unsigned long>(hr));
        ready(false);
        return 1;
    }

    WNDCLASSEXW wc{
        .cbSize = sizeof(WNDCLASSEXW),
        .lpfnWndProc = PreviewWndProc,
        .hInstance = GetModuleHandle(nullptr),
        .lpszClassName = kPreviewClass,
    };
    if (!RegisterClassExW(&wc)) {
        Wh_Log(L"RegisterClassEx failed");
        CoUninitialize();
        ready(false);
        return 1;
    }

    // Message-only window: the hook thread posts show/close requests
    // here, which keeps all real work out of the input path. Creating
    // it also gives this thread the message queue it will pump.
    HWND msgWindow = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!msgWindow) {
        Wh_Log(L"CreateWindowEx failed");
        CoUninitialize();
        ready(false);
        return 1;
    }
    g_msgWindow = msgWindow;
    // Auto-close checks for the open preview run on this timer (400 ms
    // is the design's 300-500 ms window).
    SetTimer(msgWindow, 1, 400, nullptr);
    ready(true);

    bool stopping = false;
    while (!stopping) {
        // Wake up on the stop event or on messages (the hook thread
        // posts its requests to the message-only window above).
        DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE,
                                               INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            stopping = true;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                stopping = true;
                break;
            }
            if (msg.message == WM_SHOW_PREVIEW) {
                ShowPreview();
                continue;
            }
            if (msg.message == WM_CLOSE_PREVIEW) {
                ClosePreview();
                continue;
            }
            if (msg.message == WM_TIMER && msg.hwnd == msgWindow) {
                // Our own timer: the auto-close check. Timers belonging
                // to the preview handler's child windows must still be
                // dispatched, so only this thread's message-only window
                // is intercepted.
                CheckAutoClose();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    ClosePreview();
    KillTimer(msgWindow, 1);
    DestroyWindow(msgWindow);
    g_msgWindow = nullptr;
    CoUninitialize();
    Wh_Log(L"STA thread stopped");
    return 0;
}

// The hook thread: a low-level keyboard hook with a message pump of its
// own and nothing heavy in it. Callbacks only run cheap checks and
// post requests to the STA thread; a slow preview handler render can
// therefore never stall input or get the hook removed by Windows
// (LowLevelHooksTimeout).
DWORD WINAPI HookThread(LPVOID /*param*/) {
    // Message-only window of this thread, used for the heal timer.
    HWND hookMsgWindow =
        CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                        nullptr, nullptr, nullptr);
    if (!hookMsgWindow) {
        Wh_Log(L"CreateWindowEx failed");
        return 1;
    }

    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      GetModuleHandle(nullptr), 0);
    if (!g_keyboardHook) {
        Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
        DestroyWindow(hookMsgWindow);
        return 1;
    }
    Wh_Log(L"Keyboard hook installed");

    // Reconcile the key repeat state with the real key state 4 times a
    // second, so a keyup missed by the hook cannot break Space
    // permanently.
    SetTimer(hookMsgWindow, 1, 250, nullptr);

    bool stopping = false;
    while (!stopping) {
        DWORD wait = MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE,
                                               INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            stopping = true;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                stopping = true;
                break;
            }
            if (msg.message == WM_TIMER) {
                HealKeyStates();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = nullptr;
    KillTimer(hookMsgWindow, 1);
    DestroyWindow(hookMsgWindow);
    Wh_Log(L"Hook thread stopped");
    return 0;
}

void WhTool_ModUninit() {
    Wh_Log(L">");
    // Both threads wake up on the stop event. The hook thread is
    // joined first so no new requests can arrive while the STA thread
    // tears down. The timeout guards against a preview handler that
    // refuses to come back: the tool process exits right after this
    // callback, taking any stuck thread with it.
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    if (g_hookThreadHandle) {
        if (WaitForSingleObject(g_hookThreadHandle, 5000) == WAIT_TIMEOUT) {
            Wh_Log(L"Hook thread did not stop in time");
        }
        CloseHandle(g_hookThreadHandle);
        g_hookThreadHandle = nullptr;
    }
    if (g_staThreadHandle) {
        if (WaitForSingleObject(g_staThreadHandle, 5000) == WAIT_TIMEOUT) {
            Wh_Log(L"STA thread did not stop in time");
        }
        CloseHandle(g_staThreadHandle);
        g_staThreadHandle = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_staReadyEvent) {
        CloseHandle(g_staReadyEvent);
        g_staReadyEvent = nullptr;
    }
}

void WhTool_ModSettingsChanged() {
    Wh_Log(L">");
    LoadSettings();
}

BOOL WhTool_ModInit() {
    Wh_Log(L">");
    LoadSettings();

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_staReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent || !g_staReadyEvent) {
        Wh_Log(L"CreateEvent failed");
        WhTool_ModUninit();
        return FALSE;
    }

    g_staThreadHandle =
        CreateThread(nullptr, 0, StaThread, nullptr, 0, nullptr);
    if (!g_staThreadHandle ||
        WaitForSingleObject(g_staReadyEvent, 10000) != WAIT_OBJECT_0 ||
        !g_staReady.load(std::memory_order_relaxed)) {
        Wh_Log(L"STA thread failed to start");
        WhTool_ModUninit();
        return FALSE;
    }

    // The hook may only start once the STA thread is able to receive
    // its posts.
    g_hookThreadHandle =
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    if (!g_hookThreadHandle) {
        Wh_Log(L"CreateThread (hook) failed");
        WhTool_ModUninit();
        return FALSE;
    }
    return TRUE;
}

////////////////////////////////////////////////////////////////////////////////
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
//
// The mod will load and run in a dedicated windhawk.exe process.
//
// Paste the code below as part of the mod code, and use these callbacks:
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
