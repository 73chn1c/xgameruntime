/*
 * Real Xbox Live SISU authentication - see sisu_auth.h. Core protocol
 * logic (request/response shapes, the ProofKey/Signature scheme, the
 * device_auth/sisu_auth endpoint sequence) ported faithfully from
 * xodus-gaming/xgameruntime's own "xuser-ipc" branch (real,
 * author-authored code - not guessed), with the JSON/XML layer swapped
 * for this fork's own minimal json_min.h/shim_xml.h (see those files for
 * why: no libxml2 for mingw, no Windows.Data.Json WinRT component here).
 */

#include "private.h"
#include "shim_ipc.h"
#include "shim_xml.h"
#include "json_min.h"
#include "sisu_auth.h"
#include <bcrypt.h>
#include <winternl.h>
#include <winhttp.h>
/* wininet.h is deliberately NOT included alongside winhttp.h - their
 * headers redefine several of the same types/macros (BOOLAPI,
 * INTERNET_SCHEME, URL_COMPONENTSW, HTTP_VERSION_INFO, ...) and can't
 * coexist in one translation unit. WinHttpCrackUrl (already used by
 * http_request below) covers the one wininet-only call (InternetCrackUrlA)
 * the upstream xuser-ipc code used - see get_signature. */

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static const WCHAR USER_AGENT[] = L"curl/1.0";
static const WCHAR *ACCEPT_JSON[] = { L"application/json", NULL };

/* ---- HTTP (verbatim port of util.c's http_request, WinHTTP-based) ---- */

static HRESULT http_request( const WCHAR *method, const WCHAR *url, char *data, const WCHAR *headers, const WCHAR **accept, UCHAR **buffer, SIZE_T *bufferSize )
{
    URL_COMPONENTS uc = { .dwStructSize = sizeof(URL_COMPONENTS), .dwHostNameLength = -1, .dwUrlPathLength = -1 };
    HINTERNET connection = NULL, request = NULL, session = NULL;
    DWORD size = sizeof( DWORD ), status;
    WCHAR *hostName = NULL;
    UCHAR *tmpBuffer = NULL;
    HRESULT hr = S_OK;

    TRACE( "method %s, url %s, data %s, headers %s, accept %p, buffer %p, bufferSize %p.\n",
           debugstr_w( method ), debugstr_w( url ), debugstr_a( data ), debugstr_w( headers ), accept, buffer, bufferSize );

    if (!WinHttpCrackUrl( url, 0, 0, &uc )) goto error;
    if (!(hostName = calloc( uc.dwHostNameLength + 1, sizeof(WCHAR) )))
    {
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }
    memcpy( hostName, uc.lpszHostName, uc.dwHostNameLength * sizeof(WCHAR) );

    if (!(session = WinHttpOpen( USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 ))) goto error;
    if (!(connection = WinHttpConnect( session, hostName, INTERNET_DEFAULT_HTTPS_PORT, 0 ))) goto error;
    if (!(request = WinHttpOpenRequest( connection, method, uc.lpszUrlPath, NULL, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE ))) goto error;
    if (!WinHttpSendRequest( request, headers, -1, data, (data ? strlen( data ) : 0), (data ? strlen( data ) : 0), 0 )) goto error;
    if (!WinHttpReceiveResponse( request, NULL )) goto error;
    if (!WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX )) goto error;
    if (status != 200)
    {
        WARN( "HTTP request to %s failed with status %lu.\n", debugstr_w( url ), status );
        hr = E_FAIL;
        goto cleanup;
    }

    *bufferSize = 0;
    *buffer = NULL;
    do
    {
        if (!WinHttpQueryDataAvailable( request, &size )) goto error;
        if (!size) break;
        if (!(tmpBuffer = realloc( *buffer, *bufferSize + size )))
        {
            hr = E_OUTOFMEMORY;
            goto cleanup;
        }
        *buffer = tmpBuffer;

        if (!WinHttpReadData( request, *buffer + *bufferSize, size, &size )) goto error;
        *bufferSize += size;
    }
    while (size);
    goto cleanup;

error:
    hr = HRESULT_FROM_WIN32( GetLastError() );
cleanup:
    if (connection) WinHttpCloseHandle( connection );
    if (request) WinHttpCloseHandle( request );
    if (session) WinHttpCloseHandle( session );
    if (hostName) free( hostName );
    if (SUCCEEDED(hr)) return hr;
    if (*buffer) free( *buffer );
    *bufferSize = 0;
    *buffer = NULL;
    return hr;
}

