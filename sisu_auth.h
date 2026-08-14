/*
 * Real Xbox Live user authentication (SISU - Microsoft's "Sign-In and Set
 * Up" protocol) - a self-contained port of xodus-gaming/xgameruntime's
 * "xuser-ipc" branch real, author-authored auth chain (get_rps_tickets ->
 * device_auth -> sisu_auth), adapted to:
 *   - this fork's own json_min.h / shim_xml.h (no libxml2 for mingw, no
 *     Windows.Data.Json WinRT component in this fork)
 *   - a simple flat output API instead of the upstream's own separate
 *     IUser COM object design, so it can be called directly from this
 *     fork's existing xuser.c (IXUserImpl6) without replacing its
 *     architecture.
 */
#ifndef __WINE_XGAMERUNTIME_SISU_AUTH_H
#define __WINE_XGAMERUNTIME_SISU_AUTH_H

struct sisu_auth_result
{
    UINT64 xuid;
    char gamertag[16];
    char modernGamertag[97];
    char modernGamertagSuffix[15];
    char uniqueModernGamertag[101];
    WCHAR *xblAuthHeader; /* "Authorization: XBL3.0 x=-;<token>", owned - free with free() */
};

/* Performs the full real sign-in chain: launches the shim process (env
 * var XODUS_IPC_PROXY must point at xodus-service or a compatible shim),
 * requests a real MSA/device RPS ticket pair for msaAppId, exchanges them
 * for a device token then a full SISU authorization, and fills *result.
 * Synchronous/blocking (real network I/O) - callers running this on the
 * XAsync DoWork step (off the calling thread) is the caller's
 * responsibility, this function does no threading of its own. */
HRESULT sisu_perform_auth( const char *msaAppId, BOOLEAN allowUi, struct sisu_auth_result *result );

#endif
