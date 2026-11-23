// ClipAgent.cpp part 01 -- includes, globals, prototypes, utilities.
// ASCII only. // comments only. No C4819 triggers.
// Merged with parts 02/03/04 into one translation unit.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// winsock2 MUST precede windows.h or the old winsock.h wins.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <restartmanager.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "ClipProtocol.h"
#include "miniz.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Rstrtmgr.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Advapi32.lib")

// ---- ui / timer ids, shared by parts 03 and 04 ----
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_CHOICE      (WM_APP + 2)
#define WM_APP_PROGRESS    (WM_APP + 3)
#define ID_TRAY_ICON       1
#define IDM_PASTE          1001
#define IDM_CANCEL         1002
#define IDM_RESUME         1003
#define IDM_OPEN_OUTDIR    1004
#define IDM_SHOW_LOG       1005
#define IDM_SHOWLOG       IDM_SHOW_LOG
#define IDM_EXIT           1006
#define HOTKEY_PASTE       1
#define HOTKEY_CANCEL      2
#define HOTKEY_LOG         3
#define IDT_CHOICE         4
#define IDT_SWEEP          5
#define IDT_PROGRESS       6
#define CLIP_WND_CLASS     "ClipAgentHiddenWnd"

// ---- log levels ----
#define LOG_INFO  0
#define LOG_OK    1
#define LOG_WARN  2
#define LOG_ERR   3

// ---- pseudo error for a clean peer close ----
#define CLIP_NET_CLOSED    0xFFFFFFF0u
#define CLIP_MAX_TEXT_WCH  524288
#define CLIP_MAX_DEPTH     32
#define CLIP_MAX_PATHS     4096

// ---- one global context. Every cross-thread field is volatile. ----
typedef struct _CLIP_CTX {
    SOCKET            sock;
    CLIP_STATE        state;
    volatile LONG     abort;
    volatile LONG     netRunning;
    volatile LONG     uiRunning;

    char              key[CLIP_KEY_MAX];
    ULONG             keyTag;
    char              peerHost[128];
    USHORT            port;
    BOOL              isHost;
    BOOL              quiet;

    WCHAR             outDir[MAX_PATH];
    WCHAR             tempDir[MAX_PATH];
    ULONG             maxFileMb;
    ULONG             confirmMb;

    HINSTANCE         hInst;
    HWND              hwnd;
    HWND              hwndDlg;
    DWORD             uiThreadId;
    HANDLE            hNetThread;
    CRITICAL_SECTION  logLock;
    NOTIFYICONDATAW   nid;
    BOOL              trayAdded;
    BOOL              consoleShown;
    HANDLE            hConsole;

    // advert state: paths and text only, NEVER file content
    WCHAR**           pendingPaths;
    ULONG             pendingCount;
    ULONG             pendingCap;
    WCHAR*            pendingText;
    ULONGLONG         pendingTextBytes;
    ULONGLONG         pendingClipSeq;
    CLIP_LIST         pendingList;

    // transfer state
    CLIPZIP           zip;
    WCHAR             zipTemp[MAX_PATH];
    WCHAR             cdTemp[MAX_PATH];
    WCHAR             partPath[MAX_PATH];
    WCHAR             finalPath[MAX_PATH];
    HANDLE            hPart;
    ULONGLONG         requestId;
    ULONGLONG         bytesTotal;
    ULONGLONG         bytesDone;
    ULONGLONG         blockSeq;
    ULONG             wholeCrc;
    BOOL              textMode;      // FRAME_ZIP_START.FileName[0] == 0
    DWORD             lastErr;

    // pause / skip
    CLIP_PAUSE        pauseInfo;
    DWORD             pauseTick;
    char              skipComment[CLIPZIP_MAX_COMMENT];
    ULONG             skipCount;
} CLIP_CTX;

static CLIP_CTX g;

// ============================================================
// Forward declarations for parts 02 / 03 / 04.
// Kept here so any part may call any other part after merging.
// ============================================================

// part 02: state machine, cleanup, interruption
VOID     TransferSetState(CLIP_STATE st);
VOID     CleanupTransfer(CLIP_REASON reason, BOOL keepPartial);
VOID     TransferRequestAbort(VOID);
BOOL     TransferIsAborted(VOID);
VOID     SweepStaleFiles(BOOL atStartup);
VOID     PauseTransfer(const CLIP_PAUSE* p);
BOOL     ResumeTransfer(ULONG action);
VOID     HandleDisconnect(DWORD err);

// part 03: owner lookup, error split, front end
BOOL     UiInit(HINSTANCE hInst);
VOID     UiShutdown(VOID);
VOID     TrayBalloon(const char* title, const char* text, DWORD flags);
VOID     TraySetTip(const char* tip);
BOOL     UiRegisterHotKeys(VOID);
VOID     UiShowLog(BOOL show);
LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
ULONG    ClipQueryOwnerProcesses(const WCHAR* path, char* out, ULONG cap);
ULONG    ClipClassifyAndReport(const WCHAR* subject, DWORD err,
                              char* out, ULONG cap);
ULONG    AskUserChoice(const char* prompt);
BOOL     ClipGetForegroundDir(WCHAR* out, ULONG cap);
BOOL     ClipCheckDiskSpace(const WCHAR* dir, ULONGLONG need);
VOID     ClipShowSkipList(const char* comment);

// part 04: wire dispatch, streaming, entry point
DWORD WINAPI NetThread(LPVOID p);
BOOL     HandleFrame(const CLIP_FRAME_HEADER* h, const BYTE* payload);
BOOL     HandlePull(const CLIP_PULL* p);
BOOL     BuildAndSendPayload(ULONGLONG requestId, BOOL resume);
BOOL     ReceiverBegin(const CLIP_ZIP_START* s);
BOOL     ReceiverWriteChunk(ULONGLONG seq, const BYTE* p, ULONG n);
BOOL     ReceiverFinish(const CLIP_ZIP_END* e);
BOOL     ParseArgs(int argc, WCHAR** argv);

// part 01 utilities
VOID     Log(ULONG level, const char* fmt, ...);
char*    W2A(const WCHAR* w);
WCHAR*   A2W(const char* a);
VOID     FreeBuf(void* p, ULONG n);
VOID     ClipFormatBytes(ULONGLONG n, char* out, ULONG cap);
ULONG    FrameCrc(const BYTE* p, ULONG n);
BOOL     NetSendFrame(USHORT type, ULONGLONG seq,
                      const void* payload, ULONG len, PDWORD pdwErr);
int      NetRecvFrame(CLIP_FRAME_HEADER* h, BYTE* payload, ULONG cap,
                      PULONG pLen, PDWORD pdwErr);
BOOL     ClipSanitizeName(const WCHAR* raw, WCHAR* out, ULONG cap,
                          ULONGLONG seq);
BOOL     ClipMakeTempName(WCHAR* out, ULONG cap, const WCHAR* suffix,
                          ULONGLONG seq);
BOOL     ClipMakePartName(const WCHAR* finalName, WCHAR* out, ULONG cap);
BOOL     ClipEnsureDir(const WCHAR* dir);
VOID     ClipDeleteIfExists(const WCHAR* path);
VOID     ClipZeroAndFree(void* p, ULONG n);
BOOL     ClipGetDropPaths(WCHAR*** pPaths, PULONG pCount);
BOOL     ClipGetUnicodeText(WCHAR** pText, PULONGLONG pBytes);
BOOL     ClipSetUnicodeText(const WCHAR* text);
BOOL     ClipCalcTotal(WCHAR** paths, ULONG count, PULONGLONG pTotal,
                       PULONGLONG pLargest, PULONG pFiles, PULONG pDirs);
VOID     PendingClear(VOID);
BOOL     PendingEnsure(ULONG need);

// ============================================================
// Log: timestamped console line + tray balloon on error.
// Console text is ASCII only on purpose (C4819 avoidance).
// ============================================================
VOID Log(ULONG level, const char* fmt, ...)
{
    char        body[1024];
    char        line[1200];
    SYSTEMTIME  st;
    va_list     ap;
    CONSOLE_SCREEN_BUFFER_INFO cbi;
    WORD        attr = 0, old = 0;
    BOOL        recol = FALSE;

    va_start(ap, fmt);
    StringCchVPrintfA(body, 1024, fmt, ap);
    va_end(ap);

    GetLocalTime(&st);
    StringCchPrintfA(line, 1200, "[%02u:%02u:%02u] %s\n",
                     st.wHour, st.wMinute, st.wSecond, body);

    EnterCriticalSection(&g.logLock);
    if (!g.quiet && g.hConsole) {
        if (GetConsoleScreenBufferInfo(g.hConsole, &cbi)) {
            old = cbi.wAttributes;
            switch (level) {
            case LOG_OK:   attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case LOG_WARN: attr = FOREGROUND_RED | FOREGROUND_GREEN |
                                 FOREGROUND_INTENSITY; break;
            case LOG_ERR:  attr = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            default:       attr = old; break;
            }
            SetConsoleTextAttribute(g.hConsole, attr);
            recol = TRUE;
        }
        WriteConsoleA(g.hConsole, line, (DWORD)lstrlenA(line), NULL, NULL);
        if (recol) SetConsoleTextAttribute(g.hConsole, old);
    }
    LeaveCriticalSection(&g.logLock);

    if (level >= LOG_ERR && g.trayAdded)
        TrayBalloon("ClipAgent", body, 0x00000001);
}

// ============================================================
// Small string helpers. Every buffer is zeroed before free.
// ============================================================
char* W2A(const WCHAR* w)
{
    int   n;
    char* p;
    n = WideCharToMultiByte(CP_ACP, 0, w ? w : L"", -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    p = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)n + 1);
    if (!p) return NULL;
    if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, p, n, NULL, NULL);
    p[n] = '\0';
    return p;
}

WCHAR* A2W(const char* a)
{
    int    n;
    WCHAR* p;
    n = MultiByteToWideChar(CP_ACP, 0, a ? a : "", -1, NULL, 0);
    if (n <= 0) n = 1;
    p = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                          ((SIZE_T)n + 1) * sizeof(WCHAR));
    if (!p) return NULL;
    if (a) MultiByteToWideChar(CP_ACP, 0, a, -1, p, n);
    p[n] = L'\0';
    return p;
}

VOID FreeBuf(void* p, ULONG n)
{
    if (!p) return;
    if (n) ZeroMemory(p, n);
    HeapFree(GetProcessHeap(), 0, p);
}

VOID ClipZeroAndFree(void* p, ULONG n)
{
    FreeBuf(p, n);
}

VOID ClipFormatBytes(ULONGLONG n, char* out, ULONG cap)
{
    double v = (double)n;
    if (n < 1024ull)
        StringCchPrintfA(out, cap, "%I64u B", n);
    else if (n < 1024ull * 1024ull)
        StringCchPrintfA(out, cap, "%.1f KB", v / 1024.0);
    else if (n < 1024ull * 1024ull * 1024ull)
        StringCchPrintfA(out, cap, "%.1f MB", v / 1048576.0);
    else
        StringCchPrintfA(out, cap, "%.2f GB", v / 1073741824.0);
}

// ============================================================
// Frame CRC: standard IEEE result, seed 0xFFFFFFFF, final invert.
// Same arithmetic as the ZIP entries in miniz.c.
// ============================================================
ULONG FrameCrc(const BYTE* p, ULONG n)
{
    return ~ClipCrc32(0xFFFFFFFFu, p, n);
}

static int SockSendAll(SOCKET s, const BYTE* p, ULONG n, PDWORD pdwErr)
{
    ULONG left = n;
    while (left) {
        int k = send(s, (const char*)p, (int)left, 0);
        if (k == SOCKET_ERROR) { *pdwErr = (DWORD)WSAGetLastError(); return 0; }
        if (k == 0)           { *pdwErr = CLIP_NET_CLOSED;          return 0; }
        p    += k;
        left -= (ULONG)k;
    }
    return 1;
}

static int SockRecvAll(SOCKET s, BYTE* p, ULONG n, PDWORD pdwErr)
{
    ULONG left = n;
    while (left) {
        int k = recv(s, (char*)p, (int)left, 0);
        if (k == SOCKET_ERROR) { *pdwErr = (DWORD)WSAGetLastError(); return 0; }
        if (k == 0)           { *pdwErr = CLIP_NET_CLOSED;          return 0; }
        p    += k;
        left -= (ULONG)k;
    }
    return 1;
}

// Both ends are x64 little-endian Windows. Structs go raw, no hton*.
BOOL NetSendFrame(USHORT type, ULONGLONG seq,
                  const void* payload, ULONG len, PDWORD pdwErr)
{
    CLIP_FRAME_HEADER h;
    DWORD e = ERROR_SUCCESS;

    if (pdwErr) *pdwErr = ERROR_SUCCESS;
    if (g.sock == INVALID_SOCKET || len > CLIP_MAX_FRAME_PAYLOAD) {
        if (pdwErr) *pdwErr = ERROR_INVALID_PARAMETER;
        return FALSE;
    }

    ZeroMemory(&h, sizeof(h));
    h.Magic   = CLIP_FRAME_MAGIC;
    h.Version = (USHORT)CLIP_PROTO_VERSION;
    h.Type    = type;
    h.KeyTag  = g.keyTag;
    h.Seq     = seq;
    h.Length  = len;
    h.Crc32   = FrameCrc((const BYTE*)payload, len);

    if (!SockSendAll(g.sock, (const BYTE*)&h, sizeof(h), &e)) goto fail;
    if (len && !SockSendAll(g.sock, (const BYTE*)payload, len, &e)) goto fail;
    return TRUE;

fail:
    if (pdwErr) *pdwErr = e;
    return FALSE;
}

// returns 1 = frame ok, 0 = error (pdwErr), -1 = peer closed
int NetRecvFrame(CLIP_FRAME_HEADER* h, BYTE* payload, ULONG cap,
                 PULONG pLen, PDWORD pdwErr)
{
    DWORD e = ERROR_SUCCESS;
    ZeroMemory(h, sizeof(*h));
    if (pLen)  *pLen  = 0;
    if (pdwErr) *pdwErr = ERROR_SUCCESS;

    if (!SockRecvAll(g.sock, (BYTE*)h, sizeof(*h), &e)) {
        if (pdwErr) *pdwErr = e;
        return (e == CLIP_NET_CLOSED) ? -1 : 0;
    }
    if (h->Magic != CLIP_FRAME_MAGIC) {
        if (pdwErr) *pdwErr = ERROR_INVALID_DATA; return 0;
    }
    if (h->Version != CLIP_PROTO_VERSION) {
        if (pdwErr) *pdwErr = ERROR_NOT_SUPPORTED; return 0;
    }
    if (h->KeyTag != g.keyTag) {
        if (pdwErr) *pdwErr = ERROR_ACCESS_DENIED; return 0;
    }
    if (h->Length > cap || h->Length > CLIP_MAX_FRAME_PAYLOAD) {
        if (pdwErr) *pdwErr = ERROR_INVALID_DATA; return 0;
    }
    if (h->Length && !SockRecvAll(g.sock, payload, h->Length, &e)) {
        if (pdwErr) *pdwErr = e;
        return (e == CLIP_NET_CLOSED) ? -1 : 0;
    }
    if (FrameCrc(payload, h->Length) != h->Crc32) {
        if (pdwErr) *pdwErr = ERROR_CRC; return 0;
    }
    if (pLen) *pLen = h->Length;
    return 1;
}

