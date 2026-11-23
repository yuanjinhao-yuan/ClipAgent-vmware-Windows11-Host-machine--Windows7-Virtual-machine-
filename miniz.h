// miniz.h -- ClipAgent built-in minimal ZIP writer, store method only.
// NOTE: this is NOT richgel999/miniz. It is a project-local module.
// The names miniz.h / miniz.c are kept so build.bat and merge.bat
// need no change. Do not mix with upstream miniz sources.
// ASCII only. // comments only.
#ifndef _CLIP_MINIZ_H_
#define _CLIP_MINIZ_H_

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include "ClipProtocol.h"

// ZIP32 limit. An archive above 4 GiB is rejected at finalize; the
// --max-file-mb gate keeps normal use far below this.
#define CLIPZIP_MAX_ARCHIVE      0xFFFFFFFFull
#define CLIPZIP_MAX_COMMENT      4096
#define CLIPZIP_MAX_ENTRY_UTF8   1024
#define CLIPZIP_LOCAL_SIG        0x04034B50u
#define CLIPZIP_CENTRAL_SIG      0x02014B50u
#define CLIPZIP_EOCD_SIG         0x06054B50u
#define CLIPZIP_FLAG_UTF8        0x0800u
#define CLIPZIP_VERSION          20
#define CLIPZIP_METHOD_STORE     0

typedef struct _CLIPZIP {
    HANDLE        hZip;                     // local headers + raw file data
    HANDLE        hCd;                      // side file: central directory records
    WCHAR         ZipPath[CLIP_NAME_W];
    WCHAR         CdPath[CLIP_NAME_W];
    ULONGLONG     ZipOffset;
    ULONGLONG     CdSize;
    ULONG         EntryCount;
    char          Comment[CLIPZIP_MAX_COMMENT];
    ULONG         CommentLen;
    volatile LONG Abort;
} CLIPZIP, *PCLIPZIP;

#ifdef __cplusplus
extern "C" {
#endif

// Streaming CRC32, IEEE 0xEDB88320. Seed 0, chain across 64 KiB chunks.
ULONG ClipCrc32(ULONG seed, const void* buf, ULONG len);

// Create temp zip + side CD file. Both are owned by z until Abort.
BOOL ClipZipCreate(PCLIPZIP z, const WCHAR* zipPath, const WCHAR* cdPath);

// Add one file, store method, 64 KiB chunks, constant memory.
// entryUtf8 is a sanitized relative path using '/' separators.
// On FALSE, *pdwError carries the Win32 code for the four-way classifier.
BOOL ClipZipAddFile(PCLIPZIP z, const WCHAR* srcPath,
                    const char* entryUtf8, PDWORD pdwError);

// Skip-list text lands in the EOCD archive comment, visible to receiver.
BOOL ClipZipSetComment(PCLIPZIP z, const char* comment);

// Append central directory + EOCD, close both handles, delete the cd file.
// The zip itself is kept for streaming. *pFinalSize = final byte count.
// Rejects archives above CLIPZIP_MAX_ARCHIVE.
BOOL ClipZipFinalize(PCLIPZIP z, PULONGLONG pFinalSize);

// Idempotent teardown: close handles, delete zip AND cd, zero the struct.
// Called on OK / user cancel / peer cancel / disconnect / any failure.
VOID ClipZipAbort(PCLIPZIP z);

// Receiver side: read EOCD comment by scanning only the file tail.
// No whole-archive load. FALSE = no comment or bad signature.
BOOL ClipZipReadComment(const WCHAR* zipPath, char* out, ULONG cap);

#ifdef __cplusplus
}
#endif

#endif /* _CLIP_MINIZ_H_ */