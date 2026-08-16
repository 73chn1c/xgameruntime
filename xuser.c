/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XUser
 *
 * Copyright 2026 Olivia Ryan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "private.h"
#include "sisu_auth.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_user
{
    IXUserImpl6 IXUserImpl6_iface;
    IXUserGamertagImpl IXUserGamertagImpl_iface;
    IXUserDeviceImpl2 IXUserDeviceImpl2_iface;
    LONG ref;
};

static inline struct x_user *impl_from_IXUserImpl6( IXUserImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserImpl6_iface );
}

/* This build has no real Xbox Live account/sign-in stack (no network
 * account picker, no cached MSA tokens), so per the documented contract for
 * XUserAddAsync (learn.microsoft.com .../xuser/functions/xuseraddasync -
 * "XUserAddOptions::AddDefaultUserSilently ... does not show a UI") this
 * implements the offline/no-UI path: a single, stable, synthetic local
 * default user that XUserGetMaxUsers() (already fixed at 1, see below)
 * always allows exactly one of. The handle is an immortal process-lifetime
 * singleton rather than individually refcounted/freed - real titles only
 * ever see a single default user on this build, there is nothing else to
 * multiplex, and never actually deallocating it means XUserCloseHandle can
 * never be a use-after-close/double-free hazard. */
struct x_user_data
{
    UINT64 id;
    XUserLocalId local_id;
};

static CRITICAL_SECTION default_user_cs;
static CRITICAL_SECTION_DEBUG default_user_cs_debug =
{
    0, 0, &default_user_cs,
    { &default_user_cs_debug.ProcessLocksList, &default_user_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": default_user_cs") }
};
static CRITICAL_SECTION default_user_cs = { &default_user_cs_debug, -1, 0, 0, 0, 0 };
static struct x_user_data *default_user;

static struct x_user_data *get_default_user( void )
{
    struct x_user_data *user;

    EnterCriticalSection( &default_user_cs );
    if (!default_user)
    {
        if ((user = calloc( 1, sizeof(*user) )))
        {
            /* Real XUserLocalId/XUID values are opaque to titles - any
             * stable, nonzero value round-trips correctly through
             * XUserGetLocalId()/XUserFindUserByLocalId() and
             * XUserGetId()/XUserFindUserById(), which is all the documented
             * contract requires. */
            user->local_id.value = 1;
            user->id = 1;
            default_user = user;
        }
    }
    user = default_user;
    LeaveCriticalSection( &default_user_cs );
    return user;
}

/* Stable identity pointer for XAsyncBegin/XAsyncGetResult pairing - see the
 * identical pattern and rationale on token_sig_identity below. */
static const char x_user_add_identity[] = "XUserAddAsync";

struct x_user_add_state
{
    HRESULT hr;
    XUserHandle user;
};