// ============================================================
// Name sanitation. Six rules agreed before this part:
//  1  strip  \ / : * ? " < > |  and 0x00-0x1F
//  2  take basename after the last '\' OR '/'  (zip entries use '/')
//  3  ':' removal also kills NTFS alternate data streams
//  4  trim leading/trailing spaces, trailing dots; prefix reserved devices
//  5  '.' '..' and empty -> clip_<seq>
//  6  truncate to cap-1 while keeping the extension
//
// Clarification: a local CF_HDROP path like C:\dir\f.txt is LEGAL input;
// rule 2 reduces it to "f.txt" first. The rejection below applies only to
// a name that is STILL a bare drive root or a UNC root after that step,
// i.e. a cross-wire name that was never sanitized by the peer.
// ============================================================
static BOOL ClipIsReservedStem(const WCHAR* s, ULONG n)
{
    static const char* dev[] = {
        "CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5",
        "COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4",
        "LPT5","LPT6","LPT7","LPT8","LPT9", NULL
    };
    char a[16];
    ULONG i, k;
    if (!n || n >= 16) return FALSE;
    for (i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (c >= L'a' && c <= L'z') c = (WCHAR)(c - L'a' + L'A');
        a[i] = (c < 128) ? (char)c : '?';
    }
    a[n] = '\0';
    for (k = 0; dev[k]; k++)
        if (lstrcmpA(a, dev[k]) == 0) return TRUE;
    return FALSE;
}

static BOOL ClipIsIllegal(WCHAR c)
{
    if (c < 0x20) return TRUE;
    switch (c) {
    case L'\\': case L'/': case L':': case L'*': case L'?':
    case L'"':  case L'<': case L'>': case L'|':
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL ClipSanitizeName(const WCHAR* raw, WCHAR* out, ULONG cap,
                      ULONGLONG seq)
{
    const WCHAR* p;
    const WCHAR* start;
    const WCHAR* dot = NULL;
    WCHAR   tmp[CLIP_NAME_W * 2];
    ULONG   n = 0, i, extLen = 0, stemLen;
    BOOL    reserved;

    if (!raw || !out || cap < 8) return FALSE;
    out[0] = L'\0';

    p = raw;
    if (p[0] == L'\\' && p[1] == L'\\') {
        // UNC with no file component: refuse to land anything.
        if (!p[2]) return FALSE;
    }
    if (p[0] && p[1] == L':' && !p[2]) return FALSE;   // bare "X:"

    // rule 2: basename over BOTH separators
    start = p;
    for (; *p; p++)
        if (*p == L'\\' || *p == L'/') start = p + 1;
    if (!*start) return FALSE;

    // rules 1 and 3
    for (p = start; *p; p++) {
        if (ClipIsIllegal(*p)) continue;
        if (n >= CLIP_NAME_W * 2 - 1) break;
        tmp[n++] = *p;
    }
    tmp[n] = L'\0';
    if (!n) {
        StringCchPrintfW(out, cap, L"clip_%I64u", seq);
        return TRUE;
    }

    // rule 4: trim
    while (n && (tmp[n-1] == L' ' || tmp[n-1] == L'.')) tmp[--n] = L'\0';
    i = 0;
    while (i < n && tmp[i] == L' ') i++;
    if (i) {
        MoveMemory(tmp, tmp + i, (n - i + 1) * sizeof(WCHAR));
        n -= i;
    }
    if (!n || (n == 1 && tmp[0] == L'.') ||
        (n == 2 && tmp[0] == L'.' && tmp[1] == L'.')) {
        StringCchPrintfW(out, cap, L"clip_%I64u", seq);
        return TRUE;
    }

    // reserved device check on the stem (CON.txt also matches)
    for (i = 0; i < n; i++)
        if (tmp[i] == L'.') { dot = tmp + i; break; }
    stemLen = dot ? (ULONG)(dot - tmp) : n;
    reserved = ClipIsReservedStem(tmp, stemLen);
    if (dot) extLen = n - stemLen;

    // rule 6: truncate but keep the extension
    if (n > cap - 1) {
        ULONG keepStem;
        if (extLen && extLen < cap - 2) keepStem = cap - 1 - extLen - 1;
        else                           keepStem = cap - 1;
        if (keepStem > n) keepStem = n;
        if (reserved) {
            out[0] = L'_';
            CopyMemory(out + 1, tmp, keepStem * sizeof(WCHAR));
            out[keepStem + 1] = L'\0';
        } else {
            CopyMemory(out, tmp, keepStem * sizeof(WCHAR));
            out[keepStem] = L'\0';
        }
        if (extLen && keepStem + extLen + 1 < cap) {
            CopyMemory(out + (reserved ? keepStem + 1 : keepStem),
                       dot, extLen * sizeof(WCHAR));
            out[(reserved ? keepStem + 1 : keepStem) + extLen] = L'\0';
        }
        return TRUE;
    }

    if (reserved) {
        out[0] = L'_';
        CopyMemory(out + 1, tmp, (n + 1) * sizeof(WCHAR));
    } else {
        CopyMemory(out, tmp, (n + 1) * sizeof(WCHAR));
    }
    return TRUE;
}

BOOL ClipMakeTempName(WCHAR* out, ULONG cap, const WCHAR* suffix,
                      ULONGLONG seq)
{
    WCHAR dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cap,
        L"%s%s%lu_%I64u%s", dir, L"ClipAgent_",
        (ULONG)GetCurrentProcessId(), seq, suffix ? suffix : L""));
}

BOOL ClipMakePartName(const WCHAR* finalName, WCHAR* out, ULONG cap)
{
    return SUCCEEDED(StringCchPrintfW(out, cap, L"%s%s",
                                      finalName, L".part"));
}

BOOL ClipEnsureDir(const WCHAR* dir)
{
    DWORD a;
    if (!dir || !dir[0]) return FALSE;
    a = GetFileAttributesW(dir);
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!CreateDirectoryW(dir, NULL)) {
        if (GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
        return FALSE;
    }
    return TRUE;
}

VOID ClipDeleteIfExists(const WCHAR* path)
{
    if (!path || !path[0]) return;
    DeleteFileW(path);
}

// ============================================================
// Clipboard read. CF_HDROP carries paths only; never file bytes.
// ============================================================
BOOL ClipGetDropPaths(WCHAR*** pPaths, PULONG pCount)
{
    HDROP  hDrop;
    UINT   n, i;
    WCHAR** arr;

    if (pPaths) *pPaths = NULL;
    if (pCount) *pCount = 0;
    if (!OpenClipboard(NULL)) return FALSE;
    hDrop = (HDROP)GetClipboardData(CF_HDROP);
    if (!hDrop) { CloseClipboard(); return FALSE; }
    n = DragQueryFileW(hDrop, 0xFFFFFFFFu, NULL, 0);
    if (!n || n > CLIP_MAX_PATHS) { CloseClipboard(); return FALSE; }

    arr = (WCHAR**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                             (SIZE_T)n * sizeof(WCHAR*));
    if (!arr) { CloseClipboard(); return FALSE; }
    for (i = 0; i < n; i++) {
        UINT len = DragQueryFileW(hDrop, i, NULL, 0);
        WCHAR* s = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     ((SIZE_T)len + 2) * sizeof(WCHAR));
        if (!s) break;
        DragQueryFileW(hDrop, i, s, len + 1);
        arr[i] = s;
    }
    CloseClipboard();
    if (i != n) {
        for (UINT k = 0; k < i; k++) FreeBuf(arr[k], 0);
        FreeBuf(arr, (ULONG)(n * sizeof(WCHAR*)));
        return FALSE;
    }
    if (pPaths) *pPaths = arr;
    if (pCount) *pCount = n;
    return TRUE;
}

BOOL ClipGetUnicodeText(WCHAR** pText, PULONGLONG pBytes)
{
    HGLOBAL  h;
    LPCWSTR  src;
    SIZE_T   cch;
    WCHAR*   dst;

    if (pText)  *pText  = NULL;
    if (pBytes) *pBytes = 0;
    if (!OpenClipboard(NULL)) return FALSE;
    h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return FALSE; }
    src = (LPCWSTR)GlobalLock(h);
    if (!src) { CloseClipboard(); return FALSE; }
    cch = 0;
    while (cch < CLIP_MAX_TEXT_WCH && src[cch]) cch++;
    dst = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                            (cch + 1) * sizeof(WCHAR));
    if (dst) {
        CopyMemory(dst, src, cch * sizeof(WCHAR));
        dst[cch] = L'\0';
        if (pText)  *pText  = dst;
        if (pBytes) *pBytes = (ULONGLONG)cch * sizeof(WCHAR);
    }
    GlobalUnlock(h);
    CloseClipboard();
    return dst != NULL;
}

BOOL ClipSetUnicodeText(const WCHAR* text)
{
    SIZE_T   cch, bytes;
    HGLOBAL  h;
    LPWSTR   p;
    BOOL     ok = FALSE;
    if (!text) return FALSE;
    cch = (SIZE_T)lstrlenW(text);
    if (cch > CLIP_MAX_TEXT_WCH) return FALSE;
    bytes = (cch + 1) * sizeof(WCHAR);
    if (!OpenClipboard(NULL)) return FALSE;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        p = (LPWSTR)GlobalLock(h);
        if (p) {
            CopyMemory(p, text, bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// ============================================================
// Advert total. Recursion is depth-capped and never reads file data.
// ============================================================
static void CalcOne(const WCHAR* path, ULONG depth, PULONGLONG pTotal,
                    PULONGLONG pLargest, PULONG pFiles, PULONG pDirs)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    WCHAR  spec[MAX_PATH];
    WCHAR  child[MAX_PATH];
    HANDLE h;
    WIN32_FIND_DATAW fd;

    if (depth > CLIP_MAX_DEPTH) return;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;

    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        (*pDirs)++;
        StringCchPrintfW(spec, MAX_PATH, L"%s\\*", path);
        h = FindFirstFileW(spec, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!lstrcmpW(fd.cFileName, L".") ||
                !lstrcmpW(fd.cFileName, L"..")) continue;
            StringCchPrintfW(child, MAX_PATH, L"%s\\%s", path, fd.cFileName);
            CalcOne(child, depth + 1, pTotal, pLargest, pFiles, pDirs);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return;
    }

    ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    (*pFiles)++;
    *pTotal += sz;
    if (sz > *pLargest) *pLargest = sz;
}

BOOL ClipCalcTotal(WCHAR** paths, ULONG count, PULONGLONG pTotal,
                   PULONGLONG pLargest, PULONG pFiles, PULONG pDirs)
{
    ULONG i;
    if (pTotal)   *pTotal   = 0;
    if (pLargest) *pLargest = 0;
    if (pFiles)   *pFiles   = 0;
    if (pDirs)    *pDirs    = 0;
    if (!paths || !count) return FALSE;
    for (i = 0; i < count; i++)
        CalcOne(paths[i], 0, pTotal, pLargest, pFiles, pDirs);
    return TRUE;
}

VOID PendingClear(VOID)
{
    ULONG i;
    for (i = 0; i < g.pendingCount; i++) {
        if (g.pendingPaths[i]) {
            ULONG n = (ULONG)(lstrlenW(g.pendingPaths[i]) + 1) * sizeof(WCHAR);
            ClipZeroAndFree(g.pendingPaths[i], n);
        }
    }
    if (g.pendingPaths) {
        ClipZeroAndFree(g.pendingPaths,
                        g.pendingCap * (ULONG)sizeof(WCHAR*));
        g.pendingPaths = NULL;
    }
    g.pendingCount = 0;
    g.pendingCap   = 0;
    if (g.pendingText) {
        ClipZeroAndFree(g.pendingText, (ULONG)g.pendingTextBytes + sizeof(WCHAR));
        g.pendingText       = NULL;
        g.pendingTextBytes  = 0;
    }
    ZeroMemory(&g.pendingList, sizeof(g.pendingList));
}

BOOL PendingEnsure(ULONG need)
{
    WCHAR** np;
    ULONG   cap, i;
    if (need <= g.pendingCap) return TRUE;
    cap = g.pendingCap ? g.pendingCap : 16;
    while (cap < need) {
        if (cap > CLIP_MAX_PATHS / 2) { cap = CLIP_MAX_PATHS; break; }
        cap *= 2;
    }
    if (cap > CLIP_MAX_PATHS) cap = CLIP_MAX_PATHS;
    if (need > cap) return FALSE;
    np = (WCHAR**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                            (SIZE_T)cap * sizeof(WCHAR*));
    if (!np) return FALSE;
    for (i = 0; i < g.pendingCount; i++) np[i] = g.pendingPaths[i];
    if (g.pendingPaths)
        ClipZeroAndFree(g.pendingPaths, g.pendingCap * (ULONG)sizeof(WCHAR*));
    g.pendingPaths = np;
    g.pendingCap   = cap;
    return TRUE;
}// ClipAgent.cpp part 02 -- state machine, cleanup, interruption, sweep.
// ASCII only. // comments only. No non-ASCII bytes in this source.
// Chinese UI strings are defined in part 03 as L"\uXXXX" wide literals.

// ============================================================
// Contracts for part 04. Written here so the merge stays consistent.
//  1) g.sock is set to INVALID_SOCKET explicitly by main() before any
//     socket call. Global zero-init leaves it 0, which is NOT valid here.
//  2) ReceiverBegin() must name the .part file with the SAME
//     ClipAgent_<pid>_<seq> prefix used by ClipMakeTempName(), e.g.
//     "<outdir>\\ClipAgent_1234_7.part". The FINAL name (sanitized
//     basename) is applied only by MoveFileEx after CRC success.
//     This lets SweepStaleFiles() use one pattern "ClipAgent_*" for
//     both %TEMP% and --outdir without ever touching user .part files.
//  3) Passive pause: HandleFrame(FRAME_PAUSE) calls
//       CleanupTransfer(CLIP_R_PAUSE_KEEP, TRUE)
//       TransferSetState(CLIP_PAUSED)
//     directly. It must NOT call PauseTransfer(), or the frame echoes.
//  4) NetThread's main loop uses select() with a 200 ms timeout. Each
//     tick it polls g_clipResumeKick. If set AND g.zipTemp[0] != 0
//     (this end is the sender), it calls
//       BuildAndSendPayload(g.requestId, TRUE)
//     to continue from g.bytesDone. If this end is the receiver it just
//     ignores the kick and waits for the peer's FRAME_DATA.
//  5) HandleFrame(FRAME_RESUME) on the sender calls
//       BuildAndSendPayload(requestId, TRUE)
//     on the receiver reopens g.partPath with FILE_APPEND_DATA.
//     HandleFrame(FRAME_DATA) must reopen g.hPart if it is NULL while
//     state == CLIP_TRANSFER and g.partPath[0] != 0.
// ============================================================

// part02 -> part04 internal kick. Polled by NetThread's select loop.
volatile LONG g_clipResumeKick   = 0;
ULONG         g_clipResumeAction = 0;

// ---- helpers, file scope ----
static ULONG ClipParsePidFromName(const WCHAR* name)
{
    const WCHAR* p = name;
    ULONG pid = 0;
    if (!name) return 0;
    while (*p && *p != L'_') p++;          // skip "ClipAgent"
    if (!*p) return 0;
    p++;
    if (*p < L'0' || *p > L'9') return 0;  // not our naming scheme
    while (*p >= L'0' && *p <= L'9') {
        pid = pid * 10u + (ULONG)(*p - L'0');
        p++;
    }
    return pid;
}

static BOOL ClipAgeMinutes(const WCHAR* full, PULONG pMin)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER ft, now;
    FILETIME fnow;
    ULONGLONG d;
    if (pMin) *pMin = 0;
    if (!full || !GetFileAttributesExW(full, GetFileExInfoStandard, &fad))
        return FALSE;
    ft.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    GetSystemTimeAsFileTime(&fnow);
    now.LowPart  = fnow.dwLowDateTime;
    now.HighPart = fnow.dwHighDateTime;
    if (now.QuadPart <= ft.QuadPart) return TRUE;
    d = (ULONGLONG)(now.QuadPart - ft.QuadPart);
    if (pMin) *pMin = (ULONG)(d / (10000000ull * 60ull));
    return TRUE;
}

