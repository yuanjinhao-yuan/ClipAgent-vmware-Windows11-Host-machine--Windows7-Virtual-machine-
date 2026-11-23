// ClipProtocol.h -- shared wire protocol for ClipAgent (Win7 x64 / Win11 x64)
// ASCII only. // comments only. Pack(8) layout must match on both sides.
// Version 2: pull model. Copy advertises only; Ctrl+Alt+V pulls.
#ifndef _CLIP_PROTOCOL_H_
#define _CLIP_PROTOCOL_H_

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

// ---- constants ----
#define CLIP_PROTO_VERSION        2
#define CLIP_FRAME_MAGIC          0x50494C43u   // 'C''L''I''P' little-endian
#define CLIP_CHUNK_SIZE           65536
#define CLIP_DEFAULT_PORT         8080
#define CLIP_KEY_MAX              64
#define CLIP_NAME_W               260
#define CLIP_TEMP_PREFIX          "ClipAgent_"
#define CLIP_PART_SUFFIX          ".part"
#define CLIP_ZIP_SUFFIX           ".zip"
#define CLIP_SO_TIMEOUT_MS        10000
#define CLIP_DIALOG_TIMEOUT_SEC   60
#define CLIP_PAUSE_TTL_MIN        30
#define CLIP_MAX_FILE_DEFAULT_MB  256
#define CLIP_CONFIRM_DEFAULT_MB   50
#define CLIP_MAX_LIST_ITEMS       4096
#define CLIP_MAX_FRAME_PAYLOAD    (CLIP_CHUNK_SIZE + 4096)

// ---- frame types ----
#define FRAME_HELLO       0x0001
#define FRAME_LIST        0x0002
#define FRAME_PULL        0x0003
#define FRAME_ZIP_START   0x0004
#define FRAME_DATA        0x0005
#define FRAME_ZIP_END     0x0006
#define FRAME_CANCEL      0x0007
#define FRAME_CANCEL_ACK  0x0008
#define FRAME_PAUSE       0x0009
#define FRAME_RESUME      0x000A
#define FRAME_SKIP_LIST   0x000B

// ---- state machine ----
typedef enum _CLIP_STATE {
    CLIP_IDLE = 0,
    CLIP_HAVE_LIST,
    CLIP_TRANSFER,
    CLIP_PAUSED
} CLIP_STATE;

// ---- abort / pause reasons ----
typedef enum _CLIP_REASON {
    CLIP_R_OK = 0,
    CLIP_R_USER_CANCEL,
    CLIP_R_PEER_CANCEL,
    CLIP_R_DISCONNECT,
    CLIP_R_TIMEOUT,
    CLIP_R_SHARING,
    CLIP_R_ACCESS,
    CLIP_R_DISK_FULL,
    CLIP_R_PATH,
    CLIP_R_PAUSE_KEEP
} CLIP_REASON;

// ---- four-way error split ----
#define CLIPERR_NONE      0
#define CLIPERR_SHARING   1   // 32, 33
#define CLIPERR_ACCESS    2   // 5, 19
#define CLIPERR_PATH      3   // 2, 3, 183, 183-alike
#define CLIPERR_SPACE     4   // 39, 112
#define CLIPERR_OTHER     5

static __inline ULONG ClipClassifyError(DWORD e)
{
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return CLIPERR_SHARING;
    if (e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT)       return CLIPERR_ACCESS;
    if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL)         return CLIPERR_SPACE;
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
        e == ERROR_DIR_NOT_EMPTY || e == ERROR_ALREADY_EXISTS)       return CLIPERR_PATH;
    return CLIPERR_OTHER;
}

// ---- key tag: FNV-1a over --key. Label only, NOT encryption. ----
static __inline ULONG ClipKeyTag(const char* key)
{
    ULONG h = 2166136261u;
    const unsigned char* p = (const unsigned char*)key;
    if (!p) return h;
    while (*p) { h ^= *p++; h *= 16777619u; }
    return h;
}

#pragma pack(push, 8)