/* ---- base64 (verbatim port of util.c's encode_base64/_url) ---- */

#define encode_base64_(sfx,alph)                                                                                          \
static HRESULT encode_base64##sfx( const UINT32 dataSize, const BYTE *data, const UINT32 base64Size, char *base64, BOOLEAN pad ) \
{                                                                                                                          \
    static const char alphabet[64] = alph;                                                                                \
    const BYTE *inp = data;                                                                                               \
    char *out = base64;                                                                                                   \
    UINT32 i;                                                                                                             \
                                                                                                                           \
    if ((dataSize * 8 + 5) / 6 + (pad ? (4 - (dataSize % 4)) % 4 : 0) > base64Size)                                        \
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );                                                           \
                                                                                                                           \
    for (i = 0; i < dataSize / 3; i++)                                                                                    \
    {                                                                                                                     \
        *out++ = alphabet[ (inp[0] >> 2) & 0x3f ];                                                                        \
        *out++ = alphabet[ ((inp[0] << 4) & 0x30) | ((inp[1] >> 4) & 0x0f) ];                                             \
        *out++ = alphabet[ ((inp[1] << 2) & 0x3c) | ((inp[2] >> 6) & 0x03) ];                                             \
        *out++ = alphabet[ inp[2] & 0x3f ];                                                                               \
        inp += 3;                                                                                                        \
    }                                                                                                                     \
                                                                                                                           \
    switch (dataSize % 3)                                                                                                 \
    {                                                                                                                     \
    case 1:                                                                                                               \
        *out++ = alphabet[ (inp[0] >> 2) & 0x3f ];                                                                        \
        *out++ = alphabet[ ((inp[0] << 4) & 0x30) ];                                                                      \
        if (pad) { *out++ = '='; *out++ = '='; }                                                                          \
        break;                                                                                                            \
    case 2:                                                                                                               \
        *out++ = alphabet[ (inp[0] >> 2) & 0x3f ];                                                                        \
        *out++ = alphabet[ ((inp[0] << 4) & 0x30) | ((inp[1] >> 4) & 0x0f) ];                                             \
        *out++ = alphabet[ ((inp[1] << 2) & 0x3c) ];                                                                      \
        if (pad) *out++ = '=';                                                                                            \
        break;                                                                                                            \
    }                                                                                                                     \
    return S_OK;                                                                                                          \
}