static BOOL ClipIsActivePath(const WCHAR* full)
{
    if (!full || !full[0]) return FALSE;
    if (g.zipTemp[0]  && !lstrcmpiW(full, g.zipTemp))  return TRUE;
    if (g.cdTemp[0]   && !lstrcmpiW(full, g.cdTemp))   return TRUE;
    if (g.partPath[0] && !lstrcmpiW(full, g.partPath)) return TRUE;
    return FALSE;
}

static void SweepJoin(WCHAR* out, ULONG cap,
                      const WCHAR* dir, const WCHAR* name)
{
    size_t n;
    if (!dir || !name) { if (out && cap) out[0] = L'\0'; return; }
    n = (size_t)lstrlenW(dir);
    if (n && dir[n - 1] == L'\\')
        StringCchPrintfW(out, cap, L"%s%s", dir, name);
    else
        StringCchPrintfW(out, cap, L"%s\\%s", dir, name);
}

static void SweepOneDir(const WCHAR* dir, BOOL atStartup)
{
    WCHAR spec[MAX_PATH];
    WCHAR full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONG  myPid = GetCurrentProcessId();

    if (!dir || !dir[0]) return;
    SweepJoin(spec, MAX_PATH, dir, L"ClipAgent_*");
    h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        ULONG pid, age = 0;
        BOOL  active, keep = FALSE;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        pid = ClipParsePidFromName(fd.cFileName);
        if (pid == 0) continue;            // not ours, never delete

        SweepJoin(full, MAX_PATH, dir, fd.cFileName);
        active = ClipIsActivePath(full);

        if (active && !atStartup) {
            if (g.state == CLIP_TRANSFER) {
                keep = TRUE;
            } else if (g.state == CLIP_PAUSED) {
                ClipAgeMinutes(full, &age);
                if (age < CLIP_PAUSE_TTL_MIN) {
                    keep = TRUE;
                } else {
                    Log(LOG_WARN,
                        "paused transfer expired (%u min), temp files removed",
                        age);
                    TrayBalloon("ClipAgent",
                                "paused transfer expired, temp files removed",
                                0x00000001);
                }
            }
        }

        if (!keep) {
            DeleteFileW(full);
            Log(LOG_INFO,
                "swept stale: pid=%u mine=%u age=%umin active=%d startup=%d",
                pid, myPid, age, (int)active, (int)atStartup);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ============================================================
// State
// ============================================================
VOID TransferSetState(CLIP_STATE st)
{
    g.state = st;
    switch (st) {
    case CLIP_IDLE:
        TraySetTip("ClipAgent: idle"); break;
    case CLIP_HAVE_LIST:
        TraySetTip("ClipAgent: remote content ready"); break;
    case CLIP_TRANSFER:
        TraySetTip("ClipAgent: transferring"); break;
    case CLIP_PAUSED:
        TraySetTip("ClipAgent: paused"); break;
    default:
        TraySetTip("ClipAgent"); break;
    }
}

BOOL TransferIsAborted(VOID)
{
    return InterlockedCompareExchange(&g.abort, 0, 0) != 0;
}

VOID TransferRequestAbort(VOID)
{
    // Both flags: g.abort stops the 64 KiB stream loops in part 04,
    // g.zip.Abort stops any in-flight compression callback in miniz.c.
    InterlockedExchange(&g.abort, 1);
    InterlockedExchange(&g.zip.Abort, 1);
}

// ============================================================
// Single idempotent teardown. Every exit routes through here.
//   keepPartial == TRUE  -> PAUSE: zip + .part stay on disk, counters stay
//   keepPartial == FALSE -> OK / cancel / peer cancel / disconnect / fail:
//                           every temp artifact is removed, buffers zeroed
// ============================================================
VOID CleanupTransfer(CLIP_REASON reason, BOOL keepPartial)
{
    const char* r = "unknown";
    ULONG level = LOG_INFO;

    // 1) stop compression callbacks first
    InterlockedExchange(&g.zip.Abort, 1);

    // 2) close receiver .part handle (repeat-safe)
    if (g.hPart && g.hPart != INVALID_HANDLE_VALUE) {
        CloseHandle(g.hPart);
    }
    g.hPart = NULL;

    // 3) close any zip-side handles left open by a mid-compression abort
    if (g.zip.hZip && g.zip.hZip != INVALID_HANDLE_VALUE) {
        CloseHandle(g.zip.hZip);
        g.zip.hZip = INVALID_HANDLE_VALUE;
    }
    if (g.zip.hCd && g.zip.hCd != INVALID_HANDLE_VALUE) {
        CloseHandle(g.zip.hCd);
        g.zip.hCd = INVALID_HANDLE_VALUE;
    }

    // 4) temp artifacts
    if (!keepPartial) {
        ClipDeleteIfExists(g.zipTemp);
        ClipDeleteIfExists(g.cdTemp);
        ClipDeleteIfExists(g.partPath);
        ClipDeleteIfExists(g.zip.ZipPath);
        ClipDeleteIfExists(g.zip.CdPath);
        g.zipTemp[0]   = L'\0';
        g.cdTemp[0]    = L'\0';
        g.partPath[0]  = L'\0';
        g.finalPath[0] = L'\0';   // string only; the delivered file stays
    }
    // keepPartial keeps g.zipTemp / g.partPath for resume.
    ZeroMemory(&g.zip, sizeof(g.zip));
    InterlockedExchange(&g.zip.Abort, 1);  // stay aborted until Create resets

    // 5) counters and run-time buffers. NEVER clear on a pause.
    if (!keepPartial) {
        g.bytesDone   = 0;
        g.bytesTotal  = 0;
        g.blockSeq    = 0;
        g.wholeCrc    = 0;
        g.requestId   = 0;
        g.textMode    = FALSE;
        g.lastErr     = ERROR_SUCCESS;
        g.pauseTick   = 0;
        g.skipCount   = 0;
        g.skipComment[0] = '\0';
        ZeroMemory(&g.pauseInfo, sizeof(g.pauseInfo));
        InterlockedExchange(&g.abort, 0);
    }
    // pendingPaths / pendingText / pendingList are the ADVERT, not the
    // transfer: deliberately untouched. IDLE still has content to paste.

    // 6) state
    g.state = keepPartial ? CLIP_PAUSED : CLIP_IDLE;
    TransferSetState(g.state);

    // 7) log
    switch (reason) {
    case CLIP_R_OK:           r = "ok";           level = LOG_OK;   break;
    case CLIP_R_USER_CANCEL:  r = "user-cancel";  level = LOG_WARN; break;
    case CLIP_R_PEER_CANCEL:  r = "peer-cancel";  level = LOG_WARN; break;
    case CLIP_R_DISCONNECT:   r = "disconnect";   level = LOG_ERR;  break;
    case CLIP_R_TIMEOUT:      r = "timeout";      level = LOG_WARN; break;
    case CLIP_R_SHARING:      r = "sharing";      level = LOG_WARN; break;
    case CLIP_R_ACCESS:       r = "access";       level = LOG_WARN; break;
    case CLIP_R_DISK_FULL:    r = "disk-full";    level = LOG_ERR;  break;
    case CLIP_R_PATH:         r = "path";         level = LOG_WARN; break;
    case CLIP_R_PAUSE_KEEP:   r = "pause-keep";   level = LOG_WARN; break;
    default:                  r = "unknown";     level = LOG_INFO; break;
    }
    Log(level, "cleanup: reason=%s keep=%d state=%u",
        r, (int)keepPartial, (ULONG)g.state);
}

// ============================================================
// Startup / periodic sweep. Called at process start, on IDT_SWEEP,
// and once again at process exit.
// ============================================================
VOID SweepStaleFiles(BOOL atStartup)
{
    WCHAR tmp[MAX_PATH];
    if (g.tempDir[0]) {
        SweepOneDir(g.tempDir, atStartup);
    } else if (GetTempPathW(MAX_PATH, tmp)) {
        SweepOneDir(tmp, atStartup);
    }
    if (g.outDir[0]) SweepOneDir(g.outDir, atStartup);
}

// ============================================================
// T1/T2 local pause. Emits FRAME_PAUSE; the peer goes passive.
// ============================================================
VOID PauseTransfer(const CLIP_PAUSE* p)
{
    if (!p) return;
    CopyMemory(&g.pauseInfo, p, sizeof(CLIP_PAUSE));
    g.pauseTick = GetTickCount();

    CleanupTransfer(CLIP_R_PAUSE_KEEP, TRUE);   // sets PAUSED + tip

    if (g.sock != INVALID_SOCKET && g.sock != 0) {
        NetSendFrame(FRAME_PAUSE, g.requestId,
                     p, (ULONG)sizeof(CLIP_PAUSE), &g.lastErr);
    }
    Log(LOG_WARN, "paused: class=%u win32=%u req=%I64u",
        p->ErrClass, p->Win32Error, g.requestId);
    TrayBalloon("ClipAgent",
                "transfer paused - resolve then press Ctrl+Alt+V",
                0x00000001);
}

// ============================================================
// Resume after a pause. Only flips state, clears abort, emits the
// frame, and kicks the local NetThread. Chunk continuation lives in
// part 04 (BuildAndSendPayload / ReceiverWriteChunk).
//   action: 0=resume, 1=skip-subject, 2=terminate-owner
// ============================================================
BOOL ResumeTransfer(ULONG action)
{
    CLIP_RESUME r;
    if (g.state != CLIP_PAUSED) return FALSE;

    ZeroMemory(&r, sizeof(r));
    r.RequestId = g.requestId;
    r.Action    = action;
    r.Reserved  = 0;

    InterlockedExchange(&g.abort, 0);
    g.state = CLIP_TRANSFER;
    TransferSetState(CLIP_TRANSFER);

    g_clipResumeAction = action;
    InterlockedExchange(&g_clipResumeKick, 1);

    if (g.sock != INVALID_SOCKET && g.sock != 0) {
        NetSendFrame(FRAME_RESUME, g.requestId,
                     &r, (ULONG)sizeof(CLIP_RESUME), &g.lastErr);
    }
    Log(LOG_OK, "resume: action=%u req=%I64u", action, g.requestId);
    return TRUE;
}

// ============================================================
// T3: peer closed or WSA error. No retry, no resume, clean and idle.
// ============================================================
VOID HandleDisconnect(DWORD err)
{
    if (g.sock != INVALID_SOCKET && g.sock != 0) {
        shutdown(g.sock, SD_BOTH);
        closesocket(g.sock);
        g.sock = INVALID_SOCKET;
    }
    if (err == CLIP_NET_CLOSED)
        Log(LOG_WARN, "peer closed connection, transfer cleaned");
    else
        Log(LOG_ERR, "socket error %u, transfer cleaned", err);

    CleanupTransfer(CLIP_R_DISCONNECT, FALSE);
    TraySetTip("ClipAgent: disconnected");
}// ClipAgent.cpp part 03 -- UI, owner lookup, error split, choices.
// ASCII only. // comments only. Chinese appears ONLY as L"\uXXXX".
// WIN32_LEAN_AND_MEAN is defined in part 01, so OLE headers are
// included explicitly here. Mid-file #include is legal preprocessing.

#include <ole2.h>
#include <oleauto.h>
#include <objbase.h>
#include <exdisp.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Uuid.lib")

// ---- choice results. 0/1/2 match ResumeTransfer(action) in part 02. ----
#define CH_RESUME    0u
#define CH_SKIP      1u
#define CH_TERM      2u
#define CH_CANCEL    3u
#define CH_TIMEOUT   4u

#define IDC_CH_RESUME  2001
#define IDC_CH_SKIP    2002
#define IDC_CH_TERM    2003
#define IDC_CH_CANCEL  2004
#define IDC_CH_PROMPT  2005
#define IDC_CH_HINT    2006
#define IDC_CH_COUNT   2007

#define CLIP_CHOICE_W   560
#define CLIP_CHOICE_H   300
#define CLIP_CHOICE_SEC CLIP_DIALOG_TIMEOUT_SEC

static const WCHAR* WND_CLASS_W = L"ClipAgentHiddenWnd";

// ---- Chinese string table. Every literal is pure ASCII source bytes. ----
static const WCHAR* S_TITLE      = L"ClipAgent";
static const WCHAR* S_CHOOSE     = L"\u9009\u62e9\u5904\u7406\u65b9\u5f0f";
static const WCHAR* S_PASTE      = L"\u7c98\u8d34\u8fdc\u7aef\u5185\u5bb9";
static const WCHAR* S_CANCELTR   = L"\u53d6\u6d88\u5f53\u524d\u4f20\u8f93";
static const WCHAR* S_RESUME     = L"\u6062\u590d\u6682\u505c\u7684\u4f20\u8f93";
static const WCHAR* S_OPENDIR    = L"\u6253\u5f00\u843d\u5730\u76ee\u5f55";
static const WCHAR* S_SHOWLOG    = L"\u663e\u793a\u65e5\u5fd7\u7a97\u53e3";
static const WCHAR* S_EXIT       = L"\u9000\u51fa";
static const WCHAR* S_BTN_RESUME = L"\u7ee7\u7eed";
static const WCHAR* S_BTN_SKIP   = L"\u8df3\u8fc7";
static const WCHAR* S_BTN_TERM   = L"\u7ec8\u6b62";
static const WCHAR* S_BTN_CANCEL = L"\u53d6\u6d88";
static const WCHAR* S_HINT_BUSY  =
    L"\u5360\u7528: \u6587\u4ef6\u88ab\u4ee5\u4e0b\u8fdb\u7a0b\u6253\u5f00";
static const WCHAR* S_HINT_ACCESS =
    L"\u65e0\u6743\u9650: \u8bf7\u68c0\u67e5\u662f\u5426\u662f\u5176\u5b83"
    L"\u5185\u6838\u9a71\u52a8\u5728\u4fdd\u62a4";
static const WCHAR* S_HINT_SPACE =
    L"\u7a7a\u95f4\u4e0d\u8db3: \u8bf7\u6362\u76d8\u6216\u6e05\u7406\u76ee\u5f55";
static const WCHAR* S_HINT_PATH  = L"\u8def\u5f84\u9519\u8bef: \u8bf7\u68c0\u67e5\u76ee\u5f55";
static const WCHAR* S_HINT_OTHER = L"\u64cd\u4f5c\u5931\u8d25: \u8bf7\u67e5\u770b\u65e5\u5fd7";
static const WCHAR* S_CRITICAL   =
    L"\u5173\u952e\u7cfb\u7edf\u8fdb\u7a0b: \u4e0d\u5efa\u8bae\u7ec8\u6b62, "
    L"\u8bf7\u9009\u62e9\u6682\u505c";
static const WCHAR* S_COUNT      = L"%u \u79d2\u540e\u81ea\u52a8\u6682\u505c";
static const WCHAR* S_SKIPPED    = L"\u4ee5\u4e0b\u6587\u4ef6\u88ab\u8df3\u8fc7";
static const WCHAR* S_IDLE       = L"\u7a7a\u95f2";
static const WCHAR* S_READY      = L"\u8fdc\u7aef\u5185\u5bb9\u5c31\u7eea";
static const WCHAR* S_TRANSFERRING = L"\u4f20\u8f93\u4e2d";
static const WCHAR* S_PAUSED_TIP = L"\u6682\u505c\u4e2d";

// ---- choice-dialog shared state, owned by the UI thread ----
static HANDLE       g_hChoiceEvent = NULL;
static volatile LONG g_choiceActive = 0;
static volatile LONG g_choiceResult = CH_TIMEOUT;
static char         g_choicePrompt[1024] = { 0 };
static ULONG        g_choiceClass = CLIPERR_OTHER;
static int          g_choiceLeft = CLIP_CHOICE_SEC;
static HWND         g_hwndChoice = NULL;

// ---- exposed to part 04 for the CH_TERM branch ----
volatile ULONG      g_choiceOwnerPid = 0;
char                g_choiceOwnerName[64] = { 0 };

// ---- exposed to part 04: NetThread polls these in its select loop ----
volatile LONG       g_clipPullKick = 0;

// ============================================================
// Critical-process whitelist. Never terminate these.
// ============================================================
BOOL ClipIsCriticalProcess(const WCHAR* exeName)
{
    static const char* bad[] = {
        "csrss.exe","wininit.exe","services.exe","lsass.exe","smss.exe",
        "winlogon.exe","svchost.exe","dwm.exe","System","Registry",
        "explorer.exe","fontdrvhost.exe","sihost.exe","RuntimeBroker.exe",
        "audiodg.exe","conhost.exe","MsMpEng.exe", NULL
    };
    char a[MAX_PATH];
    WCHAR base[MAX_PATH];
    const WCHAR* p;
    ULONG i, k;

    if (!exeName || !exeName[0]) return TRUE;
    p = exeName;
    for (const WCHAR* q = exeName; *q; q++)
        if (*q == L'\\' || *q == L'/') p = q + 1;
    StringCchCopyW(base, MAX_PATH, p);
    for (i = 0; base[i] && i < MAX_PATH - 1; i++) {
        WCHAR c = base[i];
        if (c >= L'A' && c <= L'Z') c = (WCHAR)(c - L'A' + L'a');
        a[i] = (c < 128) ? (char)c : '?';
    }
    a[i] = '\0';
    for (k = 0; bad[k]; k++)
        if (lstrcmpiA(a, bad[k]) == 0) return TRUE;
    return FALSE;
}

// ============================================================
// Tray helpers. logLock guards the shared NOTIFYICONDATAW buffer;
// Log() only calls TrayBalloon AFTER releasing logLock, so no deadlock.
// ============================================================
VOID TrayBalloon(const char* title, const char* text, DWORD flags)
{
    WCHAR t[64], b[256];
    if (!g.trayAdded) return;
    MultiByteToWideChar(CP_ACP, 0, title ? title : "ClipAgent", -1, t, 64);
    MultiByteToWideChar(CP_ACP, 0, text  ? text  : "", -1, b, 256);
    EnterCriticalSection(&g.logLock);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    StringCchCopyW(g.nid.szInfoTitle, 64, t);
    StringCchCopyW(g.nid.szInfo, 256, b);
    g.nid.dwInfoFlags = flags ? flags : NIIF_INFO;
    g.nid.uTimeout = 3000;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.szInfo[0] = L'\0';
    g.nid.szInfoTitle[0] = L'\0';
    LeaveCriticalSection(&g.logLock);
}

VOID TraySetTip(const char* tip)
{
    WCHAR w[128];
    if (!g.trayAdded) return;
    MultiByteToWideChar(CP_ACP, 0, tip ? tip : "ClipAgent", -1, w, 128);
    EnterCriticalSection(&g.logLock);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    StringCchCopyW(g.nid.szTip, 128, w);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    LeaveCriticalSection(&g.logLock);
}

VOID UiShowLog(BOOL show)
{
    HWND c = GetConsoleWindow();
    if (!c) return;
    ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    g.consoleShown = show ? TRUE : FALSE;
}

// ============================================================
// Choice window. Non-modal, lives on the UI thread, 60 s countdown.
// AskUserChoice() is called from NetThread and blocks on g_hChoiceEvent,
// so the main message pump stays responsive (tray + hotkeys alive).
// ============================================================
static VOID UiSetChoiceResult(ULONG r)
{
    if (!InterlockedCompareExchange(&g_choiceActive, 0, 0)) return;
    InterlockedExchange(&g_choiceResult, (LONG)r);
    InterlockedExchange(&g_choiceActive, 0);
    if (g_hwndChoice) {
        KillTimer(g_hwndChoice, IDT_CHOICE);
        DestroyWindow(g_hwndChoice);
        g_hwndChoice = NULL;
        g.hwndDlg = NULL;
    }
    if (g_hChoiceEvent) SetEvent(g_hChoiceEvent);
}

static VOID UiSetChildFont(HWND parent)
{
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c = GetWindow(parent, GW_CHILD);
    while (c) {
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        c = GetWindow(c, GW_HWNDNEXT);
    }
}

static VOID UiUpdateCountText(VOID)
{
    WCHAR w[128];
    if (!g_hwndChoice) return;
    StringCchPrintfW(w, 128, S_COUNT, (ULONG)g_choiceLeft);
    SetDlgItemTextW(g_hwndChoice, IDC_CH_COUNT, w);
}

static LRESULT CALLBACK ChoiceWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_CH_RESUME: UiSetChoiceResult(CH_RESUME); return 0;
        case IDC_CH_SKIP:   UiSetChoiceResult(CH_SKIP);   return 0;
        case IDC_CH_TERM:   UiSetChoiceResult(CH_TERM);   return 0;
        case IDC_CH_CANCEL: UiSetChoiceResult(CH_CANCEL); return 0;
        default: break;
        }
        break;
    case WM_TIMER:
        if (w == IDT_CHOICE) {
            g_choiceLeft--;
            UiUpdateCountText();
            if (g_choiceLeft <= 0) UiSetChoiceResult(CH_TIMEOUT);
            return 0;
        }
        break;
    case WM_CLOSE:
        return 0;   // ignore: only the four buttons or the timeout decide
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
static BOOL ClipIsCriticalProcessA(const char* n);
static BOOL UiCreateChoiceWindow(VOID)
{
    WNDCLASSEXW wc;
    WCHAR promptW[1024];
    const WCHAR* hint;
    HWND hp, hh, hc, b1, b2, b3, b4;
    int x, y;
    BOOL termAllowed;

    if (g_hwndChoice) return TRUE;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ChoiceWndProc;
    wc.hInstance = g.hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ClipAgentChoiceWnd";
    RegisterClassExW(&wc);

    x = (GetSystemMetrics(SM_CXSCREEN) - CLIP_CHOICE_W) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - CLIP_CHOICE_H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_hwndChoice = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        L"ClipAgentChoiceWnd", S_CHOOSE,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, CLIP_CHOICE_W, CLIP_CHOICE_H,
        g.hwnd, NULL, g.hInst, NULL);
    if (!g_hwndChoice) return FALSE;
    g.hwndDlg = g_hwndChoice;

    MultiByteToWideChar(CP_ACP, 0, g_choicePrompt, -1, promptW, 1024);
    hp = CreateWindowExW(0, L"STATIC", promptW,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        16, 14, 520, 118, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_PROMPT, g.hInst, NULL);

    switch (g_choiceClass) {
    case CLIPERR_SHARING: hint = S_HINT_BUSY;   break;
    case CLIPERR_ACCESS:  hint = S_HINT_ACCESS; break;
    case CLIPERR_SPACE:   hint = S_HINT_SPACE;  break;
    case CLIPERR_PATH:    hint = S_HINT_PATH;   break;
    default:              hint = S_HINT_OTHER;  break;
    }
    hh = CreateWindowExW(0, L"STATIC", hint,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        16, 138, 520, 26, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_HINT, g.hInst, NULL);

    hc = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        16, 168, 520, 22, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_COUNT, g.hInst, NULL);

    b1 = CreateWindowExW(0, L"BUTTON", S_BTN_RESUME,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        16,  210, 120, 34, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_RESUME, g.hInst, NULL);
    b2 = CreateWindowExW(0, L"BUTTON", S_BTN_SKIP,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        150, 210, 120, 34, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_SKIP, g.hInst, NULL);
    b3 = CreateWindowExW(0, L"BUTTON", S_BTN_TERM,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        284, 210, 120, 34, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_TERM, g.hInst, NULL);
    b4 = CreateWindowExW(0, L"BUTTON", S_BTN_CANCEL,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        418, 210, 120, 34, g_hwndChoice,
        (HMENU)(UINT_PTR)IDC_CH_CANCEL, g.hInst, NULL);

    termAllowed = (g_choiceClass == CLIPERR_SHARING) &&
                  (g_choiceOwnerPid != 0) &&
                  (g_choiceOwnerName[0] != '\0') &&
                  !ClipIsCriticalProcessA(g_choiceOwnerName);
    if (!termAllowed) {
        EnableWindow(b3, FALSE);
        SetDlgItemTextW(g_hwndChoice, IDC_CH_HINT,
            (g_choiceClass == CLIPERR_SHARING) ? S_CRITICAL : hint);
    }

    UiSetChildFont(g_hwndChoice);
    (void)hp; (void)hh; (void)hc; (void)b1; (void)b2; (void)b4;

    g_choiceLeft = CLIP_CHOICE_SEC;
    UiUpdateCountText();
    SetTimer(g_hwndChoice, IDT_CHOICE, 1000, NULL);
    SetForegroundWindow(g_hwndChoice);
    return TRUE;
}

