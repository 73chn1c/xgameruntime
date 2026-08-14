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

#include <bcrypt.h>

struct sisu_auth_result
{
    UINT64 xuid;
    char gamertag[16];
    char modernGamertag[97];
    char modernGamertagSuffix[15];
    char uniqueModernGamertag[101];
    WCHAR *xblAuthHeader; /* "Authorization: XBL3.0 x=-;<token>", owned - free with free() */
    /* Kept alive (NOT destroyed) on success so the same ProofKey identity
     * used during sign-in can go on signing later, per-request calls via
     * sisu_sign_request() - real Xbox Live services expect every signed
     * request from a session to use the same key the sign-in was
     * negotiated with. Caller owns this and must BCryptDestroyKey() it
     * once truly done with the session (e.g. process exit). */
    BCRYPT_KEY_HANDLE key;
};

/* Performs the full real sign-in chain: launches the shim process (env
 * var XODUS_IPC_PROXY must point at xodus-service or a compatible shim),
 * requests a real MSA/device RPS ticket pair for msaAppId, exchanges them
 * for a device token then a full SISU authorization, and fills *result.
 * Synchronous/blocking (real network I/O) - callers running this on the
 * XAsync DoWork step (off the calling thread) is the caller's
 * responsibility, this function does no threading of its own. */
HRESULT sisu_perform_auth( const char *msaAppId, BOOLEAN allowUi, struct sisu_auth_result *result );

/* Computes the real, documented Xbox Live request-signature ("Signature:"
 * header value) for an arbitrary caller-supplied request - the same
 * ProofKey-based ECDSA scheme sisu_perform_auth uses internally for its
 * own device_auth/sisu_auth calls, exposed here so a caller (like
 * XUserGetTokenAndSignatureAsync) can sign whatever specific request the
 * game is actually about to make. url must be absolute; signature must
 * point at a 104-byte buffer (real, fixed size of this signature scheme's
 * base64 output) - NOT NUL-terminated, matching get_signature()'s own
 * real output shape. */
HRESULT sisu_sign_request( BCRYPT_KEY_HANDLE key, const char *method, const WCHAR *url, const char *auth, SIZE_T bodySize, const void *body, char signature[104] );

#endif