static HRESULT CALLBACK x_user_add_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct x_user_add_state *state = data->context;

    switch (op)
    {
    case XAsyncOp_Begin:
        /* No real Xbox Live account picker or network round trip involved -
         * the result was already computed synchronously before XAsyncBegin
         * was even called (see x_user_XUserAddAsync below), so complete
         * immediately instead of scheduling a DoWork step. */
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, state->hr, SUCCEEDED(state->hr) ? sizeof(XUserHandle) : 0 );
        return S_OK;

    case XAsyncOp_GetResult:
        if (data->bufferSize < sizeof(XUserHandle)) return E_NOT_SUFFICIENT_BUFFER;
        *(XUserHandle *)data->buffer = state->user;
        return S_OK;

    case XAsyncOp_Cleanup:
        free( state );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT WINAPI x_user_QueryInterface( IXUserImpl6 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXUserImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserImpl2 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl3 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl4 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl5 ) ||
        IsEqualGUID( iid, &IID_IXUserImpl6 ))
    {
        IXUserImpl6_AddRef( *out = &impl->IXUserImpl6_iface );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IXUserGamertagImpl ))
    {
        IXUserGamertagImpl_AddRef( *out = &impl->IXUserGamertagImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_AddRef( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_user_Release( IXUserImpl6 *iface )
{
    struct x_user *impl = impl_from_IXUserImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_user_XUserDuplicateHandle( IXUserImpl6 *iface, XUserHandle handle, XUserHandle *duplicatedHandle )
{
    TRACE( "iface %p, handle %p, duplicatedHandle %p.\n", iface, handle, duplicatedHandle );

    if (!duplicatedHandle) return E_INVALIDARG;
    if (!handle || (struct x_user_data *)handle != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    /* The handle is an immortal singleton (see struct x_user_data above),
     * so a "duplicate" is just the same pointer - there is nothing separate
     * to allocate or refcount. */
    *duplicatedHandle = handle;
    return S_OK;
}

static void WINAPI x_user_XUserCloseHandle( IXUserImpl6 *iface, XUserHandle user )
{
    TRACE( "iface %p, user %p.\n", iface, user );
    /* No-op: see struct x_user_data comment above - the synthetic default
     * user is never actually deallocated, so closing a handle to it is
     * always safe and never a use-after-close hazard. */
}

static INT32 WINAPI x_user_XUserCompare( IXUserImpl6 *iface, XUserHandle user1, XUserHandle user2 )
{
    TRACE( "iface %p, user1 %p, user2 %p.\n", iface, user1, user2 );
    /* Only one user (the singleton) can ever exist on this build, so handle
     * identity is exactly user identity. */
    return user1 == user2 ? 0 : -1;
}

static HRESULT WINAPI x_user_XUserGetMaxUsers( IXUserImpl6 *iface, UINT32 *maxUsers )
{
    TRACE( "iface %p, maxUsers %p.\n", iface, maxUsers );
    *maxUsers = 1;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserAddAsync( IXUserImpl6 *iface, XUserAddOptions options, XAsyncBlock *async )
{
    struct x_user_add_state *state;
    struct x_user_data *user;
    HRESULT hr;

    TRACE( "iface %p, options %#x, async %p.\n", iface, (unsigned int)options, async );

    if (!async) return E_INVALIDARG;
    /* Documented as mutually exclusive: "You cannot use
     * XUserAddOptions::AllowGuests with XUserAddOptions::AddDefaultUserSilently.
     * A guest cannot be the default user." */
    if ((options & XUserAddOptions_AllowGuests) && (options & XUserAddOptions_AddDefaultUserSilently))
        return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;

    /* No real Xbox Live account picker exists on this build, so every
     * XUserAddOptions combination (None / AddDefaultUserSilently /
     * AddDefaultUserAllowingUI, with or without AllowGuests) resolves the
     * same documented "default user" rather than showing UI - see the
     * struct x_user_data comment above. Computed synchronously up front;
     * x_user_add_provider's Begin step just reports it through the real
     * async engine (asyncBlock->internal[0] is that engine's own state
     * pointer now - see async_state_from_block - so every XUser*Async
     * completion has to go through XAsyncBegin/XAsyncComplete rather than
     * stashing a private result type there directly, or generic callers
     * like XAsyncGetStatus/XAsyncGetResultSize crash dereferencing it as
     * the wrong type). */
    if ((user = get_default_user()))
    {
        state->hr = S_OK;
        state->user = (XUserHandle)user;
    }
    else
    {
        state->hr = E_OUTOFMEMORY;
        state->user = NULL;
    }

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, state, &x_user_add_identity, "XUserAddAsync", x_user_add_provider )))
        free( state );
    return hr;
}

static HRESULT WINAPI x_user_XUserAddResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    XUserHandle user = NULL;
    HRESULT hr;

    TRACE( "iface %p, async %p, newUser %p.\n", iface, async, newUser );

    if (!async || !newUser) return E_INVALIDARG;

    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_user_add_identity, sizeof(user), &user, NULL );
    *newUser = SUCCEEDED( hr ) ? user : NULL;
    return hr;
}

static HRESULT WINAPI x_user_XUserGetLocalId( IXUserImpl6 *iface, XUserHandle user, XUserLocalId *userLocalId )
{
    struct x_user_data *impl = (struct x_user_data *)user;

    TRACE( "iface %p, user %p, userLocalId %p.\n", iface, user, userLocalId );

    if (!userLocalId) return E_INVALIDARG;
    if (!user || impl != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    *userLocalId = impl->local_id;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserByLocalId( IXUserImpl6 *iface, XUserLocalId userLocalId, XUserHandle *handle )
{
    TRACE( "iface %p, userLocalId %s, handle %p.\n", iface, wine_dbgstr_longlong(userLocalId.value), handle );

    if (!handle) return E_INVALIDARG;

    if (!default_user || userLocalId.value != default_user->local_id.value)
    {
        *handle = NULL;
        return E_GAMEUSER_USER_NOT_FOUND;
    }

    *handle = (XUserHandle)default_user;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetId( IXUserImpl6 *iface, XUserHandle user, UINT64 *userId )
{
    struct x_user_data *impl = (struct x_user_data *)user;

    TRACE( "iface %p, user %p, userId %p.\n", iface, user, userId );

    if (!userId) return E_INVALIDARG;
    if (!user || impl != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    *userId = impl->id;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserFindUserById( IXUserImpl6 *iface, UINT64 userId, XUserHandle *handle )
{
    TRACE( "iface %p, userId %s, handle %p.\n", iface, wine_dbgstr_longlong(userId), handle );

    if (!handle) return E_INVALIDARG;

    if (!default_user || userId != default_user->id)
    {
        *handle = NULL;
        return E_GAMEUSER_USER_NOT_FOUND;
    }

    *handle = (XUserHandle)default_user;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetIsGuest( IXUserImpl6 *iface, XUserHandle user, BOOLEAN *isGuest )
{
    TRACE( "iface %p, user %p, isGuest %p.\n", iface, user, isGuest );

    if (!isGuest) return E_INVALIDARG;
    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    /* AllowGuests is honored at the option-validation level in
     * XUserAddAsync; the synthesized default user itself is never a guest. */
    *isGuest = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserGetState( IXUserImpl6 *iface, XUserHandle user, XUserState *state )
{
    TRACE( "iface %p, user %p, state %p.\n", iface, user, state );

    if (!state) return E_INVALIDARG;
    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    *state = XUserState_SignedIn;
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXUserImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGamerPictureSize pictureSize, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, pictureSize %d, async %p stub!\n", iface, user, pictureSize, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    FIXME( "iface %p, async %p, bufferSize %p stub!\n", iface, async, bufferSize );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetGamerPictureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    FIXME( "iface %p, async %p, bufferSize %Iu, buffer %p, bufferUsed %p stub!\n", iface, async, bufferSize, buffer, bufferUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetAgeGroup( IXUserImpl6 *iface, XUserHandle user, XUserAgeGroup *ageGroup )
{
    TRACE( "iface %p, user %p, ageGroup %p.\n", iface, user, ageGroup );

    if (!ageGroup) return E_INVALIDARG;
    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    /* No real Xbox Live profile/parental-controls data exists on this
     * build; reporting Adult is the documented value that never triggers
     * age-gated content restrictions, which is the safe default for a
     * locally-signed-in single-player user. */
    *ageGroup = XUserAgeGroup_Adult;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserCheckPrivilege( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, BOOLEAN *hasPrivilege, XUserPrivilegeDenyReason *reason )
{
    TRACE( "iface %p, user %p, options %#x, privilege %d, hasPrivilege %p, reason %p.\n",
           iface, user, (unsigned int)options, privilege, hasPrivilege, reason );

    if (!hasPrivilege) return E_INVALIDARG;
    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    /* No real Xbox Live entitlement/parental-controls service exists on
     * this build to evaluate privileges against, so every privilege is
     * granted unconditionally - the safe choice for an offline single local
     * user rather than blocking gameplay on an unimplemented check. */
    *hasPrivilege = TRUE;
    if (reason) *reason = XUserPrivilegeDenyReason_None;
    return S_OK;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiAsync( IXUserImpl6 *iface, XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %d, privilege %d, async %p stub!\n", iface, user, options, privilege, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolvePrivilegeWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

/* Real, cached Xbox Live sign-in state - lazily populated the first time a
 * title actually asks for a signed token (XUserGetTokenAndSignatureAsync),
 * not at XUserAddAsync time (which stays the fast, synchronous, synthetic
 * path above - every previously-tested title's startup sequence depends
 * on that completing instantly, so it's deliberately left untouched).
 * Real network I/O (multiple HTTPS round trips), so this can take real
 * wall-clock time on the very first call - subsequent calls reuse the
 * cached result and just compute a fresh per-request signature. */
static CRITICAL_SECTION real_auth_cs;
static CRITICAL_SECTION_DEBUG real_auth_cs_debug =
{
    0, 0, &real_auth_cs,
    { &real_auth_cs_debug.ProcessLocksList, &real_auth_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": real_auth_cs") }
};
static CRITICAL_SECTION real_auth_cs = { &real_auth_cs_debug, -1, 0, 0, 0, 0 };
static struct sisu_auth_result real_auth;
static BOOL real_auth_attempted;
static HRESULT real_auth_hr = E_FAIL;

/* MSAAppId isn't passed to any XUser* call - real titles carry it in their
 * own MicrosoftGame.config, which real GDK also reads it from. Same
 * locate-next-to-exe-and-grep-a-flat-tag approach as
 * XGameGetXboxTitleId's own MicrosoftGame.config reader in xgame.c
 * (independent copy, not shared, since that one isn't exported from this
 * module). */
static HRESULT get_msa_app_id( char **out )
{
    WCHAR path[MAX_PATH];
    WCHAR *sep;
    HANDLE file;
    DWORD size, read_len;
    char *buf, *tag_start, *tag_end;
    HRESULT hr;

    if (!GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) ) || !(sep = wcsrchr( path, '\\' )))
        return E_FAIL;
    wcscpy( sep + 1, L"MicrosoftGame.config" );

    if ((file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL )) == INVALID_HANDLE_VALUE)
        return E_FAIL;

    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || !size || size > 1024 * 1024)
    {
        CloseHandle( file );
        return E_FAIL;
    }
    if (!(buf = malloc( size + 1 )))
    {
        CloseHandle( file );
        return E_OUTOFMEMORY;
    }
    if (!ReadFile( file, buf, size, &read_len, NULL ))
    {
        free( buf );
        CloseHandle( file );
        return E_FAIL;
    }
    CloseHandle( file );
    buf[read_len] = 0;

    hr = E_FAIL;
    if ((tag_start = strstr( buf, "<MSAAppId>" )))
    {
        tag_start += strlen( "<MSAAppId>" );
        if ((tag_end = strstr( tag_start, "</MSAAppId>" )))
        {
            *tag_end = 0;
            if ((*out = _strdup( tag_start ))) hr = S_OK;
            else hr = E_OUTOFMEMORY;
        }
    }
    free( buf );
    return hr;
}

/* Runs sisu_perform_auth() exactly once per process (real network I/O -
 * multiple HTTPS round trips - so deliberately not repeated), caching
 * either the success or the failure so later calls fail fast instead of
 * retrying a real network operation on every single signed request. */
static HRESULT ensure_real_auth( void )
{
    HRESULT hr;
    char *msaAppId;

    EnterCriticalSection( &real_auth_cs );
    if (real_auth_attempted)
    {
        hr = real_auth_hr;
        LeaveCriticalSection( &real_auth_cs );
        return hr;
    }

    if (FAILED(hr = get_msa_app_id( &msaAppId )))
    {
        WARN( "get_msa_app_id failed, hr %#lx - no <MSAAppId> in this title's MicrosoftGame.config?\n", hr );
    }
    else
    {
        hr = sisu_perform_auth( msaAppId, FALSE, &real_auth );
        if (FAILED(hr)) WARN( "sisu_perform_auth failed, hr %#lx.\n", hr );
        free( msaAppId );
    }

    real_auth_attempted = TRUE;
    real_auth_hr = hr;
    LeaveCriticalSection( &real_auth_cs );
    return hr;
}

/* Stable, unique-per-callsite address used only for pointer-equality
 * identity checks between XAsyncBegin and XAsyncGetResult (a plain static
 * variable, rather than relying on string-literal-pooling giving two
 * separate literals in this file the same address - guaranteed correct
 * either way, since it's the address of one single variable). */
static const char token_sig_identity[] = "XUserGetTokenAndSignatureAsync";

struct token_sig_state
{
    char *method;
    WCHAR *url;
    void *body;
    SIZE_T bodySize;
    char *token;       /* owned, NUL-terminated */
    SIZE_T tokenSize;  /* strlen(token) + 1 */
    char signature[105]; /* 104 real signature bytes + NUL we add ourselves */
    SIZE_T signatureSize;
};

static void free_token_sig_state( struct token_sig_state *state )
{
    if (!state) return;
    free( state->method );
    free( state->url );
    free( state->body );
    free( state->token );
    free( state );
}

static HRESULT CALLBACK token_sig_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct token_sig_state *state = data->context;
    HRESULT hr;

    switch (op)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );

    case XAsyncOp_DoWork:
        if (FAILED(hr = ensure_real_auth()))
        {
            IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, 0 );
            return S_OK;
        }

        {
            /* real_auth.xblAuthHeader is "Authorization: XBL3.0 x=-;<token>" -
             * XUserGetTokenAndSignatureData.token is documented as just the
             * value a caller would put in their own Authorization header,
             * without the header name - skip past "Authorization: ". */
            static const WCHAR prefix[] = L"Authorization: ";
            const WCHAR *tokenW = real_auth.xblAuthHeader + wcslen( prefix );
            int tokenLen = WideCharToMultiByte( CP_UTF8, 0, tokenW, -1, NULL, 0, NULL, NULL );

            if (!tokenLen || !(state->token = malloc( tokenLen )) ||
                !WideCharToMultiByte( CP_UTF8, 0, tokenW, -1, state->token, tokenLen, NULL, NULL ))
            {
                hr = HRESULT_FROM_WIN32( GetLastError() );
                IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, 0 );
                return S_OK;
            }
            state->tokenSize = (SIZE_T)tokenLen;
        }

        hr = sisu_sign_request( real_auth.key, state->method, state->url, state->token, state->bodySize, state->body, state->signature );
        if (SUCCEEDED(hr))
        {
            state->signature[104] = 0;
            state->signatureSize = 105;
        }
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, sizeof(XUserGetTokenAndSignatureData) + state->tokenSize + state->signatureSize );
        return S_OK;

    case XAsyncOp_GetResult:
    {
        XUserGetTokenAndSignatureData *out;
        SIZE_T need = sizeof(*out) + state->tokenSize + state->signatureSize;
        char *strings;

        if (data->bufferSize < need) return E_NOT_SUFFICIENT_BUFFER;
        out = data->buffer;
        strings = (char *)data->buffer + sizeof(*out);
        memcpy( strings, state->token, state->tokenSize );
        memcpy( strings + state->tokenSize, state->signature, state->signatureSize );
        out->tokenSize = state->tokenSize;
        out->signatureSize = state->signatureSize;
        out->token = strings;
        out->signature = strings + state->tokenSize;
        return S_OK;
    }

    case XAsyncOp_Cleanup:
        free_token_sig_state( state );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const char *method, const char *url, SIZE_T headerCount, const XUserGetTokenAndSignatureHttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    struct token_sig_state *state;
    int urlWLen;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, headerCount %Iu, headers %p, bodySize %Iu, bodyBuffer %p, async %p.\n",
           iface, user, options, debugstr_a( method ), debugstr_a( url ), headerCount, headers, bodySize, bodyBuffer, async );

    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;
    if (!method || !url || !async) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;
    if (!(state->method = _strdup( method ))) { free_token_sig_state( state ); return E_OUTOFMEMORY; }
    if (!(urlWLen = MultiByteToWideChar( CP_UTF8, 0, url, -1, NULL, 0 ))) { free_token_sig_state( state ); return HRESULT_FROM_WIN32( GetLastError() ); }
    if (!(state->url = malloc( urlWLen * sizeof(WCHAR) ))) { free_token_sig_state( state ); return E_OUTOFMEMORY; }
    MultiByteToWideChar( CP_UTF8, 0, url, -1, state->url, urlWLen );
    if (bodySize)
    {
        if (!(state->body = malloc( bodySize ))) { free_token_sig_state( state ); return E_OUTOFMEMORY; }
        memcpy( state->body, bodyBuffer, bodySize );
        state->bodySize = bodySize;
    }

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, state, &token_sig_identity, "XUserGetTokenAndSignatureAsync", token_sig_provider )))
        free_token_sig_state( state );
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );

    if (!async || !bufferSize) return E_INVALIDARG;

    /* XAsyncGetResultSize already returns E_PENDING/the real failure HRESULT
     * if the op isn't done or failed, and otherwise the exact
     * requiredBufferSize XAsyncComplete was given - no extra status check
     * needed here. */
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureData **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );

    if (!async || !buffer) return E_INVALIDARG;

    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &token_sig_identity, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
    return hr;
}