// Narrow-name wrapper so part 04 can reuse the whitelist with ASCII.
static BOOL ClipIsCriticalProcessA(const char* n)
{
    WCHAR w[MAX_PATH];
    if (!n) return TRUE;
    MultiByteToWideChar(CP_ACP, 0, n, -1, w, MAX_PATH);
    return ClipIsCriticalProcess(w);
}

// Called from NetThread ONLY. Posts to the UI thread, then waits.
ULONG AskUserChoice(const char* prompt)
{
    if (!g_hChoiceEvent || !g.hwnd) {
        Log(LOG_WARN, "ui not ready, auto-pause instead of choice dialog");
        return CH_TIMEOUT;
    }
    if (InterlockedCompareExchange(&g_choiceActive, 0, 0))
        return CH_TIMEOUT;

    StringCchCopyA(g_choicePrompt, 1024, prompt ? prompt : "");
    ResetEvent(g_hChoiceEvent);
    InterlockedExchange(&g_choiceResult, (LONG)CH_TIMEOUT);
    InterlockedExchange(&g_choiceActive, 1);
    if (!PostMessageW(g.hwnd, WM_APP_CHOICE, 0, 0)) {
        InterlockedExchange(&g_choiceActive, 0);
        return CH_TIMEOUT;
    }
    WaitForSingleObject(g_hChoiceEvent, INFINITE);
    return (ULONG)InterlockedCompareExchange(&g_choiceResult, 0, 0);
}

// ============================================================
// Restart Manager owner lookup. Returns the count and fills an ASCII
// report; also records the first non-critical PID for CH_TERM.
// ============================================================
ULONG ClipQueryOwnerProcesses(const WCHAR* path, char* out, ULONG cap)
{
    DWORD  session = 0, reason = RmUnknownApp;
    WCHAR  key[CCH_RM_SESSION_KEY + 1];
    PCWSTR files[1];
    UINT   needed = 0, got = 0;
    RM_PROCESS_INFO* info = NULL;
    ULONG  i, count = 0;
    size_t used = 0;

    if (out && cap) out[0] = '\0';
    g_choiceOwnerPid = 0;
    g_choiceOwnerName[0] = '\0';
    if (!path) return 0;

    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) {
        if (out && cap)
            StringCchCopyA(out, cap, "owner lookup unavailable");
        return 0;
    }
    files[0] = path;
    if (RmRegisterResources(session, 1, files, 0, NULL, 0, NULL)
        != ERROR_SUCCESS) {
        RmEndSession(session);
        if (out && cap) StringCchCopyA(out, cap, "owner register failed");
        return 0;
    }

    RmGetList(session, &needed, &got, NULL, &reason);
    if (needed == 0) {
        RmEndSession(session);
        if (out && cap) StringCchCopyA(out, cap, "no owner reported");
        return 0;
    }
    info = (RM_PROCESS_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (SIZE_T)needed * sizeof(RM_PROCESS_INFO));
    if (!info) { RmEndSession(session); return 0; }
    got = needed;
    reason = RmUnknownApp;
    if (RmGetList(session, &needed, &got, info, &reason) != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, info);
        RmEndSession(session);
        return 0;
    }
    RmEndSession(session);

    for (i = 0; i < got; i++) {
        char* nm = W2A(info[i].strAppName);
        WCHAR exe[MAX_PATH];
        const WCHAR* p = info[i].strAppName;
        BOOL critical;
        char one[256];

        exe[0] = L'\0';
        {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, info[i].Process.dwProcessId);
            if (hProc) {
                DWORD sz = MAX_PATH;
                QueryFullProcessImageNameW(hProc, 0, exe, &sz);
                CloseHandle(hProc);
            }
        }
        critical = exe[0] ? ClipIsCriticalProcess(exe)
                          : ClipIsCriticalProcess(info[i].strAppName);
        (void)p;

        StringCchPrintfA(one, 256, "[%u] %s (PID %lu)%s\r\n",
            i + 1, nm ? nm : "unknown",
            (ULONG)info[i].Process.dwProcessId,
            critical ? "  [critical - do not terminate]" : "");
        if (out && cap && used + lstrlenA(one) < cap - 1) {
            StringCchCatA(out, cap, one);
            used += lstrlenA(one);
        }
        if (!critical && !g_choiceOwnerPid) {
            g_choiceOwnerPid = (ULONG)info[i].Process.dwProcessId;
            StringCchCopyA(g_choiceOwnerName, 64, nm ? nm : "unknown");
        }
        if (nm) FreeBuf(nm, (ULONG)(lstrlenA(nm) + 1));
        count++;
    }

    ZeroMemory(info, (ULONG)(got * sizeof(RM_PROCESS_INFO)));
    HeapFree(GetProcessHeap(), 0, info);
    return count;
}

// ============================================================
// Four-way error split. Returns the class; fills an ASCII report that
// ClipAgent feeds into the choice prompt and the log.
// ============================================================
ULONG ClipClassifyAndReport(const WCHAR* subject, DWORD err,
                            char* out, ULONG cap)
{
    ULONG cls = ClipClassifyError(err);
    char owners[1024];
    char sz[512];
    char* s = W2A(subject);
    char b[64];

    if (out && cap) out[0] = '\0';
    ClipFormatBytes(0, b, 64);  // keep helper linked; harmless no-op
    StringCchPrintfA(sz, 512,
        "subject: %s\r\nwin32: %u\r\nclass: %u\r\n",
        s ? s : "(none)", err, cls);
    if (out && cap) StringCchCatA(out, cap, sz);

    switch (cls) {
    case CLIPERR_SHARING:
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "sharing violation - file is open by another process\r\n");
        owners[0] = '\0';
        ClipQueryOwnerProcesses(subject, owners, 1024);
        if (out && cap) StringCchCatA(out, cap, owners);
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "\r\n[T] terminate  [S] pause  [K] skip  [C] cancel\r\n");
        break;
    case CLIPERR_ACCESS:
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "access denied - check protected dir, read-only attr,\r\n"
            "or your own minifilter / ObCallback blocking the write.\r\n"
            "try: sc stop MonDrv7   then paste again\r\n"
            "\r\n[S] pause  [K] skip  [C] cancel\r\n");
        break;
    case CLIPERR_SPACE:
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "disk full - free space or change --outdir\r\n"
            "\r\n[S] pause  [K] skip  [C] cancel\r\n");
        break;
    case CLIPERR_PATH:
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "path error - verify the target directory\r\n"
            "\r\n[S] pause  [K] skip  [C] cancel\r\n");
        break;
    default:
        StringCchCatA(out ? out : sz, out ? cap : 512,
            "unclassified error - see log\r\n"
            "\r\n[S] pause  [K] skip  [C] cancel\r\n");
        break;
    }
    if (s) FreeBuf(s, (ULONG)(lstrlenA(s) + 1));
    g_choiceClass = cls;
    return cls;
}

