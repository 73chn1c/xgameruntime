/*
 * IPC client for the "shim" (xodus-service, this project's own Rust
 * daemon that owns the real, already-authenticated Xbox Live credential
 * store) - connects over ordinary loopback TCP/winsock rather than
 * spawning a child process. An earlier version of this file launched a
 * native helper process via CreateProcessW and talked to it over
 * stdin/stdout pipes (matching the design of xodus-gaming/xgameruntime's
 * "xuser-ipc" branch, which this is otherwise a faithful port of) - that
 * approach was dropped after live testing showed this fork's Wine build
 * doesn't reliably launch/wire pipes for native (non-PE) child processes
 * (CreateProcessInternalW reported a bogus pid 0/tid 0 and the pipe I/O
 * failed immediately with ERROR_NO_DATA). xodus-service already listens
 * on 127.0.0.1:47810 speaking the exact same wire protocol as a loopback
 * TCP mirror of its Unix socket specifically for this reason - ordinary
 * winsock TCP is solid, well-exercised code in every Wine build, unlike
 * native process spawning. Wire protocol (shim_header + payload) is
 * unchanged.
 */
#ifndef __WINE_XGAMERUNTIME_SHIM_IPC_H
#define __WINE_XGAMERUNTIME_SHIM_IPC_H

#include <winsock2.h>

struct shim_header
{
    UINT32 magic;
    UINT16 type;
    UINT16 length;
};

#define XML_MAGIC 0x58445358
#define PING_REQUEST 1
#define PING_RESPONSE 2
#define MSA_TOKEN_REQUEST  3
#define MSA_TOKEN_RESPONSE  4

#define SHIM_TCP_PORT 47810

struct shim_channel
{
    SOCKET sock;
    CRITICAL_SECTION lock;
    BOOL active;
};

/* envVar is accepted for source-compatibility with the original
 * process-launch design but no longer used - connection is always to
 * 127.0.0.1:SHIM_TCP_PORT. Kept as a parameter so call sites don't need
 * to change if a future revision reintroduces a configurable target. */
HRESULT shim_start( struct shim_channel *shim, const WCHAR *envVar );
void shim_stop( struct shim_channel *shim );
HRESULT shim_call( struct shim_channel *shim, UINT16 type, const char *payload, UINT16 payloadLen,
                    UINT16 *respType, char **resp, UINT16 *respLen );

#endif