static const char token_sig_utf16_identity[] = "XUserGetTokenAndSignatureUtf16Async";

struct x_user_token_sig_utf16_state
{
    WCHAR *token;
    SIZE_T tokenSize;
    WCHAR *signature;
    SIZE_T signatureSize;
};

static HRESULT CALLBACK x_user_token_sig_utf16_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct x_user_token_sig_utf16_state *state = data->context;

    switch (op)
    {
    case XAsyncOp_Begin:
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(XUserGetTokenAndSignatureUtf16Data) + state->tokenSize + state->signatureSize );
        return S_OK;

    case XAsyncOp_GetResult:
    {
        XUserGetTokenAndSignatureUtf16Data *out;
        SIZE_T need = sizeof(*out) + state->tokenSize + state->signatureSize;
        WCHAR *strings;

        if (data->bufferSize < need) return E_NOT_SUFFICIENT_BUFFER;
        out = data->buffer;
        strings = (WCHAR *)((char *)data->buffer + sizeof(*out));
        memcpy( strings, state->token, state->tokenSize );
        memcpy( (char *)strings + state->tokenSize, state->signature, state->signatureSize );
        out->tokenCount = state->tokenSize / sizeof(WCHAR);
        out->signatureCount = state->signatureSize / sizeof(WCHAR);
        out->token = strings;
        out->signature = (WCHAR *)((char *)strings + state->tokenSize);
        return S_OK;
    }

    case XAsyncOp_Cleanup:
        free( state->token );
        free( state->signature );
        free( state );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Async( IXUserImpl6 *iface, XUserHandle user, XUserGetTokenAndSignatureOptions options, const WCHAR *method, const WCHAR *url, SIZE_T headerCount, const XUserGetTokenAndSignatureUtf16HttpHeader *headers, SIZE_T bodySize, const void *bodyBuffer, XAsyncBlock *async )
{
    struct x_user_token_sig_utf16_state *state;
    HRESULT hr;

    TRACE( "iface %p, user %p, options %d, method %s, url %s, headerCount %Iu, headers %p, bodySize %Iu, bodyBuffer %p, async %p\n", iface, user, options, debugstr_w( method ), debugstr_w( url ), headerCount, headers, bodySize, bodyBuffer, async );
    if (!async) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;

    state->token = _wcsdup( L"Bearer MockToken" );
    state->tokenSize = (wcslen( state->token ) + 1) * sizeof(WCHAR);
    state->signature = _wcsdup( L"MockSignature" );
    state->signatureSize = (wcslen( state->signature ) + 1) * sizeof(WCHAR);

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, state, &token_sig_utf16_identity, "XUserGetTokenAndSignatureUtf16Async", x_user_token_sig_utf16_provider )))
    {
        free( state->token );
        free( state->signature );
        free( state );
    }
    return hr;
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16ResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *bufferSize )
{
    TRACE( "iface %p, async %p, bufferSize %p.\n", iface, async, bufferSize );
    if (!async || !bufferSize) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, bufferSize );
}