// ============================================================
// Foreground Explorer directory via IShellWindows (IDispatch only,
// so no vtable-name dependency). Falls back to --outdir.
// ============================================================
static BOOL ClipUrlToPath(const WCHAR* url, WCHAR* out, ULONG cap)
{
    static const WCHAR* prefix = L"file:///";
    char  raw[MAX_PATH * 3];
    const WCHAR* p;
    ULONG i, n = 0;

    if (!url || !out || cap < 4) return FALSE;
    p = url;
    for (i = 0; i < 8; i++)
        if (p[i] != prefix[i]) return FALSE;
    p += 8;
    for (; *p && n < sizeof(raw) - 1; p++) {
        WCHAR c = *p;
        if (c == L'%') {
            int hi = -1, lo = -1;
            WCHAR a = p[1], b = p[2];
            if (a >= L'0' && a <= L'9') hi = (int)(a - L'0');
            else if (a >= L'A' && a <= L'F') hi = (int)(a - L'A' + 10);
            else if (a >= L'a' && a <= L'f') hi = (int)(a - L'a' + 10);
            if (b >= L'0' && b <= L'9') lo = (int)(b - L'0');
            else if (b >= L'A' && b <= L'F') lo = (int)(b - L'A' + 10);
            else if (b >= L'b' && b <= L'f') lo = (int)(b - L'b' + 10);
            if (hi >= 0 && lo >= 0) {
                raw[n++] = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        raw[n++] = (c == L'/') ? '\\' : (c < 128 ? (char)c : '?');
    }
    raw[n] = '\0';
    if (n < 3 || raw[1] != ':') return FALSE;
    if (!MultiByteToWideChar(CP_UTF8, 0, raw, -1, out, cap)) return FALSE;
    return TRUE;
}

BOOL ClipGetForegroundDir(WCHAR* out, ULONG cap)
{
    HWND fg;
    IDispatch* sw = NULL;
    HRESULT hr;
    BOOL ok = FALSE, coInited = FALSE;

    if (!out || cap < 4) return FALSE;
    out[0] = L'\0';
    fg = GetForegroundWindow();
    if (!fg) goto fallback;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) goto fallback;
    if (FAILED(hr)) goto fallback;
    coInited = TRUE;

    if (FAILED(CoCreateInstance(CLSID_ShellWindows, NULL,
                                CLSCTX_LOCAL_SERVER,
                                IID_IDispatch, (void**)&sw)) || !sw)
        goto done;

    {
        DISPID didCount = 0, didItem = 0;
        OLECHAR* nc = (OLECHAR*)L"Count";
        OLECHAR* ni = (OLECHAR*)L"Item";
        VARIANT vres, varg;
        DISPPARAMS dp;
        long total = 0, i;

        if (FAILED(sw->GetIDsOfNames(IID_NULL, &nc, 1,
                       LOCALE_USER_DEFAULT, &didCount))) goto done;
        VariantInit(&vres);
        ZeroMemory(&dp, sizeof(dp));
        if (FAILED(sw->Invoke(didCount, IID_NULL, LOCALE_USER_DEFAULT,
                       DISPATCH_PROPERTYGET, &dp, &vres, NULL, NULL)))
            goto done;
        total = (vres.vt == VT_I4) ? vres.lVal : 0;
        VariantClear(&vres);
        if (total <= 0 || total > 256) goto done;

        if (FAILED(sw->GetIDsOfNames(IID_NULL, &ni, 1,
                       LOCALE_USER_DEFAULT, &didItem))) goto done;

        for (i = 0; i < total && !ok; i++) {
            IDispatch* item = NULL;
            IWebBrowser2* wb = NULL;
            SHANDLE_PTR hw = 0;
            BSTR url = NULL;

            VariantInit(&varg);
            varg.vt = VT_I4;
            varg.lVal = i;
            ZeroMemory(&dp, sizeof(dp));
            dp.rgvarg = &varg;
            dp.cArgs = 1;
            VariantInit(&vres);
            if (FAILED(sw->Invoke(didItem, IID_NULL, LOCALE_USER_DEFAULT,
                           DISPATCH_METHOD, &dp, &vres, NULL, NULL)))
                goto next;
            item = (vres.vt == VT_DISPATCH) ? vres.pdispVal : NULL;
            if (!item) goto next;
            if (SUCCEEDED(item->QueryInterface(IID_IWebBrowser2,
                                               (void**)&wb)) && wb) {
                if (SUCCEEDED(wb->get_HWND(&hw)) &&
                    (HWND)hw == fg &&
                    SUCCEEDED(wb->get_LocationURL(&url)) && url) {
                    ok = ClipUrlToPath(url, out, cap);
                    SysFreeString(url);
                }
                wb->Release();
            }
        next:
            VariantClear(&vres);
            if (item) item->Release();
            VariantClear(&varg);
        }
    }

done:
    if (sw) sw->Release();
    if (coInited) CoUninitialize();
    if (ok) return TRUE;

fallback:
    if (g.outDir[0]) {
        StringCchCopyW(out, cap, g.outDir);
        return TRUE;
    }
    return FALSE;
}

BOOL ClipCheckDiskSpace(const WCHAR* dir, ULONGLONG need)
{
    ULARGE_INTEGER freeBytes, totalBytes, totalFree;
    WCHAR path[MAX_PATH];
    if (!dir) return FALSE;
    StringCchCopyW(path, MAX_PATH, dir);
    if (!GetDiskFreeSpaceExW(path, &freeBytes, &totalBytes, &totalFree))
        return FALSE;
    return freeBytes.QuadPart >= (ULONGLONG)need;
}

VOID ClipShowSkipList(const char* comment)
{
    WCHAR w[1024];
    char* a;
    if (!comment || !comment[0]) return;
    Log(LOG_WARN, "skipped entries reported by sender: %s", comment);
    TrayBalloon("ClipAgent", "some entries were skipped - see log", 0x00000002);
    a = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024);
    if (a) {
        StringCchPrintfA(a, 1024, "%s", comment);
        StringCchPrintfW(w, 1024, L"%s\r\n%S", S_SKIPPED, a);
        MessageBoxW(g.hwnd ? g.hwnd : NULL, w, S_TITLE,
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        ZeroMemory(a, 1024);
        HeapFree(GetProcessHeap(), 0, a);
    }
}

// ============================================================
// Hidden window, hotkeys, tray menu, timers.
// ============================================================
BOOL UiRegisterHotKeys(VOID)
{
    BOOL a, b, c;
    a = RegisterHotKey(g.hwnd, HOTKEY_PASTE,
                       MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, L'V');
    b = RegisterHotKey(g.hwnd, HOTKEY_CANCEL,
                       MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, L'C');
    c = RegisterHotKey(g.hwnd, HOTKEY_LOG,
                       MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, L'S');
    if (!a || !b || !c)
        Log(LOG_WARN, "some hotkeys unavailable (paste=%d cancel=%d log=%d)",
            (int)a, (int)b, (int)c);
    return a && b;
}

static VOID UiDoPaste(VOID)
{
    if (g.state == CLIP_PAUSED) {
        ResumeTransfer(CH_RESUME);
        return;
    }
    if (g.state == CLIP_TRANSFER) {
        Log(LOG_WARN, "transfer already in progress, paste ignored");
        return;
    }
    InterlockedExchange(&g_clipPullKick, 1);
    Log(LOG_INFO, "Ctrl+Alt+V -> PULL queued");
}

static VOID UiDoCancel(VOID)
{
    if (InterlockedCompareExchange(&g_choiceActive, 0, 0)) {
        UiSetChoiceResult(CH_CANCEL);
        return;
    }
    // NetThread owns CleanupTransfer. Only raise the flags here;
    // part 04 detects g.abort, cleans, and emits FRAME_CANCEL.
    TransferRequestAbort();
    Log(LOG_WARN, "Ctrl+Alt+C -> abort requested");
}

static VOID UiTrayMenu(VOID)
{
    HMENU m = CreatePopupMenu();
    POINT p;
    char st[128];
    WCHAR w[256];

    if (!m) return;
    StringCchPrintfA(st, 128, "ClipAgent: %s",
        g.state == CLIP_IDLE ? "idle" :
        g.state == CLIP_HAVE_LIST ? "content ready" :
        g.state == CLIP_TRANSFER ? "transferring" : "paused");
    MultiByteToWideChar(CP_ACP, 0, st, -1, w, 256);
    InsertMenuW(m, 0, MF_BYPOSITION | MF_GRAYED | MF_STRING, 0, w);
    InsertMenuW(m, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(m, 2, MF_BYPOSITION | MF_STRING, IDM_PASTE, S_PASTE);
    InsertMenuW(m, 3, MF_BYPOSITION | MF_STRING, IDM_CANCEL, S_CANCELTR);
    InsertMenuW(m, 4, MF_BYPOSITION | MF_STRING, IDM_RESUME, S_RESUME);
    InsertMenuW(m, 5, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(m, 6, MF_BYPOSITION | MF_STRING, IDM_OPEN_OUTDIR, S_OPENDIR);
    InsertMenuW(m, 7, MF_BYPOSITION | MF_STRING, IDM_SHOWLOG, S_SHOWLOG);
    InsertMenuW(m, 8, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(m, 9, MF_BYPOSITION | MF_STRING, IDM_EXIT, S_EXIT);

    GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN,
                   p.x, p.y, 0, g.hwnd, NULL);
    DestroyMenu(m);
}

LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_HOTKEY:
        switch ((int)w) {
        case HOTKEY_PASTE:  UiDoPaste();  return 0;
        case HOTKEY_CANCEL: UiDoCancel(); return 0;
        case HOTKEY_LOG:    UiShowLog(!g.consoleShown); return 0;
        default: break;
        }
        break;

    case WM_APP_TRAY:
        switch ((UINT)l) {
        case WM_LBUTTONDBLCLK: UiShowLog(!g.consoleShown); return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:   UiTrayMenu(); return 0;
        default: break;
        }
        break;

    case WM_APP_CHOICE:
        UiCreateChoiceWindow();
        return 0;

    case WM_APP_PROGRESS:
        {
            char t[128];
            StringCchPrintfA(t, 128, "ClipAgent: transferring %u%%",
                             (ULONG)w);
            TraySetTip(t);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_PASTE:      UiDoPaste(); break;
        case IDM_CANCEL:     UiDoCancel(); break;
        case IDM_RESUME:
            if (g.state == CLIP_PAUSED) ResumeTransfer(CH_RESUME);
            break;
        case IDM_OPEN_OUTDIR:
            if (g.outDir[0])
                ShellExecuteW(NULL, L"open", g.outDir,
                              NULL, NULL, SW_SHOWNORMAL);
            break;
        case IDM_SHOWLOG:    UiShowLog(TRUE); break;
        case IDM_EXIT:       PostMessageW(h, WM_CLOSE, 0, 0); break;
        default: break;
        }
        return 0;

    case WM_TIMER:
        if (w == IDT_SWEEP) {
            SweepStaleFiles(FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

BOOL UiInit(HINSTANCE hInst)
{
    WNDCLASSEXW wc;

    // MUST be first: Log() uses this lock and may be called right after.
    InitializeCriticalSection(&g.logLock);

    g.hInst = hInst;
    g.uiThreadId = GetCurrentThreadId();
    g.hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    g.consoleShown = !g.quiet;

    g_hChoiceEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hChoiceEvent) return FALSE;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WND_CLASS_W;
    if (!RegisterClassExW(&wc)) return FALSE;

    g.hwnd = CreateWindowExW(0, WND_CLASS_W, WND_CLASS_W,
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g.hwnd) return FALSE;

    ZeroMemory(&g.nid, sizeof(g.nid));
    g.nid.cbSize = sizeof(NOTIFYICONDATAW);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = ID_TRAY_ICON;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    StringCchCopyW(g.nid.szTip, 128, L"ClipAgent");
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &g.nid) ? TRUE : FALSE;

    UiRegisterHotKeys();
    SetTimer(g.hwnd, IDT_SWEEP, 60000, NULL);
    return TRUE;
}

VOID UiShutdown(VOID)
{
    if (g.hwnd) {
        KillTimer(g.hwnd, IDT_SWEEP);
        UnregisterHotKey(g.hwnd, HOTKEY_PASTE);
        UnregisterHotKey(g.hwnd, HOTKEY_CANCEL);
        UnregisterHotKey(g.hwnd, HOTKEY_LOG);
    }
    if (InterlockedCompareExchange(&g_choiceActive, 0, 0))
        UiSetChoiceResult(CH_CANCEL);
    if (g.trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        g.trayAdded = FALSE;
    }
    if (g.hwnd) {
        DestroyWindow(g.hwnd);
        g.hwnd = NULL;
    }
    if (g_hChoiceEvent) {
        CloseHandle(g_hChoiceEvent);
        g_hChoiceEvent = NULL;
    }
    DeleteCriticalSection(&g.logLock);
}// ClipAgent.cpp part 04 -- net loop, frames, streaming, args, entry.
// ASCII only. // comments only. Chinese strings come from part 03.
#include <winsock2.h>   // already in part 01; harmless repeat, guard wins

// ---- part04-local helpers / state ----
static volatile LONG  g_suppressClip = 0;   // suppress self-clipboard echo
static ULONGLONG      s_nextReq = 0;
static ULONG          s_lastPct = 0;
static SOCKET         s_listen  = INVALID_SOCKET;

static ULONG NextRequestId(VOID)
{
    s_nextReq++;
    if (!s_nextReq) s_nextReq = 1;
    return (ULONG)s_nextReq ^ (ULONG)GetCurrentProcessId();
}

static BOOL Utf8FromWide(const WCHAR* w, char* out, ULONG cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0 || (ULONG)n > cap) return FALSE;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap,
                               NULL, NULL) > 0;
}

static VOID ProgressPost(ULONGLONG done, ULONGLONG total)
{
    ULONG pct;
    if (!total) return;
    pct = (ULONG)((done * 100ull) / total);
    if (pct > 100u) pct = 100u;
    if (pct == s_lastPct) return;
    s_lastPct = pct;
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_PROGRESS, (WPARAM)pct, 0);
}

static VOID SkipAdd(const char* rel, const char* why)
{
    char one[512];
    size_t used;
    StringCchPrintfA(one, 512, "skipped [%s]: %s\n", why ? why : "error",
                     rel ? rel : "(unnamed)");
    used = (size_t)lstrlenA(g.skipComment);
    if (used + lstrlenA(one) < CLIPZIP_MAX_COMMENT - 1) {
        StringCchCatA(g.skipComment, CLIPZIP_MAX_COMMENT, one);
        g.skipCount++;
    }
}