encode_base64_(_url,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
encode_base64_(,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")

/* ---- ProofKey (JWK) generation ---- */

static HRESULT generate_proof_key( BCRYPT_KEY_HANDLE *key, char **proofKeyOut )
{
    static const char template[] = "{\"alg\":\"ES256\",\"crv\":\"P-256\",\"kty\":\"EC\",\"use\":\"sig\",\"x\":\"";
    BYTE blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    char *proofKey, *x, *y;
    NTSTATUS status;
    ULONG dummy;
    HRESULT hr;

    if (!NT_SUCCESS(status = BCryptGenerateKeyPair( BCRYPT_ECDSA_P256_ALG_HANDLE, key, 256, 0 ))) return HRESULT_FROM_NT( status );
    if (!NT_SUCCESS(status = BCryptFinalizeKeyPair( *key, 0 ))) return HRESULT_FROM_NT( status );
    if (!NT_SUCCESS(status = BCryptExportKey( *key, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &dummy, 0 ))) return HRESULT_FROM_NT( status );

    if (!(proofKey = calloc( 1, sizeof(template) + sizeof("\",\"y\":\"\"}") + 85 ))) return E_OUTOFMEMORY;
    x = proofKey + sizeof(template) - 1;
    y = x + 43 + strlen( "\",\"y\":\"" );
    strcpy( proofKey, template );
    if (FAILED(hr = encode_base64_url( 32, blob + sizeof(BCRYPT_ECCKEY_BLOB), 43, x, FALSE ))) { free( proofKey ); return hr; }
    strcat( proofKey, "\",\"y\":\"" );
    if (FAILED(hr = encode_base64_url( 32, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, 43, y, FALSE ))) { free( proofKey ); return hr; }
    strcat( proofKey, "\"}" );

    *proofKeyOut = proofKey;
    return S_OK;
}

/* ---- request signing (verbatim port of xuser.c's user_GetSignature) ---- */

static HRESULT get_signature( BCRYPT_KEY_HANDLE key, UINT32 version, const char *method, const WCHAR *url, const char *auth, UINT32 bodySize, const void *body, char signature[104] )
{
    BYTE hash[32], rawSignature[76] = { (version >> 24) & 0xff, (version >> 16) & 0xff, (version >> 8) & 0xff, version & 0xff };
    URL_COMPONENTS uc = { .dwStructSize = sizeof(URL_COMPONENTS), .dwUrlPathLength = -1, .dwExtraInfoLength = -1 };
    ULONG dataBufferSize, dummy;
    BYTE *dataBuffer, *ptr;
    FILETIME timestamp;
    NTSTATUS status;
    int urlPathLen;
    HRESULT hr;
    const char *m;

    /* WinHttpCrackUrl only works on wide strings - convert the extracted
     * path+query back to narrow (UTF-8) afterward for the signature
     * buffer, which real Xbox Live services expect as raw bytes, not
     * UTF-16. wininet.h's InternetCrackUrlA (the upstream xuser-ipc
     * branch's original choice) can't be used here alongside winhttp.h in
     * the same translation unit - see the top-of-file comment. */
    if (!WinHttpCrackUrl( url, 0, 0, &uc )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!(urlPathLen = WideCharToMultiByte( CP_UTF8, 0, uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength, NULL, 0, NULL, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );

    dataBufferSize = 18 + strlen( method ) + (ULONG)urlPathLen + strlen( auth ) + bodySize;

    GetSystemTimeAsFileTime( &timestamp );
    rawSignature[4]  = (timestamp.dwHighDateTime >> 24) & 0xff;
    rawSignature[5]  = (timestamp.dwHighDateTime >> 16) & 0xff;
    rawSignature[6]  = (timestamp.dwHighDateTime >> 8 ) & 0xff;
    rawSignature[7]  =  timestamp.dwHighDateTime        & 0xff;
    rawSignature[8]  = (timestamp.dwLowDateTime  >> 24) & 0xff;
    rawSignature[9]  = (timestamp.dwLowDateTime  >> 16) & 0xff;
    rawSignature[10] = (timestamp.dwLowDateTime  >> 8 ) & 0xff;
    rawSignature[11] =  timestamp.dwLowDateTime         & 0xff;

    if (!(dataBuffer = calloc( 1, dataBufferSize ))) return E_OUTOFMEMORY;
    memcpy( dataBuffer, rawSignature, 4 );
    memcpy( dataBuffer + 5, rawSignature + 4, 8 );
    ptr = dataBuffer + 14;
    for (m = method; *m; m++) *(ptr++) = (BYTE)toupper( (unsigned char)*m );
    ptr++; /* NUL after method */
    if (!WideCharToMultiByte( CP_UTF8, 0, uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength, (char *)ptr, urlPathLen, NULL, NULL ))
    {
        free( dataBuffer );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    ptr += urlPathLen + 1; /* + NUL */
    memcpy( ptr, auth, strlen( auth ) );
    ptr += strlen( auth ) + 1; /* + NUL */
    memcpy( ptr, body, bodySize );

    if (!NT_SUCCESS(status = BCryptHash( BCRYPT_SHA256_ALG_HANDLE, NULL, 0, dataBuffer, dataBufferSize, hash, 32 ))) goto error;
    if (!NT_SUCCESS(status = BCryptSignHash( key, NULL, hash, 32, rawSignature + 12, 64, &dummy, 0 ))) goto error;

    hr = encode_base64( 76, rawSignature, 104, signature, TRUE );
    free( dataBuffer );
    return hr;

error:
    free( dataBuffer );
    return HRESULT_FROM_NT( status );
}

/* ---- shim MSA token request/response (using shim_ipc.h + shim_xml.h) ---- */

static HRESULT get_rps_tickets( struct shim_channel *shim, const char *msaAppId, BOOLEAN allowUi, char **userTicket, char **deviceTicket )
{
    char *payload = NULL, *resp = NULL;
    UINT16 payloadLen, respType, respLen;
    HRESULT hr;

    if (!build_msa_token_request_xml( msaAppId, allowUi, FALSE, &payload, &payloadLen )) return E_OUTOFMEMORY;
    hr = shim_call( shim, MSA_TOKEN_REQUEST, payload, payloadLen, &respType, &resp, &respLen );
    free( payload );
    if (FAILED(hr)) return hr;

    if (!xml_get_tag_text( resp, respLen, "Token", userTicket ) ||
        !xml_get_tag_text( resp, respLen, "DeviceRps", deviceTicket ))
    {
        free( resp );
        return E_FAIL;
    }
    free( resp );
    return S_OK;
}

/* ---- device_auth (JSON layer swapped for json_min.h) ---- */

static HRESULT device_auth( BCRYPT_KEY_HANDLE key, const char *proofKey, const char *deviceTicket, char **deviceToken )
{
    static const char template[] = "{\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\",\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"ProofKey\":";
    SIZE_T size = sizeof(template) + strlen( proofKey ) + strlen( ",\"RpsTicket\":\"\"}}" ) + strlen( deviceTicket );
    struct json_value *root = NULL;
    char signature[104];
    UCHAR *buffer = NULL;
    SIZE_T bufSize;
    HRESULT hr;
    char *body;
    char *tok;

    if (!(body = calloc( 1, size ))) return E_OUTOFMEMORY;
    strcpy( body, template );
    strcat( body, proofKey );
    strcat( body, ",\"RpsTicket\":\"" );
    strcat( body, deviceTicket );
    strcat( body, "\"}}" );

    if (FAILED(hr = get_signature( key, 1, "POST", L"https://device.auth.xboxlive.com/device/authenticate", "", (UINT32)(size - 1), body, signature ))) goto cleanup;

    {
        /* signature[104] is a raw base64 byte buffer, NOT a NUL-terminated
         * C string (base64 output has no terminator) - converting it with
         * an explicit length of 104 (not -1) and writing straight into a
         * WCHAR array pre-filled with the "Signature: " prefix, exactly
         * as the real upstream xuser-ipc branch's own user_GetSignature
         * caller does, avoids treating it as a string at all. An earlier
         * version of this function used strcat() on it, which read past
         * the 104-byte buffer looking for a NUL that was never there -
         * a real bug caught via a live ERROR_NO_UNICODE_TRANSLATION
         * failure during testing (the resulting garbage bytes weren't
         * valid UTF-8), not something guessed. */
        WCHAR headerW[116] = L"Signature: ";
        if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, signature, 104, headerW + 11, 104 )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto cleanup; }
        headerW[115] = L'\0';
        hr = http_request( L"POST", L"https://device.auth.xboxlive.com/device/authenticate", body, headerW, ACCEPT_JSON, &buffer, &bufSize );
    }
    if (FAILED(hr)) goto cleanup;

    if (!(root = json_parse( (char *)buffer, bufSize )) || !json_get_string( root, "Token", &tok, NULL ))
    {
        hr = E_FAIL;
        goto cleanup;
    }
    if (!(*deviceToken = _strdup( tok ))) hr = E_OUTOFMEMORY;

cleanup:
    if (root) json_free( root );
    if (buffer) free( buffer );
    free( body );
    return hr;
}

/* ---- sisu_auth (JSON layer swapped for json_min.h) ---- */

static HRESULT sisu_auth( BCRYPT_KEY_HANDLE key, const char *userTicket, const char *deviceToken, struct sisu_auth_result *result )
{
    static const char template[] = "{\"Sandbox\":\"RETAIL\",\"UseModernGamertag\":true,\"DeviceToken\":\"";
    SIZE_T size = sizeof(template) + strlen( deviceToken ) + strlen( "\",\"AccessToken\":\"\"}" ) + strlen( userTicket );
    struct json_value *root = NULL;
    const struct json_value *userObj, *authObj, *claims, *xui, *identity;
    char signature[104];
    UCHAR *buffer = NULL;
    SIZE_T bufSize;
    HRESULT hr;
    char *body;
    char *s;

    if (!(body = calloc( 1, size ))) return E_OUTOFMEMORY;
    strcpy( body, template );
    strcat( body, deviceToken );
    strcat( body, "\",\"AccessToken\":\"" );
    strcat( body, userTicket );
    strcat( body, "\"}" );

    if (FAILED(hr = get_signature( key, 1, "POST", L"https://sisu.xboxlive.com/authorize", "", (UINT32)(size - 1), body, signature ))) goto cleanup;

    {
        /* See the identical fix + rationale in device_auth() above. */
        WCHAR headerW[116] = L"Signature: ";
        if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, signature, 104, headerW + 11, 104 )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto cleanup; }
        headerW[115] = L'\0';
        hr = http_request( L"POST", L"https://sisu.xboxlive.com/authorize", body, headerW, ACCEPT_JSON, &buffer, &bufSize );
    }
    if (FAILED(hr)) goto cleanup;

    if (!(root = json_parse( (char *)buffer, bufSize ))) { hr = E_FAIL; goto cleanup; }
    if (!json_get_object( root, "UserToken", &userObj ) || !json_get_string( userObj, "Token", &s, NULL )) { hr = E_FAIL; goto cleanup; }

    if (!json_get_object( root, "AuthorizationToken", &authObj ) ||
        !json_get_object( authObj, "DisplayClaims", &claims ) ||
        !json_get_array( claims, "xui", &xui ) ||
        !(identity = json_array_at( xui, 0 )))
    {
        hr = E_FAIL;
        goto cleanup;
    }

    if (!json_get_string( identity, "xid", &s, NULL )) { hr = E_FAIL; goto cleanup; }
    result->xuid = _strtoui64( s, NULL, 10 );

    if (!json_get_string( identity, "gtg", &s, NULL )) { hr = E_FAIL; goto cleanup; }
    lstrcpynA( result->gamertag, s, sizeof(result->gamertag) );

    if (!json_get_string( identity, "mgt", &s, NULL )) { hr = E_FAIL; goto cleanup; }
    lstrcpynA( result->modernGamertag, s, sizeof(result->modernGamertag) );

    if (json_get_string( identity, "mgs", &s, NULL ))
        lstrcpynA( result->modernGamertagSuffix, s, sizeof(result->modernGamertagSuffix) );

    if (!json_get_string( identity, "umg", &s, NULL )) { hr = E_FAIL; goto cleanup; }
    lstrcpynA( result->uniqueModernGamertag, s, sizeof(result->uniqueModernGamertag) );

    if (!json_get_string( authObj, "Token", &s, NULL )) { hr = E_FAIL; goto cleanup; }
    {
        WCHAR *tokenW;
        int tokenWLen = MultiByteToWideChar( CP_UTF8, 0, s, -1, NULL, 0 );
        static const WCHAR prefix[] = L"Authorization: XBL3.0 x=-;";

        if (!tokenWLen) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto cleanup; }
        if (!(result->xblAuthHeader = calloc( wcslen( prefix ) + tokenWLen, sizeof(WCHAR) ))) { hr = E_OUTOFMEMORY; goto cleanup; }
        wcscpy( result->xblAuthHeader, prefix );
        tokenW = result->xblAuthHeader + wcslen( prefix );
        if (!MultiByteToWideChar( CP_UTF8, 0, s, -1, tokenW, tokenWLen )) { hr = HRESULT_FROM_WIN32( GetLastError() ); goto cleanup; }
    }
    hr = S_OK;

cleanup:
    if (root) json_free( root );
    if (buffer) free( buffer );
    free( body );
    return hr;
}