static HRESULT WINAPI x_user_XUserGetTokenAndSignatureUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T bufferSize, void *buffer, XUserGetTokenAndSignatureUtf16Data **ptrToBuffer, SIZE_T *bufferUsed )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, bufferSize %Iu, buffer %p, ptrToBuffer %p, bufferUsed %p.\n", iface, async, bufferSize, buffer, ptrToBuffer, bufferUsed );
    if (!async || !buffer) return E_INVALIDARG;

    hr = IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &token_sig_utf16_identity, bufferSize, buffer, bufferUsed );
    if (SUCCEEDED(hr) && ptrToBuffer) *ptrToBuffer = buffer;
    return hr;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiAsync( IXUserImpl6 *iface, XUserHandle user, const char *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_a( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Async( IXUserImpl6 *iface, XUserHandle user, const WCHAR *url, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, url %s, async %p stub!\n", iface, user, debugstr_w( url ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserResolveIssueWithUiUtf16Result( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserRegisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueHandle queue, void *context, XUserChangeEventCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_user_XUserUnregisterForChangeEvent( IXUserImpl6 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, token %llu, wait %d.\n", iface, (unsigned long long)token.token, wait );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserGetSignOutDeferral( IXUserImpl6 *iface, XUserSignOutDeferralHandle *deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
    *deferral = NULL;
    return E_GAMEUSER_DEFERRAL_NOT_AVAILABLE;
}

static void WINAPI x_user_XUserCloseSignOutDeferralHandle( IXUserImpl6 *iface, XUserSignOutDeferralHandle deferral )
{
    TRACE( "iface %p, deferral %p.\n", iface, deferral );
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiAsync( IXUserImpl6 *iface, UINT64 userId, XAsyncBlock *async )
{
    FIXME( "iface %p, userId %llu, async %p stub!\n", iface, userId, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserAddByIdWithUiResult( IXUserImpl6 *iface, XAsyncBlock *async, XUserHandle *newUser )
{
    FIXME( "iface %p, async %p, newUser %p stub!\n", iface, async, newUser );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyAsync( IXUserImpl6 *iface, XUserHandle user, XUserGetMsaTokenSilentlyOptions options, const char *scope, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, options %u, scope %s, async %p stub!\n", iface, user, options, debugstr_a( scope ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResult( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T resultTokenSize, char *resultToken, SIZE_T *resultTokenUsed )
{
    FIXME( "iface %p, async %p, resultTokenSize %Iu, resultToken %p, resultTokenUsed %p stub!\n", iface, async, resultTokenSize, resultToken, resultTokenUsed );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserGetMsaTokenSilentlyResultSize( IXUserImpl6 *iface, XAsyncBlock *async, SIZE_T *tokenSize )
{
    FIXME( "iface %p, async %p, tokenSize %p stub!\n", iface, async, tokenSize );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsStoreUser( IXUserImpl6 *iface, XUserHandle user )
{
    FIXME( "iface %p, user %p stub!\n", iface, user );
    return TRUE;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformRemoteConnectEventHandlers *handlers )
{
    FIXME( "iface %p, queue %p, handlers %p stub!\n", iface, queue, handlers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformRemoteConnectCancelPrompt( IXUserImpl6 *iface, XUserPlatformOperation operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptSetEventHandlers( IXUserImpl6 *iface, XTaskQueueHandle queue, XUserPlatformSpopPromptEventHandler *handler, void *context )
{
    FIXME( "iface %p, queue %p, handler %p, context %p stub!\n", iface, queue, handler, context );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserPlatformSpopPromptComplete( IXUserImpl6 *iface, XUserPlatformOperation operation, XUserPlatformOperationResult result )
{
    FIXME( "iface %p, operation %p, result %d stub!\n", iface, operation, result );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_XUserIsSignOutPresent( IXUserImpl6 *iface )
{
    TRACE( "iface %p.\n", iface );
    return FALSE;
}

static HRESULT WINAPI x_user_XUserSignOutAsync( IXUserImpl6 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_XUserSignOutResult( IXUserImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXUserImpl6Vtbl x_user_vtbl =
{
    x_user_QueryInterface,
    x_user_AddRef,
    x_user_Release,
    /* IXUserImpl methods */
    x_user_XUserDuplicateHandle,
    x_user_XUserCloseHandle,
    x_user_XUserCompare,
    x_user_XUserGetMaxUsers,
    x_user_XUserAddAsync,
    x_user_XUserAddResult,
    x_user_XUserGetLocalId,
    x_user_XUserFindUserByLocalId,
    x_user_XUserGetId,
    x_user_XUserFindUserById,
    x_user_XUserGetIsGuest,
    x_user_XUserGetState,
    __PADDING__,
    x_user_XUserGetGamerPictureAsync,
    x_user_XUserGetGamerPictureResultSize,
    x_user_XUserGetGamerPictureResult,
    x_user_XUserGetAgeGroup,
    x_user_XUserCheckPrivilege,
    x_user_XUserResolvePrivilegeWithUiAsync,
    x_user_XUserResolvePrivilegeWithUiResult,
    x_user_XUserGetTokenAndSignatureAsync,
    x_user_XUserGetTokenAndSignatureResultSize,
    x_user_XUserGetTokenAndSignatureResult,
    x_user_XUserGetTokenAndSignatureUtf16Async,
    x_user_XUserGetTokenAndSignatureUtf16ResultSize,
    x_user_XUserGetTokenAndSignatureUtf16Result,
    x_user_XUserResolveIssueWithUiAsync,
    x_user_XUserResolveIssueWithUiResult,
    x_user_XUserResolveIssueWithUiUtf16Async,
    x_user_XUserResolveIssueWithUiUtf16Result,
    x_user_XUserRegisterForChangeEvent,
    x_user_XUserUnregisterForChangeEvent,
    x_user_XUserGetSignOutDeferral,
    x_user_XUserCloseSignOutDeferralHandle,
    /* IXUserImpl2 methods */
    x_user_XUserAddByIdWithUiAsync,
    x_user_XUserAddByIdWithUiResult,
    /* IXUserImpl3 methods */
    x_user_XUserGetMsaTokenSilentlyAsync,
    x_user_XUserGetMsaTokenSilentlyResult,
    x_user_XUserGetMsaTokenSilentlyResultSize,
    /* IXUserImpl4 methods */
    x_user_XUserIsStoreUser,
    /* IXUserImpl5 methods */
    x_user_XUserPlatformRemoteConnectSetEventHandlers,
    x_user_XUserPlatformRemoteConnectCancelPrompt,
    x_user_XUserPlatformSpopPromptSetEventHandlers,
    x_user_XUserPlatformSpopPromptComplete,
    /* IXUserImpl6 methods */
    x_user_XUserIsSignOutPresent,
    x_user_XUserSignOutAsync,
    x_user_XUserSignOutResult,
};

static inline struct x_user *impl_from_IXUserGamertagImpl( IXUserGamertagImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserGamertagImpl_iface );
}

static HRESULT WINAPI x_user_gamertag_QueryInterface( IXUserGamertagImpl *iface, REFIID riid, void **out )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_QueryInterface( &impl->IXUserImpl6_iface, riid, out );
}

static ULONG WINAPI x_user_gamertag_AddRef( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_gamertag_Release( IXUserGamertagImpl *iface )
{
    struct x_user *impl = impl_from_IXUserGamertagImpl( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_gamertag_XUserGetGamertag( IXUserGamertagImpl *iface, XUserHandle user, XUserGamertagComponent gamertagComponent, SIZE_T gamertagSize, char *gamertag, SIZE_T *gamertagUsed )
{
    static const char name[] = "Player";
    SIZE_T len = sizeof(name);

    TRACE( "iface %p, user %p, gamertagComponent %d, gamertagSize %Iu, gamertag %p, gamertagUsed %p.\n",
           iface, user, gamertagComponent, gamertagSize, gamertag, gamertagUsed );

    if (!user || (struct x_user_data *)user != default_user) return E_GAMEUSER_USER_NOT_FOUND;

    if (gamertagUsed) *gamertagUsed = len;
    if (!gamertag) return E_INVALIDARG;
    if (gamertagSize < len) return E_NOT_SUFFICIENT_BUFFER;

    /* No real Xbox Live profile exists on this build to source a gamertag
     * from; the same placeholder is returned for every documented
     * component (Classic/Modern/ModernSuffix/UniqueModern) - titles that
     * only display the string work, at the cost of not distinguishing
     * components that real accounts would render differently. */
    memcpy( gamertag, name, len );
    return S_OK;
}

static const struct IXUserGamertagImplVtbl x_user_gamertag_vtbl =
{
    x_user_gamertag_QueryInterface,
    x_user_gamertag_AddRef,
    x_user_gamertag_Release,
    /* IXUserGamertag methods */
    x_user_gamertag_XUserGetGamertag,
};

static inline struct x_user *impl_from_IXUserDeviceImpl2( IXUserDeviceImpl2 *iface )
{
    return CONTAINING_RECORD( iface, struct x_user, IXUserDeviceImpl2_iface );
}

static HRESULT WINAPI x_user_device_QueryInterface( IXUserDeviceImpl2 *iface, REFIID iid, void **out )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown          ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl  ) ||
        IsEqualGUID( iid, &IID_IXUserDeviceImpl2 ))
    {
        IXUserDeviceImpl2_AddRef( *out = &impl->IXUserDeviceImpl2_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_user_device_AddRef( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_AddRef( &impl->IXUserImpl6_iface );
}

static ULONG WINAPI x_user_device_Release( IXUserDeviceImpl2 *iface )
{
    struct x_user *impl = impl_from_IXUserDeviceImpl2( iface );
    return IXUserImpl6_Release( &impl->IXUserImpl6_iface );
}

static HRESULT WINAPI x_user_device_XUserFindForDevice( IXUserDeviceImpl2 *iface, const APP_LOCAL_DEVICE_ID *deviceId, XUserHandle *handle )
{
    FIXME( "iface %p, deviceId %p, handle %p stub!\n", iface, deviceId, handle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDeviceAssociationChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDeviceAssociationChanged( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserGetDefaultAudioEndpointUtf16( IXUserDeviceImpl2 *iface, XUserLocalId user, XUserDefaultAudioEndpointKind defaultAudioEndpointKind, SIZE_T endpointIdUtf16Count, WCHAR *endpointIdUtf16, SIZE_T *endpointIdUtf16Used )
{
    FIXME( "iface %p, user %p, defaultAudioEndpointKind %d, endpointIdUtf16Count %Iu, endpointIdUtf16 %p, endpointIdUtf16Used %p stub!\n", iface, &user, defaultAudioEndpointKind, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueHandle queue, void *context, XUserDefaultAudioEndpointUtf16ChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, context %p, callback %p, token %p stub!\n", iface, queue, context, callback, token );
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed( IXUserDeviceImpl2 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    FIXME( "iface %p, token %p, wait %d stub!\n", iface, &token, wait );
    return FALSE;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync( IXUserDeviceImpl2 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiResult( IXUserDeviceImpl2 *iface, XAsyncBlock *async, APP_LOCAL_DEVICE_ID *deviceId )
{
    FIXME( "iface %p, async %p, deviceId %p stub!\n", iface, async, deviceId );
    return E_NOTIMPL;
}

static const struct IXUserDeviceImpl2Vtbl x_user_device_vtbl =
{
    x_user_device_QueryInterface,
    x_user_device_AddRef,
    x_user_device_Release,
    /* IXUserDeviceImpl/IXUserDeviceImpl2 methods */
    x_user_device_XUserFindForDevice,
    x_user_device_XUserRegisterForDeviceAssociationChanged,
    x_user_device_XUserUnregisterForDeviceAssociationChanged,
    x_user_device_XUserGetDefaultAudioEndpointUtf16,
    x_user_device_XUserRegisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    x_user_device_XUserFindControllerForUserWithUiAsync,
    x_user_device_XUserFindControllerForUserWithUiResult,
};

static struct x_user x_user =
{
    {&x_user_vtbl},
    {&x_user_gamertag_vtbl},
    {&x_user_device_vtbl},
    0,
};

IXUserImpl *x_user_impl = (IXUserImpl *)&x_user.IXUserImpl6_iface;
IXUserDeviceImpl *x_user_device_impl = (IXUserDeviceImpl *)&x_user.IXUserDeviceImpl2_iface;