static BOOL TerminateOwner(VOID)
{
    HANDLE h;
    WCHAR  w[64];
    BOOL   ok = FALSE;
    if (!g_choiceOwnerPid) return FALSE;
    MultiByteToWideChar(CP_ACP, 0, g_choiceOwnerName, -1, w, 64);
    if (ClipIsCriticalProcess(w)) {
        Log(LOG_WARN, "refuse to terminate critical process");
        return FALSE;
    }
    h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)g_choiceOwnerPid);
    if (!h) return FALSE;
    ok = TerminateProcess(h, 1) ? TRUE : FALSE;
    CloseHandle(h);
    Log(ok ? LOG_OK : LOG_WARN, "terminate pid=%lu %s",
        (ULONG)g_choiceOwnerPid, ok ? "ok" : "failed");
    return ok;
}

// ============================================================
// Advert scan: clipboard changed -> store paths/text locally and send
// FRAME_LIST only. ZERO file bytes cross the wire at copy time.
// ============================================================
static VOID AdvertScan(VOID)
{
    ULONGLONG seq = (ULONGLONG)GetClipboardSequenceNumber();
    WCHAR**   paths = NULL;
    ULONG     n = 0, i;
    WCHAR*    txt = NULL;
    ULONGLONG bytes = 0, total = 0, largest = 0;
    ULONG     files = 0, dirs = 0;
    WCHAR     base[MAX_PATH];

    if (g.state == CLIP_TRANSFER || g.state == CLIP_PAUSED) return;
    if (g.sock == INVALID_SOCKET || g.sock == 0) return;

    if (InterlockedCompareExchange(&g_suppressClip, 0, 0) > 0) {
        InterlockedDecrement(&g_suppressClip);
        g.pendingClipSeq = seq;
        return;
    }
    if (seq == g.pendingClipSeq) return;
    g.pendingClipSeq = seq;

    PendingClear();

    if (ClipGetDropPaths(&paths, &n) && n) {
        if (!PendingEnsure(n)) {
            for (i = 0; i < n; i++)
                FreeBuf(paths[i], (ULONG)(lstrlenW(paths[i]) + 1) * sizeof(WCHAR));
            HeapFree(GetProcessHeap(), 0, paths);
            return;
        }
        for (i = 0; i < n; i++) g.pendingPaths[i] = paths[i];
        g.pendingCount = n;
        HeapFree(GetProcessHeap(), 0, paths);
        ClipCalcTotal(g.pendingPaths, g.pendingCount,
                      &total, &largest, &files, &dirs);
    } else if (ClipGetUnicodeText(&txt, &bytes)) {
        g.pendingText = txt;
        g.pendingTextBytes = bytes;
        total = bytes;
    } else {
        return;
    }

    ZeroMemory(&g.pendingList, sizeof(g.pendingList));
    g.pendingList.TotalBytes   = total;
    g.pendingList.LargestBytes = largest;
    g.pendingList.FileCount    = files;
    g.pendingList.DirCount     = dirs;
    g.pendingList.IsDir        = dirs ? 1u : 0u;
    g.pendingList.Reserved     = 0;
    g.pendingList.ClipSeq      = seq;
    if (g.pendingCount && ClipSanitizeName(g.pendingPaths[0], base,
                                           MAX_PATH, seq))
        StringCchCopyW(g.pendingList.RootName, CLIP_NAME_W, base);
    else if (g.pendingText)
        StringCchCopyW(g.pendingList.RootName, CLIP_NAME_W,
                       L"clipboard-text");

    if (!NetSendFrame(FRAME_LIST, seq, &g.pendingList,
                      (ULONG)sizeof(CLIP_LIST), &g.lastErr)) {
        Log(LOG_WARN, "advert send failed err=%u", g.lastErr);
        return;
    }
    TransferSetState(CLIP_HAVE_LIST);
    {
        char b[64];
        ClipFormatBytes(total, b, 64);
        Log(LOG_INFO, "advert: %s, files=%u dirs=%u", b, files, dirs);
    }
}

// ============================================================
// D2: pack one source into the temp zip. Any failure is resolved at
// FILE granularity so finalize() always runs -> never a half zip.
// ============================================================
typedef struct _PACKCTX {
    PCLIPZIP zip;
    BOOL*    pCancel;
    ULONG*   pEntries;
} PACKCTX;

static BOOL PackOne(PACKCTX* c, const WCHAR* full, const char* rel,
                    ULONG depth);

static BOOL PackDir(PACKCTX* c, const WCHAR* full, const char* relPrefix,
                    ULONG depth)
{
    WCHAR  spec[MAX_PATH];
    WCHAR  child[MAX_PATH];
    WCHAR  safe[MAX_PATH];
    char   relUtf8[CLIPZIP_MAX_ENTRY_UTF8];
    char   seg[512];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > CLIP_MAX_DEPTH) {
        SkipAdd(relPrefix, "depth-limit");
        return TRUE;
    }
    StringCchPrintfW(spec, MAX_PATH, L"%s\\*", full);
    h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        SkipAdd(relPrefix, "dir-open");
        return TRUE;
    }
    do {
        if (!lstrcmpW(fd.cFileName, L".") ||
            !lstrcmpW(fd.cFileName, L"..")) continue;
        if (*c->pCancel) break;
        if (!ClipSanitizeName(fd.cFileName, safe, MAX_PATH,
                              (ULONGLONG)depth))
            continue;
        if (!Utf8FromWide(safe, seg, 512)) continue;
        if (!lstrcmpA(seg, "..") || !lstrcmpA(seg, ".")) {
            SkipAdd(seg, "zip-slip");
            continue;
        }
        StringCchPrintfA(relUtf8, CLIPZIP_MAX_ENTRY_UTF8, "%s/%s",
                         relPrefix, seg);
        StringCchPrintfW(child, MAX_PATH, L"%s\\%s", full, fd.cFileName);
        if (!PackOne(c, child, relUtf8, depth + 1)) {
            FindClose(h);
            return FALSE;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return TRUE;
}

static BOOL PackOne(PACKCTX* c, const WCHAR* full, const char* rel,
                    ULONG depth)
{
    DWORD  a;
    DWORD  err = ERROR_SUCCESS;
    char   report[2048];
    ULONG  ch;

    if (*c->pCancel) return FALSE;
    a = GetFileAttributesW(full);
    if (a == INVALID_FILE_ATTRIBUTES) {
        SkipAdd(rel, "stat-failed");
        return TRUE;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY)
        return PackDir(c, full, rel, depth);

    if (ClipZipAddFile(c->zip, full, rel, &err)) {
        (*c->pEntries)++;
        return TRUE;
    }

    // D2 failure: classify, ask, resolve at file granularity.
    ClipClassifyAndReport(full, err, report, 2048);
    ch = AskUserChoice(report);

    if (ch == CH_CANCEL) {
        *c->pCancel = TRUE;
        return FALSE;
    }
    if (ch == CH_TERM) {
        TerminateOwner();
        err = ERROR_SUCCESS;
        if (ClipZipAddFile(c->zip, full, rel, &err)) {
            (*c->pEntries)++;
            return TRUE;
        }
        SkipAdd(rel, "retry-after-terminate");
        return TRUE;
    }
    if (ch == CH_RESUME) {
        err = ERROR_SUCCESS;
        if (ClipZipAddFile(c->zip, full, rel, &err)) {
            (*c->pEntries)++;
            return TRUE;
        }
        SkipAdd(rel, "retry-failed");
        return TRUE;
    }
    // CH_SKIP / CH_TIMEOUT: never pause here (no half zip may survive).
    SkipAdd(rel, ch == CH_TIMEOUT ? "timeout" : "user-skip");
    Log(LOG_WARN, "packing skipped one entry: %s (choice=%u)", rel, ch);
    return TRUE;
}

// ============================================================
// Cancel emission. Called only from NetThread (single-threaded state).
// ============================================================
static VOID EmitCancelAndCleanup(CLIP_REASON reason)
{
    CLIP_CANCEL c;
    ZeroMemory(&c, sizeof(c));
    c.Reason    = (ULONG)reason;
    c.RequestId = g.requestId;
    if (g.sock != INVALID_SOCKET && g.sock != 0)
        NetSendFrame(FRAME_CANCEL, g.requestId, &c,
                     (ULONG)sizeof(CLIP_CANCEL), &g.lastErr);
    CleanupTransfer(reason, FALSE);
}

// ============================================================
// Sender. resume=TRUE -> resume an existing finalized zip at bytesDone.
// Constant 64 KiB buffer; the temp zip is deleted on EVERY exit.
// ============================================================
BOOL BuildAndSendPayload(ULONGLONG requestId, BOOL resume)
{
    CLIP_ZIP_START zs;
    CLIP_ZIP_END   ze;
    HANDLE    hSrc = INVALID_HANDLE_VALUE;
    BYTE*     buf = NULL;
    ULONGLONG finalSize = 0, off = 0;
    ULONG     crc = 0xFFFFFFFFu;
    ULONG     entries = 0, sent = 0;
    BOOL      cancel = FALSE, ok = FALSE;
    DWORD     err = ERROR_SUCCESS;
    ULONG     seqNo = 0;
    PACKCTX   pc;

    s_lastPct = 0;
    if (g.sock == INVALID_SOCKET || g.sock == 0) return FALSE;
    if (!g.pendingCount && !g.pendingText) {
        Log(LOG_WARN, "nothing to paste (no local clipboard advert)");
        return FALSE;
    }
    if (resume && !g.zipTemp[0]) resume = FALSE;

    if (!resume) {
        CleanupTransfer(CLIP_R_OK, FALSE);   // normalise state before build
        g.requestId   = requestId ? requestId : (ULONGLONG)NextRequestId();
        g.bytesDone   = 0;
        g.blockSeq    = 0;
        g.skipCount   = 0;
        g.skipComment[0] = '\0';
        g.textMode    = (g.pendingText != NULL) && (g.pendingCount == 0);

        if (!g.textMode) {
            ULONGLONG tag = (ULONGLONG)GetTickCount64();
            if (!ClipMakeTempName(g.zipTemp, MAX_PATH,
                                  L".zip", tag) ||
                !ClipMakeTempName(g.cdTemp, MAX_PATH,
                                  L".cd", tag)) {
                Log(LOG_ERR, "temp name failed");
                return FALSE;
            }
            if (!ClipZipCreate(&g.zip, g.zipTemp, g.cdTemp)) {
                Log(LOG_ERR, "zip create failed");
                EmitCancelAndCleanup(CLIP_R_PATH);
                return FALSE;
            }
            InterlockedExchange(&g.zip.Abort, 0);

            pc.zip      = &g.zip;
            pc.pCancel  = &cancel;
            pc.pEntries = &entries;
            {
                WCHAR  safe[MAX_PATH];
                char   rel[CLIPZIP_MAX_ENTRY_UTF8];
                ULONG  i;
                for (i = 0; i < g.pendingCount && !cancel; i++) {
                    if (!ClipSanitizeName(g.pendingPaths[i], safe,
                                          MAX_PATH, i)) continue;
                    if (!Utf8FromWide(safe, rel,
                                      CLIPZIP_MAX_ENTRY_UTF8)) continue;
                    if (!PackOne(&pc, g.pendingPaths[i], rel, 0)) break;
                }
            }
            if (cancel) {
                ClipZipAbort(&g.zip);
                EmitCancelAndCleanup(CLIP_R_USER_CANCEL);
                return FALSE;
            }
            if (g.skipCount)
                ClipZipSetComment(&g.zip, g.skipComment);
            if (!ClipZipFinalize(&g.zip, &finalSize)) {
                Log(LOG_ERR, "zip finalize failed (too large or I/O)");
                ClipZipAbort(&g.zip);
                EmitCancelAndCleanup(CLIP_R_DISK_FULL);
                return FALSE;
            }
            g.bytesTotal = finalSize;
        } else {
            g.bytesTotal = g.pendingTextBytes;
        }
    } else {
        g.bytesTotal = g.zipTemp[0] ? g.bytesTotal : g.pendingTextBytes;
        InterlockedExchange(&g.zip.Abort, 0);
        Log(LOG_INFO, "resume from %I64u / %I64u bytes",
            g.bytesDone, g.bytesTotal);
    }

    buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, CLIP_CHUNK_SIZE);
    if (!buf) {
        EmitCancelAndCleanup(CLIP_R_DISK_FULL);
        return FALSE;
    }

    ZeroMemory(&zs, sizeof(zs));
    zs.ZipBytes   = g.bytesTotal;
    zs.EntryCount = g.textMode ? 0u : entries;
    zs.Reserved   = 0;
    if (!g.textMode) {
        WCHAR safe[MAX_PATH];
        if (g.pendingCount &&
            ClipSanitizeName(g.pendingPaths[0], safe, MAX_PATH, 0)) {
            // strip extension, force .zip
            WCHAR* d = safe;
            for (WCHAR* p = safe; *p; p++) if (*p == L'.') d = p;
            if (d != safe) *d = L'\0';
            StringCchPrintfW(zs.FileName, CLIP_NAME_W, L"%s.zip", safe);
        } else {
            StringCchCopyW(zs.FileName, CLIP_NAME_W, L"clipboard.zip");
        }
    } else {
        zs.FileName[0] = L'\0';   // text-mode marker (part 01 contract)
    }
    if (!NetSendFrame(FRAME_ZIP_START, g.requestId, &zs,
                      (ULONG)sizeof(CLIP_ZIP_START), &err)) {
        Log(LOG_ERR, "zip-start send failed err=%u", err);
        goto fail;
    }

    TransferSetState(CLIP_TRANSFER);

    if (!g.textMode) {
        hSrc = CreateFileW(g.zipTemp, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL |
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSrc == INVALID_HANDLE_VALUE) {
            Log(LOG_ERR, "open temp zip failed err=%u", GetLastError());
            goto fail;
        }
        if (g.bytesDone) {
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)g.bytesDone;
            if (!SetFilePointerEx(hSrc, li, NULL, FILE_BEGIN)) goto fail;
        }
    }

    off = g.bytesDone;
    seqNo = (ULONG)(off / CLIP_CHUNK_SIZE);
    while (off < g.bytesTotal) {
        ULONG want = (ULONG)((g.bytesTotal - off > CLIP_CHUNK_SIZE)
                             ? CLIP_CHUNK_SIZE
                             : (g.bytesTotal - off));
        DWORD got = 0;

        if (TransferIsAborted()) {
            EmitCancelAndCleanup(CLIP_R_USER_CANCEL);
            goto done;
        }
        if (g.textMode) {
            CopyMemory(buf, (const BYTE*)g.pendingText + off, want);
            got = want;
        } else {
            if (!ReadFile(hSrc, buf, want, &got, NULL) || got == 0) {
                Log(LOG_ERR, "read temp zip failed err=%u", GetLastError());
                goto fail;
            }
        }
        crc = ClipCrc32(crc, buf, got);
        if (!NetSendFrame(FRAME_DATA, (ULONGLONG)seqNo,
                          buf, got, &err)) {
            Log(LOG_ERR, "data send failed err=%u", err);
            goto fail;
        }
        off += got;
        g.bytesDone = off;
        g.blockSeq  = (ULONGLONG)seqNo + 1;
        seqNo++;
        sent += got;
        ZeroMemory(buf, got);
        ProgressPost(off, g.bytesTotal);
    }

    ZeroMemory(&ze, sizeof(ze));
    ze.WholeCrc32   = ~crc;
    ze.SkippedCount = g.skipCount;
    ze.SkippedBytes = 0;
    ze.Reserved     = 0;
    if (!NetSendFrame(FRAME_ZIP_END, g.requestId, &ze,
                      (ULONG)sizeof(CLIP_ZIP_END), &err)) {
        Log(LOG_ERR, "zip-end send failed err=%u", err);
        goto fail;
    }
    ok = TRUE;

