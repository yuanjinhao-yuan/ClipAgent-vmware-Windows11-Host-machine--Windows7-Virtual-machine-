// miniz.c -- ClipAgent built-in minimal ZIP writer, store method only.
// Project-local module, NOT upstream richgel999/miniz.
// Constant 64 KiB buffers. Every exit path is idempotent.
// ASCII only. // comments only.
#include "miniz.h"
#include <stdlib.h>

#pragma pack(push, 1)
typedef struct _ZIP_LOCAL {
    ULONG   Sig;           // 0x04034B50
    USHORT  VerNeed;
    USHORT  Flag;
    USHORT  Method;
    USHORT  ModTime;
    USHORT  ModDate;
    ULONG   Crc32;
    ULONG   CompSize;
    ULONG   UncompSize;
    USHORT  NameLen;
    USHORT  ExtraLen;
} ZIP_LOCAL;               // 30 bytes

typedef struct _ZIP_CENTRAL {
    ULONG   Sig;           // 0x02014B50
    USHORT  VerMade;
    USHORT  VerNeed;
    USHORT  Flag;
    USHORT  Method;
    USHORT  ModTime;
    USHORT  ModDate;
    ULONG   Crc32;
    ULONG   CompSize;
    ULONG   UncompSize;
    USHORT  NameLen;
    USHORT  ExtraLen;
    USHORT  CommentLen;
    USHORT  DiskStart;
    USHORT  IntAttr;
    ULONG   ExtAttr;
    ULONG   LocalOff;
} ZIP_CENTRAL;             // 46 bytes

typedef struct _ZIP_EOCD {
    ULONG   Sig;           // 0x06054B50
    USHORT  Disk;
    USHORT  CdDisk;
    USHORT  EntriesDisk;
    USHORT  EntriesTotal;
    ULONG   CdSize;
    ULONG   CdOffset;
    USHORT  CommentLen;
} ZIP_EOCD;                // 22 bytes
#pragma pack(pop)

// 1980-01-01 00:00, the lowest legal DOS timestamp. Every decoder accepts it.
#define ZIP_DOS_DATE   0x0021
#define ZIP_DOS_TIME   0x0000
#define ZIP_BUF        CLIP_CHUNK_SIZE

static ULONG  g_CrcTable[256];
static volatile LONG g_CrcOnce = 0;

static void CrcInitOnce(void)
{
    ULONG i; int k;
    if (InterlockedCompareExchange(&g_CrcOnce, 1, 0) == 0) {
        for (i = 0; i < 256; i++) {
            ULONG c = i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_CrcTable[i] = c;
        }
        InterlockedExchange(&g_CrcOnce, 2);
    } else {
        while (InterlockedCompareExchange(&g_CrcOnce, 2, 2) != 2) Sleep(0);
    }
}

// Pure chained CRC. Caller seeds 0xFFFFFFFF and inverts the final value.
// Chaining across 64 KiB chunks is exact; no whole-file buffer exists.
ULONG ClipCrc32(ULONG seed, const void* buf, ULONG len)
{
    const BYTE* p = (const BYTE*)buf;
    ULONG c = seed, i;
    CrcInitOnce();
    for (i = 0; i < len; i++)
        c = g_CrcTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

static BOOL ZipAborted(PCLIPZIP z)
{
    return InterlockedCompareExchange(&z->Abort, 0, 0) != 0;
}

static BOOL WriteAll(HANDLE h, const void* p, ULONG n)
{
    const BYTE* q = (const BYTE*)p;
    ULONG left = n;
    while (left) {
        DWORD wrote = 0;
        if (!WriteFile(h, q, left, &wrote, NULL) || wrote == 0) return FALSE;
        q += wrote;
        left -= wrote;
    }
    return TRUE;
}

static BOOL SeekTo(HANDLE h, ULONGLONG off)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) != FALSE;
}