// ---- every frame begins with this header ----
typedef struct _CLIP_FRAME_HEADER {
    ULONG      Magic;      // CLIP_FRAME_MAGIC
    USHORT     Version;    // CLIP_PROTO_VERSION
    USHORT     Type;       // FRAME_*
    ULONG      KeyTag;     // ClipKeyTag(--key)
    ULONGLONG  Seq;        // request id / data block index
    ULONG      Length;     // payload bytes after this header
    ULONG      Crc32;      // CRC32 of payload only
} CLIP_FRAME_HEADER, *PCLIP_FRAME_HEADER;

// ---- FRAME_HELLO: first frame after connect, validates key + version ----
typedef struct _CLIP_HELLO {
    ULONG      Pid;
    USHORT     Role;       // 1 = host(listen), 2 = guest(connect)
    USHORT     Reserved;
    WCHAR      Machine[CLIP_NAME_W];
} CLIP_HELLO, *PCLIP_HELLO;

// ---- FRAME_LIST: copy-side advert, fixed size, ZERO file data ----
// Per-item paths are NOT sent across the wire. The sender re-enumerates
// locally on PULL, so memory stays constant regardless of item count.
typedef struct _CLIP_LIST {
    ULONGLONG  TotalBytes;
    ULONGLONG  LargestBytes;
    ULONG      FileCount;
    ULONG      DirCount;
    ULONG      IsDir;
    ULONG      Reserved;
    ULONGLONG  ClipSeq;    // GetClipboardSequenceNumber snapshot
    WCHAR      RootName[CLIP_NAME_W];   // display name only
} CLIP_LIST, *PCLIP_LIST;

// ---- FRAME_PULL: guest pressed Ctrl+Alt+V ----
typedef struct _CLIP_PULL {
    ULONGLONG  RequestId;
    ULONG      MaxFileMb;
    ULONG      Flags;      // bit0 = resume from PAUSED
} CLIP_PULL, *PCLIP_PULL;

// ---- FRAME_ZIP_START: sender begins streaming the temp zip ----
typedef struct _CLIP_ZIP_START {
    ULONGLONG  ZipBytes;
    ULONG      EntryCount;
    ULONG      Reserved;
    WCHAR      FileName[CLIP_NAME_W];   // basename only, e.g. project.zip
} CLIP_ZIP_START, *PCLIP_ZIP_START;

// ---- FRAME_DATA: no struct. Header.Length = actual chunk size,
//      Header.Seq = block index, payload = raw bytes (<= CLIP_CHUNK_SIZE).

// ---- FRAME_ZIP_END ----
typedef struct _CLIP_ZIP_END {
    ULONG      WholeCrc32;
    ULONG      SkippedCount;
    ULONGLONG  SkippedBytes;
    ULONG      Reserved;
} CLIP_ZIP_END, *PCLIP_ZIP_END;

// ---- FRAME_CANCEL / FRAME_CANCEL_ACK ----
typedef struct _CLIP_CANCEL {
    ULONG      Reason;     // CLIP_REASON
    ULONGLONG  RequestId;
} CLIP_CANCEL, *PCLIP_CANCEL;

// ---- FRAME_PAUSE: offending operation, four-way class, raw win32 code ----
typedef struct _CLIP_PAUSE {
    ULONG      Reason;     // CLIP_REASON
    ULONG      ErrClass;   // CLIPERR_*
    DWORD      Win32Error;
    ULONGLONG  RequestId;
    WCHAR      Subject[CLIP_NAME_W];   // offending file / dir
} CLIP_PAUSE, *PCLIP_PAUSE;

// ---- FRAME_RESUME: user's choice after pause ----
typedef struct _CLIP_RESUME {
    ULONGLONG  RequestId;
    ULONG      Action;     // 0=resume, 1=skip-subject, 2=terminate-owner
    ULONG      Reserved;
} CLIP_RESUME, *PCLIP_RESUME;

// ---- FRAME_SKIP_LIST: summary only; names live in zip archive comment ----
typedef struct _CLIP_SKIP_LIST {
    ULONG      Count;
    ULONGLONG  TotalBytes;
} CLIP_SKIP_LIST, *PCLIP_SKIP_LIST;

#pragma pack(pop)

#endif /* _CLIP_PROTOCOL_H_ */