fail:
    if (!ok) {
        if (hSrc != INVALID_HANDLE_VALUE) {
            CloseHandle(hSrc);
            hSrc = INVALID_HANDLE_VALUE;
        }
        ClipZipAbort(&g.zip);
        Log(LOG_WARN, "sender cleaned, %I64u bytes delivered", g.bytesDone);
        CleanupTransfer(CLIP_R_DISCONNECT, FALSE);
    }
done:
    if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
    if (buf) {
        ZeroMemory(buf, CLIP_CHUNK_SIZE);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    if (ok || g.state == CLIP_IDLE) {
        ClipZipAbort(&g.zip);          // delete temp zip on success too
        CleanupTransfer(CLIP_R_OK, FALSE);
        Log(LOG_OK, "sent %I64u bytes, skipped=%u", sent, g.skipCount);
    }
    return ok;
}

// ============================================================
// Receiver. .part uses the ClipAgent_<pid>_<seq> prefix (part02 c2),
// final name applied only by MoveFileEx after CRC success.
// ============================================================
BOOL ReceiverBegin(const CLIP_ZIP_START* s)
{
    WCHAR target[MAX_PATH];
    WCHAR finalName[MAX_PATH];
    WCHAR safe[MAX_PATH];
    ULONGLONG tag;
    char  report[2048];
    ULONG ch;

    if (!s) return FALSE;
    if (g.state == CLIP_TRANSFER) {
        Log(LOG_WARN, "zip-start while transferring, ignored");
        return FALSE;
    }
    g.requestId   = (s->ZipBytes << 8) ^ (ULONGLONG)GetTickCount64();
    g.bytesTotal  = s->ZipBytes;
    g.bytesDone   = 0;
    g.blockSeq    = 0;
    g.wholeCrc    = 0xFFFFFFFFu;
    g.lastErr     = ERROR_SUCCESS;
    g.textMode    = (s->FileName[0] == L'\0');
    s_lastPct     = 0;
    if (g.hPart) { CloseHandle(g.hPart); g.hPart = NULL; }

    if (g.textMode &&
        g.bytesTotal > (ULONGLONG)CLIP_MAX_TEXT_WCH * sizeof(WCHAR)) {
        Log(LOG_ERR, "text payload too large, rejected");
        EmitCancelAndCleanup(CLIP_R_PATH);
        return FALSE;
    }

    if (!ClipGetForegroundDir(target, MAX_PATH)) {
        StringCchCopyW(target, MAX_PATH, g.outDir);
    }
    if (!ClipEnsureDir(target)) {
        StringCchCopyW(target, MAX_PATH, g.outDir);
        ClipEnsureDir(target);
    }
    if (!ClipCheckDiskSpace(target, g.bytesTotal + 1048576ull)) {
        ClipClassifyAndReport(target, ERROR_DISK_FULL, report, 2048);
        ch = AskUserChoice(report);
        if (ch == CH_CANCEL || ch == CH_TIMEOUT || ch == CH_SKIP) {
            EmitCancelAndCleanup(CLIP_R_DISK_FULL);
            return FALSE;
        }
    }

    tag = (ULONGLONG)GetTickCount64();
    if (!ClipMakeTempName(g.partPath, MAX_PATH, L".part", tag)) {
        // %TEMP% may be on another volume than target; keep it in target
        // so MoveFileEx is atomic on the same volume.
        StringCchPrintfW(g.partPath, MAX_PATH,
            L"%s\\ClipAgent_%lu_%I64u.part", target,
            (ULONG)GetCurrentProcessId(), tag);
    }

    if (!g.textMode) {
        if (!ClipSanitizeName(s->FileName, safe, MAX_PATH, tag)) {
            EmitCancelAndCleanup(CLIP_R_PATH);
            return FALSE;
        }
        StringCchCopyW(finalName, MAX_PATH, safe);
        StringCchPrintfW(g.finalPath, MAX_PATH, L"%s\\%s",
                         target, finalName);
    } else {
        g.finalPath[0] = L'\0';   // text has no final file
    }

    g.hPart = CreateFileW(g.partPath, GENERIC_WRITE | FILE_APPEND_DATA,
                          FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_TEMPORARY |
                          FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (g.hPart == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        ClipClassifyAndReport(g.partPath, e, report, 2048);
        Log(LOG_ERR, "create .part failed err=%u", e);
        ch = AskUserChoice(report);
        if (ch == CH_TERM) TerminateOwner();
        if (ch == CH_CANCEL || ch == CH_TIMEOUT) {
            EmitCancelAndCleanup(CLIP_R_ACCESS);
            return FALSE;
        }
        g.hPart = CreateFileW(g.partPath,
                              GENERIC_WRITE | FILE_APPEND_DATA,
                              FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (g.hPart == INVALID_HANDLE_VALUE) {
            EmitCancelAndCleanup(CLIP_R_ACCESS);
            return FALSE;
        }
    }

    TransferSetState(CLIP_TRANSFER);
    {
        char b[64];
        ClipFormatBytes(g.bytesTotal, b, 64);
        Log(LOG_INFO, "receiving %s -> %s%s", b,
            g.textMode ? "(clipboard text)" : "file",
            g.textMode ? "" : " (part staged)");
    }
    return TRUE;
}

BOOL ReceiverWriteChunk(ULONGLONG seq, const BYTE* p, ULONG n)
{
    DWORD wrote = 0;
    if (!p || !n) return FALSE;
    if (g.state != CLIP_TRANSFER) return FALSE;

    if (!g.hPart && g.partPath[0]) {
        g.hPart = CreateFileW(g.partPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (g.hPart == INVALID_HANDLE_VALUE) {
            g.hPart = NULL;
            return FALSE;
        }
    }
    if (!WriteFile(g.hPart, p, n, &wrote, NULL) || wrote != n)
        return FALSE;

    g.wholeCrc  = ClipCrc32(g.wholeCrc, p, n);
    g.bytesDone += n;
    g.blockSeq   = seq + 1;
    ProgressPost(g.bytesDone, g.bytesTotal);
    return TRUE;
}

// D3 rename retry (contract C2). .part is already complete here.
static BOOL ReceiverRetryRename(VOID)
{
    char report[2048];
    DWORD e;
    if (!g.partPath[0] || !g.finalPath[0]) return FALSE;
    if (MoveFileExW(g.partPath, g.finalPath,
                    MOVEFILE_REPLACE_EXISTING)) {
        Log(LOG_OK, "rename succeeded after resume");
        CleanupTransfer(CLIP_R_OK, FALSE);
        return TRUE;
    }
    e = GetLastError();
    ClipClassifyAndReport(g.finalPath, e, report, 2048);
    Log(LOG_ERR, "rename failed again err=%u, transfer dropped", e);
    EmitCancelAndCleanup(CLIP_R_ACCESS);
    return FALSE;
}

BOOL ReceiverFinish(const CLIP_ZIP_END* e)
{
    char  comment[CLIPZIP_MAX_COMMENT];
    char  report[2048];
    ULONG ch;
    DWORD err;

    if (!e) return FALSE;
    if (g.hPart) { CloseHandle(g.hPart); g.hPart = NULL; }

    if (g.bytesDone != g.bytesTotal || (~g.wholeCrc) != e->WholeCrc32) {
        Log(LOG_ERR, "crc/size mismatch: got=%I64u want=%I64u",
            g.bytesDone, g.bytesTotal);
        EmitCancelAndCleanup(CLIP_R_PEER_CANCEL);
        return FALSE;
    }

    if (g.textMode) {
        HANDLE h = CreateFileW(g.partPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        BYTE*  tb = NULL;
        DWORD  got = 0, n = (DWORD)g.bytesTotal;
        if (h != INVALID_HANDLE_VALUE) {
            tb = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  (SIZE_T)n + 2);
            if (tb) {
                ReadFile(h, tb, n, &got, NULL);
                CloseHandle(h);
                ((WCHAR*)tb)[got / 2] = L'\0';
                InterlockedExchange(&g_suppressClip, 2);
                ClipSetUnicodeText((const WCHAR*)tb);
                ZeroMemory(tb, n + 2);
                HeapFree(GetProcessHeap(), 0, tb);
                Log(LOG_OK, "text pasted to local clipboard (%u bytes)", got);
            } else {
                CloseHandle(h);
            }
        }
        CleanupTransfer(CLIP_R_OK, FALSE);   // deletes the .part
        return TRUE;
    }

    while (!MoveFileExW(g.partPath, g.finalPath,
                        MOVEFILE_REPLACE_EXISTING)) {
        err = GetLastError();
        ClipClassifyAndReport(g.finalPath, err, report, 2048);
        ch = AskUserChoice(report);
        if (ch == CH_CANCEL) {
            EmitCancelAndCleanup(CLIP_R_USER_CANCEL);
            return FALSE;
        }
        if (ch == CH_TERM) { TerminateOwner(); continue; }
        if (ch == CH_TIMEOUT || ch == CH_SKIP) {
            // D3: .part is COMPLETE, so a real pause with breakpoint is safe.
            CLIP_PAUSE p;
            ZeroMemory(&p, sizeof(p));
            p.Reason     = (ULONG)CLIP_R_PAUSE_KEEP;
            p.ErrClass   = ClipClassifyError(err);
            p.Win32Error = err;
            p.RequestId  = g.requestId;
            StringCchCopyW(p.Subject, CLIP_NAME_W, g.finalPath);
            PauseTransfer(&p);
            return FALSE;
        }
        // CH_RESUME -> loop and retry the rename once more
        break;
    }

    if (g.partPath[0] && GetFileAttributesW(g.partPath) != INVALID_FILE_ATTRIBUTES) {
        // last rename attempt failed after CH_RESUME: pause with breakpoint
        err = GetLastError();
        {
            CLIP_PAUSE p;
            ZeroMemory(&p, sizeof(p));
            p.Reason     = (ULONG)CLIP_R_PAUSE_KEEP;
            p.ErrClass   = CLIPERR_ACCESS;
            p.Win32Error = err;
            p.RequestId  = g.requestId;
            StringCchCopyW(p.Subject, CLIP_NAME_W, g.finalPath);
            PauseTransfer(&p);
        }
        return FALSE;
    }

    comment[0] = '\0';
    if (ClipZipReadComment(g.finalPath, comment, CLIPZIP_MAX_COMMENT))
        ClipShowSkipList(comment);

    {
        char b[64];
        ClipFormatBytes(g.bytesTotal, b, 64);
        Log(LOG_OK, "saved %s (%s)$", "file", b);
    }
    CleanupTransfer(CLIP_R_OK, FALSE);
    return TRUE;
}

// ============================================================
// Frame dispatch
// ============================================================
BOOL HandleFrame(const CLIP_FRAME_HEADER* h, const BYTE* payload)
{
    if (!h) return FALSE;
    switch (h->Type) {
    case FRAME_HELLO:
        Log(LOG_OK, "peer hello accepted (role/version ok)");
        return TRUE;

    case FRAME_LIST:
        if (h->Length < sizeof(CLIP_LIST)) return FALSE;
        CopyMemory(&g.pendingList, payload, sizeof(CLIP_LIST));
        if (g.state == CLIP_IDLE) TransferSetState(CLIP_HAVE_LIST);
        {
            char b[64];
            ClipFormatBytes(g.pendingList.TotalBytes, b, 64);
            Log(LOG_INFO, "remote content ready: %s, files=%u dirs=%u",
                b, g.pendingList.FileCount, g.pendingList.DirCount);
        }
        return TRUE;

    case FRAME_PULL:
        if (h->Length < sizeof(CLIP_PULL)) return FALSE;
        return HandlePull((const CLIP_PULL*)payload);

    case FRAME_ZIP_START:
        if (h->Length < sizeof(CLIP_ZIP_START)) return FALSE;
        return ReceiverBegin((const CLIP_ZIP_START*)payload);

    case FRAME_DATA:
        return ReceiverWriteChunk(h->Seq, payload, h->Length);

    case FRAME_ZIP_END:
        if (h->Length < sizeof(CLIP_ZIP_END)) return FALSE;
        return ReceiverFinish((const CLIP_ZIP_END*)payload);

    case FRAME_CANCEL: {
        CLIP_CANCEL ack;
        const CLIP_CANCEL* c = (const CLIP_CANCEL*)payload;
        ULONG r = (h->Length >= sizeof(CLIP_CANCEL) && c)
                  ? c->Reason : (ULONG)CLIP_R_PEER_CANCEL;
        ZeroMemory(&ack, sizeof(ack));
        ack.Reason    = r;
        ack.RequestId = g.requestId;
        NetSendFrame(FRAME_CANCEL_ACK, g.requestId, &ack,
                     (ULONG)sizeof(CLIP_CANCEL), &g.lastErr);
        CleanupTransfer(CLIP_R_PEER_CANCEL, FALSE);
        Log(LOG_WARN, "peer canceled (reason=%u)", r);
        return TRUE;
    }

    case FRAME_CANCEL_ACK:
        Log(LOG_INFO, "cancel ack received");
        return TRUE;

    case FRAME_PAUSE: {
        // Passive side (part02 contract 3): never call PauseTransfer,
        // or the frame would echo back and forth.
        const CLIP_PAUSE* p = (const CLIP_PAUSE*)payload;
        if (h->Length >= sizeof(CLIP_PAUSE) && p)
            CopyMemory(&g.pauseInfo, p, sizeof(CLIP_PAUSE));
        CleanupTransfer(CLIP_R_PAUSE_KEEP, TRUE);
        TransferSetState(CLIP_PAUSED);
        Log(LOG_WARN, "peer paused transfer, breakpoint kept");
        return TRUE;
    }

    case FRAME_RESUME: {
        const CLIP_RESUME* r = (const CLIP_RESUME*)payload;
        ULONG act = (h->Length >= sizeof(CLIP_RESUME) && r)
                    ? r->Action : 0u;
        InterlockedExchange(&g.abort, 0);
        InterlockedExchange(&g.zip.Abort, 0);
        TransferSetState(CLIP_TRANSFER);
        if (g.zipTemp[0]) {
            // this end is the sender: resume the stream at bytesDone
            BuildAndSendPayload(g.requestId, TRUE);
        } else if (g.partPath[0] && g.finalPath[0] &&
                   g.bytesDone == g.bytesTotal) {
            // C2: receiver has a complete .part awaiting rename
            ReceiverRetryRename();
        } else {
            // receiver waiting for more FRAME_DATA; reopen on next chunk
            if (g.hPart) { CloseHandle(g.hPart); g.hPart = NULL; }
        }
        Log(LOG_OK, "peer resumed (action=%u)", act);
        return TRUE;
    }

    case FRAME_SKIP_LIST:
        Log(LOG_WARN, "peer reported skipped entries (see zip comment)");
        return TRUE;

    default:
        Log(LOG_WARN, "unknown frame type 0x%04X ignored", h->Type);
        return TRUE;
    }
}

BOOL HandlePull(const CLIP_PULL* p)
{
    ULONGLONG total = 0, largest = 0;
    ULONG     files = 0, dirs = 0;
    char      report[2048];
    char      b1[64], b2[64];
    ULONG     ch;

    if (!p) return FALSE;
    if (g.state == CLIP_TRANSFER) {
        Log(LOG_WARN, "pull ignored: transfer already running");
        return FALSE;
    }
    if (!g.pendingCount && !g.pendingText) {
        Log(LOG_WARN, "pull ignored: nothing on local clipboard");
        return FALSE;
    }

    if (g.pendingText && !g.pendingCount)
        total = g.pendingTextBytes;
    else
        ClipCalcTotal(g.pendingPaths, g.pendingCount,
                      &total, &largest, &files, &dirs);

    if (total > (ULONGLONG)g.maxFileMb * 1048576ull) {
        ClipFormatBytes(total, b1, 64);
        StringCchPrintfA(b2, 64, "%u MB", g.maxFileMb);
        StringCchPrintfA(report, 2048,
            "content is %s, configured limit is %s\r\n"
            "files=%u dirs=%u\r\n"
            "continue anyway? (choose CONTINUE or CANCEL)\r\n",
            b1, b2, files, dirs);
        ch = AskUserChoice(report);
        if (ch != CH_RESUME) {
            Log(LOG_WARN, "large content declined by user (%s)", b1);
            EmitCancelAndCleanup(CLIP_R_USER_CANCEL);
            return FALSE;
        }
    } else if (total > (ULONGLONG)g.confirmMb * 1048576ull) {
        ClipFormatBytes(total, b1, 64);
        Log(LOG_WARN, "large paste %s (above %u MB warn level)",
            b1, g.confirmMb);
    }

    g.requestId = p->RequestId ? p->RequestId
                               : (ULONGLONG)NextRequestId();
    return BuildAndSendPayload(g.requestId, (p->Flags & 1u) ? TRUE : FALSE);
}

// ============================================================
// Network thread
// ============================================================
static BOOL SetSockTimeouts(SOCKET s)
{
    DWORD t = CLIP_SO_TIMEOUT_MS;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&t, sizeof(t)) == SOCKET_ERROR) return FALSE;
    if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                   (const char*)&t, sizeof(t)) == SOCKET_ERROR) return FALSE;
    return TRUE;
}