/* ---- entry point ---- */

HRESULT sisu_perform_auth( const char *msaAppId, BOOLEAN allowUi, struct sisu_auth_result *result )
{
    char *proofKey = NULL, *userTicket = NULL, *deviceTicket = NULL, *deviceToken = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    struct shim_channel shim = { 0 };
    HRESULT hr;

    TRACE( "msaAppId %s, allowUi %d, result %p.\n", debugstr_a( msaAppId ), allowUi, result );

    memset( result, 0, sizeof(*result) );

    if (FAILED(hr = shim_start( &shim, L"XODUS_IPC_PROXY" )))
    {
        WARN( "shim_start failed, hr %#lx - is XODUS_IPC_PROXY set to a running xodus-service binary?\n", hr );
        return hr;
    }

    if (FAILED(hr = generate_proof_key( &key, &proofKey ))) { ERR( "generate_proof_key failed, hr %#lx.\n", hr ); goto cleanup; }
    if (FAILED(hr = get_rps_tickets( &shim, msaAppId, allowUi, &userTicket, &deviceTicket ))) { ERR( "get_rps_tickets failed, hr %#lx.\n", hr ); goto cleanup; }
    if (FAILED(hr = device_auth( key, proofKey, deviceTicket, &deviceToken ))) { ERR( "device_auth failed, hr %#lx.\n", hr ); goto cleanup; }
    hr = sisu_auth( key, userTicket, deviceToken, result );
    if (FAILED(hr)) ERR( "sisu_auth failed, hr %#lx.\n", hr );

cleanup:
    shim_stop( &shim );
    if (key) BCryptDestroyKey( key );
    free( proofKey );
    free( userTicket );
    free( deviceTicket );
    free( deviceToken );
    if (FAILED(hr)) free( result->xblAuthHeader );
    return hr;
}