static BOOL IsHandle(HANDLE h)
{
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

BOOL ClipZipCreate(PCLIPZIP z, const WCHAR* zipPath, const WCHAR* cdPath)
{
    if (!z || !zipPath || !cdPath) return FALSE;
    ZeroMemory(z, sizeof(CLIPZIP));
    z->hZip = INVALID_HANDLE_VALUE;
    z->hCd  = INVALID_HANDLE_VALUE;
    lstrcpynW(z->ZipPath, zipPath, CLIP_NAME_W);
    lstrcpynW(z->CdPath,  cdPath,  CLIP_NAME_W);

    z->hZip = CreateFileW(zipPath, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (!IsHandle(z->hZip)) {
        if (z->hZip == INVALID_HANDLE_VALUE) z->hZip = NULL;
        ClipZipAbort(z);
        return FALSE;
    }
    z->hCd = CreateFileW(cdPath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (!IsHandle(z->hCd)) {
        if (z->hCd == INVALID_HANDLE_VALUE) z->hCd = NULL;
        ClipZipAbort(z);
        return FALSE;
    }
    z->ZipOffset   = 0;
    z->CdSize      = 0;
    z->EntryCount  = 0;
    z->CommentLen  = 0;
    return TRUE;
}

BOOL ClipZipAddFile(PCLIPZIP z, const WCHAR* srcPath,
                    const char* entryUtf8, PDWORD pdwError)
{
    HANDLE hSrc = INVALID_HANDLE_VALUE;
    BYTE*  buf = NULL;
    DWORD  err = ERROR_SUCCESS;
    ULONG  nameLen = 0;
    ULONGLONG size = 0, written = 0, localOff = 0;
    ULONG  crcSeed = 0xFFFFFFFFu, crcFinal = 0;
    BOOL   ok = FALSE, headerWritten = FALSE;
    ZIP_LOCAL   lh;
    ZIP_CENTRAL ch;
    DWORD  got = 0;

    if (pdwError) *pdwError = ERROR_SUCCESS;
    if (!z || !srcPath || !entryUtf8 || !IsHandle(z->hZip) || !IsHandle(z->hCd)) {
        if (pdwError) *pdwError = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (ZipAborted(z)) { if (pdwError) *pdwError = ERROR_OPERATION_ABORTED; return FALSE; }

    nameLen = (ULONG)lstrlenA(entryUtf8);
    if (nameLen == 0 || nameLen >= CLIPZIP_MAX_ENTRY_UTF8 || nameLen >= 0xFFFF) {
        if (pdwError) *pdwError = ERROR_BAD_PATHNAME;
        return FALSE;
    }

    hSrc = CreateFileW(srcPath, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (!IsHandle(hSrc)) {
        err = GetLastError();
        goto done;
    }

    buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, ZIP_BUF);
    if (!buf) { err = ERROR_NOT_ENOUGH_MEMORY; goto done; }

    // Pass 1: size + CRC only. Nothing is accumulated in memory.
    for (;;) {
        if (ZipAborted(z)) { err = ERROR_OPERATION_ABORTED; goto done; }
        got = 0;
        if (!ReadFile(hSrc, buf, ZIP_BUF, &got, NULL)) { err = GetLastError(); goto done; }
        if (got == 0) break;
        size += got;
        crcSeed = ClipCrc32(crcSeed, buf, got);
        if (size > 0xFFFFFFFFull) { err = ERROR_FILE_TOO_LARGE; goto done; }
    }
    crcFinal = ~crcSeed;

    if (!SeekTo(hSrc, 0)) { err = GetLastError(); goto done; }

    localOff = z->ZipOffset;
    if (localOff > 0xFFFFFFFFull) { err = ERROR_FILE_TOO_LARGE; goto done; }

    ZeroMemory(&lh, sizeof(lh));
    lh.Sig        = CLIPZIP_LOCAL_SIG;
    lh.VerNeed    = CLIPZIP_VERSION;
    lh.Flag       = (USHORT)CLIPZIP_FLAG_UTF8;
    lh.Method     = CLIPZIP_METHOD_STORE;
    lh.ModTime    = ZIP_DOS_TIME;
    lh.ModDate    = ZIP_DOS_DATE;
    lh.Crc32      = crcFinal;
    lh.CompSize   = (ULONG)size;
    lh.UncompSize = (ULONG)size;
    lh.NameLen    = (USHORT)nameLen;
    lh.ExtraLen   = 0;

    if (!WriteAll(z->hZip, &lh, sizeof(lh)) ||
        !WriteAll(z->hZip, entryUtf8, nameLen)) {
        err = GetLastError();
        goto done;
    }
    headerWritten = TRUE;

    // Pass 2: stream raw bytes. Written count must match pass 1 exactly.
    for (;;) {
        if (ZipAborted(z)) { err = ERROR_OPERATION_ABORTED; goto done; }
        got = 0;
        if (!ReadFile(hSrc, buf, ZIP_BUF, &got, NULL)) { err = GetLastError(); goto done; }
        if (got == 0) break;
        if (!WriteAll(z->hZip, buf, got)) { err = GetLastError(); goto done; }
        written += got;
    }
    if (written != size) { err = ERROR_WRITE_FAULT; goto done; }

    ZeroMemory(&ch, sizeof(ch));
    ch.Sig        = CLIPZIP_CENTRAL_SIG;
    ch.VerMade    = CLIPZIP_VERSION;
    ch.VerNeed    = CLIPZIP_VERSION;
    ch.Flag       = (USHORT)CLIPZIP_FLAG_UTF8;
    ch.Method     = CLIPZIP_METHOD_STORE;
    ch.ModTime    = ZIP_DOS_TIME;
    ch.ModDate    = ZIP_DOS_DATE;
    ch.Crc32      = crcFinal;
    ch.CompSize   = (ULONG)size;
    ch.UncompSize = (ULONG)size;
    ch.NameLen    = (USHORT)nameLen;
    ch.ExtraLen   = 0;
    ch.CommentLen = 0;
    ch.DiskStart  = 0;
    ch.IntAttr    = 0;
    ch.ExtAttr    = 0;
    ch.LocalOff   = (ULONG)localOff;

    if (!WriteAll(z->hCd, &ch, sizeof(ch)) ||
        !WriteAll(z->hCd, entryUtf8, nameLen)) {
        err = GetLastError();
        goto done;
    }

    z->ZipOffset  = localOff + sizeof(lh) + nameLen + size;
    z->CdSize    += sizeof(ch) + nameLen;
    z->EntryCount += 1;
    ok = TRUE;

done:
    // Roll back any orphan local header so the archive never carries
    // a half-written entry that the central directory does not list.
    if (!ok && headerWritten && IsHandle(z->hZip)) {
        SeekTo(z->hZip, localOff);
        SetEndOfFile(z->hZip);
        z->ZipOffset = localOff;
    }
    if (buf) {
        ZeroMemory(buf, ZIP_BUF);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    if (IsHandle(hSrc)) CloseHandle(hSrc);
    if (!ok && pdwError) *pdwError = err ? err : ERROR_GEN_FAILURE;
    return ok;
}

BOOL ClipZipSetComment(PCLIPZIP z, const char* comment)
{
    size_t n;
    if (!z) return FALSE;
    z->CommentLen = 0;
    z->Comment[0] = '\0';
    if (!comment || !comment[0]) return TRUE;
    n = lstrlenA(comment);
    if (n > CLIPZIP_MAX_COMMENT - 1) n = CLIPZIP_MAX_COMMENT - 1;
    if (n > 0xFFFFu) n = 0xFFFFu;
    CopyMemory(z->Comment, comment, n);
    z->Comment[n] = '\0';
    z->CommentLen = (ULONG)n;
    return TRUE;
}

BOOL ClipZipFinalize(PCLIPZIP z, PULONGLONG pFinalSize)
{
    BYTE*      buf = NULL;
    ZIP_EOCD   eocd;
    ULONGLONG  cdOffset = 0;
    DWORD      got = 0;
    LARGE_INTEGER fsz;
    BOOL       ok = FALSE;

    if (pFinalSize) *pFinalSize = 0;
    if (!z || !IsHandle(z->hZip) || !IsHandle(z->hCd)) return FALSE;
    if (ZipAborted(z)) return FALSE;
    if (z->ZipOffset + z->CdSize + sizeof(ZIP_EOCD) + z->CommentLen
        > CLIPZIP_MAX_ARCHIVE) return FALSE;

    buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, ZIP_BUF);
    if (!buf) return FALSE;

    cdOffset = z->ZipOffset;
    if (!SeekTo(z->hCd, 0)) goto done;
    for (;;) {
        if (ZipAborted(z)) goto done;
        got = 0;
        if (!ReadFile(z->hCd, buf, ZIP_BUF, &got, NULL)) goto done;
        if (got == 0) break;
        if (!WriteAll(z->hZip, buf, got)) goto done;
        z->ZipOffset += got;
    }

    ZeroMemory(&eocd, sizeof(eocd));
    eocd.Sig          = CLIPZIP_EOCD_SIG;
    eocd.Disk         = 0;
    eocd.CdDisk       = 0;
    eocd.EntriesDisk  = (USHORT)(z->EntryCount & 0xFFFFu);
    eocd.EntriesTotal = (USHORT)(z->EntryCount & 0xFFFFu);
    eocd.CdSize       = (ULONG)z->CdSize;
    eocd.CdOffset     = (ULONG)cdOffset;
    eocd.CommentLen   = (USHORT)z->CommentLen;

    if (!WriteAll(z->hZip, &eocd, sizeof(eocd))) goto done;
    if (z->CommentLen && !WriteAll(z->hZip, z->Comment, z->CommentLen)) goto done;

    if (!GetFileSizeEx(z->hZip, &fsz)) goto done;
    if (pFinalSize) *pFinalSize = (ULONGLONG)fsz.QuadPart;
    ok = TRUE;

done:
    if (buf) {
        ZeroMemory(buf, ZIP_BUF);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    // Close first, then delete. ZipPath is KEPT so ClipZipAbort can remove
    // the temp archive after the sender finishes streaming it.
    if (IsHandle(z->hZip)) { CloseHandle(z->hZip); z->hZip = INVALID_HANDLE_VALUE; }
    if (IsHandle(z->hCd))  { CloseHandle(z->hCd);  z->hCd  = INVALID_HANDLE_VALUE; }
    if (z->CdPath[0]) { DeleteFileW(z->CdPath); z->CdPath[0] = L'\0'; }
    if (!ok) {
        ClipZipAbort(z);
    }
    return ok;
}

VOID ClipZipAbort(PCLIPZIP z)
{
    if (!z) return;
    if (IsHandle(z->hZip)) CloseHandle(z->hZip);
    if (IsHandle(z->hCd))  CloseHandle(z->hCd);
    if (z->ZipPath[0]) DeleteFileW(z->ZipPath);
    if (z->CdPath[0])  DeleteFileW(z->CdPath);
    ZeroMemory(z, sizeof(CLIPZIP));
}

BOOL ClipZipReadComment(const WCHAR* zipPath, char* out, ULONG cap)
{
    HANDLE  h = INVALID_HANDLE_VALUE;
    BYTE*   buf = NULL;
    LARGE_INTEGER fsz;
    ULONGLONG size = 0, tail = 0, pos = 0;
    DWORD   got = 0, alloc = 0;
    LONG    i;
    BOOL    ok = FALSE;
    ZIP_EOCD e;

    if (!zipPath || !out || cap == 0) return FALSE;
    out[0] = '\0';
    h = CreateFileW(zipPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!IsHandle(h)) return FALSE;
    if (!GetFileSizeEx(h, &fsz)) goto done;
    size = (ULONGLONG)fsz.QuadPart;
    if (size < sizeof(ZIP_EOCD)) goto done;

    // Tail scan only. A 10 GiB archive never enters memory.
    tail = size;
    if (tail > 65536ull + sizeof(ZIP_EOCD))
        tail = 65536ull + sizeof(ZIP_EOCD);
    pos = size - tail;
    alloc = (DWORD)tail;
    buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, alloc);
    if (!buf) goto done;
    if (!SeekTo(h, pos)) goto done;
    if (!ReadFile(h, buf, alloc, &got, NULL) || got < sizeof(ZIP_EOCD)) goto done;

    for (i = (LONG)got - (LONG)sizeof(ZIP_EOCD); i >= 0; i--) {
        CopyMemory(&e, buf + i, sizeof(e));
        if (e.Sig != CLIPZIP_EOCD_SIG) continue;
        if ((ULONG)i + sizeof(ZIP_EOCD) + e.CommentLen > (ULONG)got) continue;
        if (e.CommentLen == 0) break;
        ULONG n = e.CommentLen;
        if (n > cap - 1) n = cap - 1;
        CopyMemory(out, buf + i + sizeof(ZIP_EOCD), n);
        out[n] = '\0';
        ok = TRUE;
        break;
    }

done:
    if (buf) {
        ZeroMemory(buf, alloc);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    if (IsHandle(h)) CloseHandle(h);
    return ok;
}