static BOOL SendHello(SOCKET s)
{
    CLIP_HELLO h;
    DWORD sz = CLIP_NAME_W;
    ZeroMemory(&h, sizeof(h));
    h.Pid      = (ULONG)GetCurrentProcessId();
    h.Role     = g.isHost ? 1 : 2;
    h.Reserved = 0;
    if (!GetComputerNameW(h.Machine, &sz))
        StringCchCopyW(h.Machine, CLIP_NAME_W, L"unknown");
    return NetSendFrame(FRAME_HELLO, 1, &h,
                        (ULONG)sizeof(CLIP_HELLO), &g.lastErr);
}

static BOOL ConnectAsGuest(VOID)
{
    SOCKET s;
    SOCKADDR_IN a;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return FALSE;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(g.port);
    a.sin_addr.S_un.S_addr = inet_addr(g.peerHost);
    if (a.sin_addr.S_un.S_addr == INADDR_NONE) {
        Log(LOG_ERR, "bad IPv4 address: %s (use dotted literal)",
            g.peerHost);
        closesocket(s);
        return FALSE;
    }
    if (connect(s, (SOCKADDR*)&a, sizeof(a)) == SOCKET_ERROR) {
        Log(LOG_ERR, "connect to %s:%u failed wsa=%u",
            g.peerHost, (ULONG)g.port, (ULONG)WSAGetLastError());
        closesocket(s);
        return FALSE;
    }
    if (!SetSockTimeouts(s)) { closesocket(s); return FALSE; }
    g.sock = s;
    Log(LOG_OK, "connected to %s:%u", g.peerHost, (ULONG)g.port);
    SendHello(s);
    return TRUE;
}

static BOOL ListenAsHost(VOID)
{
    SOCKADDR_IN a;
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == INVALID_SOCKET) return FALSE;
    {
        int one = 1;
        setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&one, sizeof(one));
    }
    ZeroMemory(&a, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(g.port);
    a.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(s_listen, (SOCKADDR*)&a, sizeof(a)) == SOCKET_ERROR) {
        Log(LOG_ERR, "bind port %u failed wsa=%u",
            (ULONG)g.port, (ULONG)WSAGetLastError());
        closesocket(s_listen);
        s_listen = INVALID_SOCKET;
        return FALSE;
    }
    if (listen(s_listen, 1) == SOCKET_ERROR) {
        closesocket(s_listen);
        s_listen = INVALID_SOCKET;
        return FALSE;
    }
    Log(LOG_INFO, "listening on port %u", (ULONG)g.port);
    return TRUE;
}

static VOID TickKicks(VOID)
{
    if (InterlockedCompareExchange(&g_clipPullKick, 0, 0)) {
        InterlockedExchange(&g_clipPullKick, 0);
        if (g.sock == INVALID_SOCKET || g.sock == 0) {
            Log(LOG_WARN, "paste ignored: not connected");
        } else if (g.state == CLIP_TRANSFER) {
            Log(LOG_WARN, "paste ignored: transfer in progress");
        } else {
            CLIP_PULL p;
            ZeroMemory(&p, sizeof(p));
            p.RequestId = (ULONGLONG)NextRequestId();
            p.MaxFileMb = g.maxFileMb;
            p.Flags     = 0;
            if (NetSendFrame(FRAME_PULL, p.RequestId, &p,
                             (ULONG)sizeof(CLIP_PULL), &g.lastErr))
                Log(LOG_INFO, "Ctrl+Alt+V -> FRAME_PULL sent");
            else
                Log(LOG_ERR, "pull send failed err=%u", g.lastErr);
        }
    }

    if (InterlockedCompareExchange(&g_clipResumeKick, 0, 0)) {
        InterlockedExchange(&g_clipResumeKick, 0);
        if (g.zipTemp[0]) {
            // this end is the sender
            BuildAndSendPayload(g.requestId, TRUE);
        } else if (g.partPath[0] && g.finalPath[0] &&
                   g.bytesDone == g.bytesTotal) {
            // C2: complete .part awaiting rename
            ReceiverRetryRename();
        } else {
            Log(LOG_INFO, "resume: waiting for peer FRAME_DATA");
        }
    }

    if (TransferIsAborted() && g.state == CLIP_TRANSFER) {
        EmitCancelAndCleanup(CLIP_R_USER_CANCEL);
    }
}

DWORD WINAPI NetThread(LPVOID p)
{
    BYTE* payload = NULL;
    (void)p;

    payload = (BYTE*)HeapAlloc(GetProcessHeap(), 0,
                               CLIP_MAX_FRAME_PAYLOAD);
    if (!payload) {
        Log(LOG_ERR, "net thread alloc failed");
        return 1;
    }

    if (g.isHost) {
        if (!ListenAsHost()) goto out;
    } else {
        if (!ConnectAsGuest()) goto out;
    }

    while (InterlockedCompareExchange(&g.netRunning, 0, 0)) {
        fd_set rd;
        struct timeval tv;
        CLIP_FRAME_HEADER h;
        ULONG  len = 0;
        DWORD  err = ERROR_SUCCESS;
        int    r;

        TickKicks();
        if (g.sock == INVALID_SOCKET || g.sock == 0) {
            // host waiting for a guest
            if (g.isHost && s_listen != INVALID_SOCKET) {
                FD_ZERO(&rd);
                FD_SET(s_listen, &rd);
                tv.tv_sec = 0; tv.tv_usec = 200000;
                r = select(0, &rd, NULL, NULL, &tv);
                if (r > 0) {
                    SOCKET a = accept(s_listen, NULL, NULL);
                    if (a != INVALID_SOCKET) {
                        SetSockTimeouts(a);
                        g.sock = a;
                        Log(LOG_OK, "guest connected");
                        SendHello(a);
                    }
                }
            } else if (!g.isHost) {
                // guest lost the link: try a clean reconnect
                Sleep(200);
                if (!ConnectAsGuest()) Sleep(1000);
            }
            continue;
        }

        AdvertScan();
        FD_ZERO(&rd);
        FD_SET(g.sock, &rd);
        tv.tv_sec = 0; tv.tv_usec = 200000;
        r = select(0, &rd, NULL, NULL, &tv);
        if (r == 0) continue;
        if (r == SOCKET_ERROR) {
            HandleDisconnect((DWORD)WSAGetLastError());
            break;
        }

        ZeroMemory(payload, CLIP_MAX_FRAME_PAYLOAD);
        r = NetRecvFrame(&h, payload, CLIP_MAX_FRAME_PAYLOAD,
                         &len, &err);
        if (r == -1) {
            HandleDisconnect(CLIP_NET_CLOSED);
            break;
        }
        if (r == 0) {
            HandleDisconnect(err);
            break;
        }
        if (!HandleFrame(&h, payload)) {
            Log(LOG_WARN, "frame handler failed type=0x%04X", h.Type);
        }
        ZeroMemory(payload, len);
    }

out:
    if (s_listen != INVALID_SOCKET) {
        closesocket(s_listen);
        s_listen = INVALID_SOCKET;
    }
    if (g.sock != INVALID_SOCKET && g.sock != 0) {
        shutdown(g.sock, SD_BOTH);
        closesocket(g.sock);
        g.sock = INVALID_SOCKET;
    }
    if (payload) {
        ZeroMemory(payload, CLIP_MAX_FRAME_PAYLOAD);
        HeapFree(GetProcessHeap(), 0, payload);
    }
    Log(LOG_INFO, "net thread stopped");
    return 0;
}

// ============================================================
// Args + entry
// ============================================================
static VOID PrintUsage(VOID)
{
    printf(
      "ClipAgent (pull model, Win7/Win11 x64)\n"
      "  HOST : ClipAgent.exe --listen 8080 --key mon --outdir C:\\ClipInbox\n"
      "  GUEST: ClipAgent.exe --connect 192.168.1.50:8080 --key mon"
      " --outdir C:\\ClipInbox\n"
      "\n"
      "  --key <s>           shared LAN label (default mon)\n"
      "  --outdir <dir>      fallback landing dir\n"
      "  --max-file-mb <n>   ask before sending above n MB (default 256)\n"
      "  --confirm-mb <n>    warn level (default 50)\n"
      "  --quiet             hide console, tray only\n"
      "  --help              this text\n"
      "\n"
      "  Ctrl+Alt+V  paste remote content (PULL)\n"
      "  Ctrl+Alt+C  cancel current transfer\n"
      "  Ctrl+Alt+S  show/hide log window\n"
    );
}

BOOL ParseArgs(int argc, WCHAR** argv)
{
    int i;
    BOOL haveMode = FALSE;

    StringCchCopyA(g.key, CLIP_KEY_MAX, "mon");
    StringCchCopyW(g.outDir, MAX_PATH, L"C:\\ClipInbox");
    g.port       = CLIP_DEFAULT_PORT;
    g.maxFileMb  = CLIP_MAX_FILE_DEFAULT_MB;
    g.confirmMb  = CLIP_CONFIRM_DEFAULT_MB;
    g.quiet      = FALSE;
    g.isHost     = FALSE;
    g.peerHost[0] = '\0';

    for (i = 1; i < argc; i++) {
        WCHAR* a = argv[i];
        char*  v;
        if (!_wcsicmp(a, L"--listen")) {
            if (i + 1 >= argc) return FALSE;
            g.port = (USHORT)_wtoi(argv[++i]);
            g.isHost = TRUE; haveMode = TRUE;
        } else if (!_wcsicmp(a, L"--connect")) {
            if (i + 1 >= argc) return FALSE;
            {
                WCHAR* s = argv[++i];
                WCHAR* c = wcsrchr(s, L':');
                char*  hp;
                if (!c) return FALSE;
                *c = L'\0';
                hp = W2A(s);
                if (!hp) return FALSE;
                StringCchCopyA(g.peerHost, 128, hp);
                FreeBuf(hp, (ULONG)(lstrlenA(hp) + 1));
                g.port = (USHORT)_wtoi(c + 1);
                *c = L':';
            }
            g.isHost = FALSE; haveMode = TRUE;
        } else if (!_wcsicmp(a, L"--key")) {
            char* hp;
            if (i + 1 >= argc) return FALSE;
            hp = W2A(argv[++i]);
            if (!hp) return FALSE;
            StringCchCopyA(g.key, CLIP_KEY_MAX, hp);
            FreeBuf(hp, (ULONG)(lstrlenA(hp) + 1));
        } else if (!_wcsicmp(a, L"--outdir")) {
            if (i + 1 >= argc) return FALSE;
            StringCchCopyW(g.outDir, MAX_PATH, argv[++i]);
        } else if (!_wcsicmp(a, L"--max-file-mb")) {
            if (i + 1 >= argc) return FALSE;
            g.maxFileMb = (ULONG)_wtoi(argv[++i]);
            if (!g.maxFileMb) g.maxFileMb = CLIP_MAX_FILE_DEFAULT_MB;
        } else if (!_wcsicmp(a, L"--confirm-mb")) {
            if (i + 1 >= argc) return FALSE;
            g.confirmMb = (ULONG)_wtoi(argv[++i]);
        } else if (!_wcsicmp(a, L"--quiet")) {
            g.quiet = TRUE;
        } else if (!_wcsicmp(a, L"--help")) {
            return FALSE;
        } else {
            v = W2A(a);
            printf("[!] unknown option: %s\n", v ? v : "?");
            if (v) FreeBuf(v, (ULONG)(lstrlenA(v) + 1));
            return FALSE;
        }
    }
    if (!haveMode || !g.port) return FALSE;
    g.keyTag = ClipKeyTag(g.key);
    return TRUE;
}

int wmain(int argc, WCHAR** argv)
{
    WSADATA wsa;
    MSG     msg;
    int     i;

    // part02 contract 1: globals are zero-initialised, 0 is NOT a socket.
    g.sock = INVALID_SOCKET;

    for (i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--help")) {
            PrintUsage();
            return 0;
        }
    }
    if (!ParseArgs(argc, argv)) {
        PrintUsage();
        return 1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[X] WSAStartup failed\n");
        return 1;
    }

    ClipEnsureDir(g.outDir);
    if (!GetTempPathW(MAX_PATH, g.tempDir))
        StringCchCopyW(g.tempDir, MAX_PATH, L"C:\\Windows\\Temp");

    if (!UiInit(GetModuleHandleW(NULL))) {
        printf("[X] UI init failed\n");
        WSACleanup();
        return 1;
    }
    if (g.quiet) UiShowLog(FALSE);
    SweepStaleFiles(TRUE);
    Log(LOG_OK, "ClipAgent %s mode, key=%s, outdir=%S",
        g.isHost ? "HOST" : "GUEST", g.key, g.outDir);

    InterlockedExchange(&g.netRunning, 1);
    g.hNetThread = CreateThread(NULL, 0, NetThread, NULL, 0, NULL);
    if (!g.hNetThread) {
        Log(LOG_ERR, "CreateThread(net) failed");
        InterlockedExchange(&g.netRunning, 0);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // exit order (part03 contract 2): net first, then UI shutdown.
    InterlockedExchange(&g.netRunning, 0);
    if (g.hNetThread) {
        WaitForSingleObject(g.hNetThread, 3000);
        CloseHandle(g.hNetThread);
        g.hNetThread = NULL;
    }
    if (g.state == CLIP_TRANSFER || g.state == CLIP_PAUSED)
        CleanupTransfer(CLIP_R_USER_CANCEL, FALSE);
    SweepStaleFiles(TRUE);     // process-exit sweep (part02 5th guard)
    UiShutdown();              // deletes logLock; no Log() after this
    WSACleanup();
    return 0;
}