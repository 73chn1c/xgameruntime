/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameSave and XGameSaveFiles
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

struct x_game_save
{
    IXGameSaveImpl3 IXGameSaveImpl3_iface;
    LONG ref;
};

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static inline struct x_game_save *impl_from_IXGameSaveImpl3( IXGameSaveImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_save, IXGameSaveImpl3_iface );
}

static HRESULT WINAPI x_game_save_QueryInterface( IXGameSaveImpl3 *iface, REFIID iid, void **out )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl3 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl4 ))
    {
        IXGameSaveImpl_AddRef( *out = &impl->IXGameSaveImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_save_AddRef( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_save_Release( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static const char x_game_save_init_identity[] = "XGameSaveInitializeProviderAsync";
static const char x_game_save_quota_identity[] = "XGameSaveGetRemainingQuotaAsync";
static const char x_game_save_submit_identity[] = "XGameSaveSubmitUpdateAsync";
static const char x_game_save_delete_identity[] = "XGameSaveDeleteContainerAsync";
static const char x_game_save_read_identity[] = "XGameSaveReadBlobDataAsync";
static const char x_game_save_folder_identity[] = "XGameSaveFilesGetFolderWithUiAsync";

struct x_game_save_init_state
{
    XGameSaveProviderHandle provider;
};

static HRESULT CALLBACK x_game_save_init_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct x_game_save_init_state *state = data->context;

    switch (op)
    {
    case XAsyncOp_Begin:
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(XGameSaveProviderHandle) );
        return S_OK;

    case XAsyncOp_GetResult:
        if (data->bufferSize < sizeof(XGameSaveProviderHandle)) return E_NOT_SUFFICIENT_BUFFER;
        *(XGameSaveProviderHandle *)data->buffer = state->provider;
        return S_OK;

    case XAsyncOp_Cleanup:
        free( state );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT CALLBACK x_game_save_quota_cb( XAsyncOp op, const XAsyncProviderData *data )
{
    INT64 *quota = data->context;

    switch (op)
    {
    case XAsyncOp_Begin:
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, sizeof(INT64) );
        return S_OK;

    case XAsyncOp_GetResult:
        if (data->bufferSize < sizeof(INT64)) return E_NOT_SUFFICIENT_BUFFER;
        *(INT64 *)data->buffer = *quota;
        return S_OK;

    case XAsyncOp_Cleanup:
        free( quota );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT CALLBACK x_game_save_generic_cb( XAsyncOp op, const XAsyncProviderData *data )
{
    switch (op)
    {
    case XAsyncOp_Begin:
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, S_OK, 0 );
        return S_OK;

    default:
        return S_OK;
    }
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProvider( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, provider %p\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, provider );
    if (!provider) return E_POINTER;
    *provider = (XGameSaveProviderHandle)0x3001;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XAsyncBlock *async )
{
    struct x_game_save_init_state *state;
    HRESULT hr;

    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, async %p\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, async );
    if (!async) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;
    state->provider = (XGameSaveProviderHandle)0x3001;

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, state, &x_game_save_init_identity, "XGameSaveInitializeProviderAsync", x_game_save_init_provider )))
        free( state );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, async %p, provider %p\n", iface, async, provider );
    if (!async || !provider) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_init_identity, sizeof(*provider), provider, NULL );
}

static void WINAPI x_game_save_XGameSaveCloseProvider( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider )
{
    TRACE( "iface %p, provider %p\n", iface, provider );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuota( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, INT64 *remainingQuota )
{
    TRACE( "iface %p, provider %p, remainingQuota %p\n", iface, provider, remainingQuota );
    if (!remainingQuota) return E_POINTER;
    *remainingQuota = 1024LL * 1024LL * 1024LL * 10LL; // 10 GB
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, XAsyncBlock *async )
{
    INT64 *state;
    HRESULT hr;

    TRACE( "iface %p, provider %p, async %p\n", iface, provider, async );
    if (!async) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;
    *state = 1024LL * 1024LL * 1024LL * 10LL;

    if (FAILED(hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, state, &x_game_save_quota_identity, "XGameSaveGetRemainingQuotaAsync", x_game_save_quota_cb )))
        free( state );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, INT64 *remainingQuota )
{
    TRACE( "iface %p, async %p, remainingQuota %p\n", iface, async, remainingQuota );
    if (!async || !remainingQuota) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_quota_identity, sizeof(*remainingQuota), remainingQuota, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName )
{
    TRACE( "iface %p, provider %p, containerName %s\n", iface, provider, debugstr_a( containerName ) );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XAsyncBlock *async )
{
    TRACE( "iface %p, provider %p, containerName %s, async %p\n", iface, provider, debugstr_a( containerName ), async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, &x_game_save_delete_identity, "XGameSaveDeleteContainerAsync", x_game_save_generic_cb );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p\n", iface, async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_delete_identity, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveGetContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerName %s, context %p, callback %p\n", iface, provider, debugstr_a( containerName ), context, callback );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, context %p, callback %p\n", iface, provider, context, callback );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfoByName( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerNamePrefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerNamePrefix %s, context %p, callback %p\n", iface, provider, debugstr_a( containerNamePrefix ), context, callback );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveCreateContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XGameSaveContainerHandle *containerContext )
{
    TRACE( "iface %p, provider %p, containerName %s, containerContext %p\n", iface, provider, debugstr_a( containerName ), containerContext );
    if (!containerContext) return E_POINTER;
    *containerContext = (XGameSaveContainerHandle)0x4001;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseContainer( IXGameSaveImpl3 *iface, XGameSaveContainerHandle context )
{
    TRACE( "iface %p, context %p\n", iface, context );
}

static void get_saves_directory( WCHAR *outPath )
{
    WCHAR userProfile[MAX_PATH];
    if (!GetEnvironmentVariableW( L"USERPROFILE", userProfile, MAX_PATH ))
        wcscpy( userProfile, L"C:\\users\\steamuser" );

    swprintf( outPath, MAX_PATH, L"%s\\Documents\\My Games\\Fallout4 MS\\Saves", userProfile );
    CreateDirectoryW( outPath, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfo( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, void *context, XGameSaveBlobInfoCallback *callback )
{
    WCHAR dirPath[MAX_PATH];
    WCHAR searchPath[MAX_PATH];
    WIN32_FIND_DATAW findData;
    HANDLE hFind;

    TRACE( "iface %p, container %p, context %p, callback %p\n", iface, container, context, callback );
    if (!callback) return E_INVALIDARG;

    get_saves_directory( dirPath );
    swprintf( searchPath, MAX_PATH, L"%s\\*", dirPath );

    hFind = FindFirstFileW( searchPath, &findData );
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                char nameA[MAX_PATH];
                XGameSaveBlobInfo info;

                WideCharToMultiByte( CP_UTF8, 0, findData.cFileName, -1, nameA, MAX_PATH, NULL, NULL );
                info.name = nameA;
                info.size = findData.nFileSizeLow;

                if (!callback( &info, context )) break;
            }
        } while (FindNextFileW( hFind, &findData ));
        FindClose( hFind );
    }

    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfoByName( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *blobNamePrefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, blobNamePrefix %s, context %p, callback %p\n", iface, container, debugstr_a( blobNamePrefix ), context, callback );
    return x_game_save_XGameSaveEnumerateBlobInfo( iface, container, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobData( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 *countOfBlobs, SIZE_T blobsSize, XGameSaveBlob *blobData )
{
    WCHAR dirPath[MAX_PATH];
    WCHAR filePath[MAX_PATH];
    WCHAR blobNameW[MAX_PATH];
    HANDLE hFile;
    DWORD readBytes;
    UINT32 i;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %p, blobsSize %Iu, blobData %p\n", iface, container, blobNames, countOfBlobs, blobsSize, blobData );

    if (!blobNames || !countOfBlobs || !blobData) return E_INVALIDARG;

    get_saves_directory( dirPath );

    for (i = 0; i < *countOfBlobs; i++)
    {
        MultiByteToWideChar( CP_UTF8, 0, blobNames[i], -1, blobNameW, MAX_PATH );
        swprintf( filePath, MAX_PATH, L"%s\\%s", dirPath, blobNameW );

        hFile = CreateFileW( filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        if (hFile != INVALID_HANDLE_VALUE)
        {
            DWORD size = GetFileSize( hFile, NULL );
            if (blobData[i].data && blobData[i].info.size >= size)
            {
                ReadFile( hFile, blobData[i].data, size, &readBytes, NULL );
                blobData[i].info.size = readBytes;
            }
            CloseHandle( hFile );
        }
    }

    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataAsync( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 countOfBlobs, XAsyncBlock *async )
{
    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %u, async %p\n", iface, container, blobNames, countOfBlobs, async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, &x_game_save_read_identity, "XGameSaveReadBlobDataAsync", x_game_save_generic_cb );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T blobsSize, XGameSaveBlob *blobData, UINT32 *countOfBlobs )
{
    TRACE( "iface %p, async %p, blobsSize %Iu, blobData %p, countOfBlobs %p\n", iface, async, blobsSize, blobData, countOfBlobs );
    if (countOfBlobs) *countOfBlobs = 0;
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_read_identity, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateUpdate( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *containerDisplayName, XGameSaveUpdateHandle *updateContext )
{
    TRACE( "iface %p, container %p, containerDisplayName %s, updateContext %p\n", iface, container, debugstr_a( containerDisplayName ), updateContext );
    if (!updateContext) return E_POINTER;
    *updateContext = (XGameSaveUpdateHandle)0x5001;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle context )
{
    TRACE( "iface %p, context %p\n", iface, context );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobWrite( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName, UINT8 *data, SIZE_T byteCount )
{
    WCHAR dirPath[MAX_PATH];
    WCHAR filePath[MAX_PATH];
    WCHAR blobNameW[MAX_PATH];
    HANDLE hFile;
    DWORD written;

    TRACE( "iface %p, updateContext %p, blobName %s, data %p, byteCount %Iu\n", iface, updateContext, debugstr_a( blobName ), data, byteCount );

    if (!blobName || !data) return E_INVALIDARG;

    get_saves_directory( dirPath );
    MultiByteToWideChar( CP_UTF8, 0, blobName, -1, blobNameW, MAX_PATH );
    swprintf( filePath, MAX_PATH, L"%s\\%s", dirPath, blobNameW );

    hFile = CreateFileW( filePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile( hFile, data, (DWORD)byteCount, &written, NULL );
        CloseHandle( hFile );
    }

    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobDelete( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName )
{
    WCHAR dirPath[MAX_PATH];
    WCHAR filePath[MAX_PATH];
    WCHAR blobNameW[MAX_PATH];

    TRACE( "iface %p, updateContext %p, blobName %s\n", iface, updateContext, debugstr_a( blobName ) );

    if (!blobName) return E_INVALIDARG;

    get_saves_directory( dirPath );
    MultiByteToWideChar( CP_UTF8, 0, blobName, -1, blobNameW, MAX_PATH );
    swprintf( filePath, MAX_PATH, L"%s\\%s", dirPath, blobNameW );
    DeleteFileW( filePath );

    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext )
{
    TRACE( "iface %p, updateContext %p\n", iface, updateContext );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateAsync( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, XAsyncBlock *async )
{
    TRACE( "iface %p, updateContext %p, async %p\n", iface, updateContext, async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, &x_game_save_submit_identity, "XGameSaveSubmitUpdateAsync", x_game_save_generic_cb );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    TRACE( "iface %p, async %p\n", iface, async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_submit_identity, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, XAsyncBlock *async )
{
    TRACE( "iface %p, requestingUser %p, configurationId %s, async %p\n", iface, requestingUser, debugstr_a( configurationId ), async );
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncBegin( x_threading_impl, async, NULL, &x_game_save_folder_identity, "XGameSaveFilesGetFolderWithUiAsync", x_game_save_generic_cb );
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    TRACE( "iface %p, async %p, folderSize %Iu, folderResult %p\n", iface, async, folderSize, folderResult );
    if (folderResult && folderSize > 0) folderResult[0] = '\0';
    if (!async) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &x_game_save_folder_identity, 0, NULL, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl3 *iface, XUserHandle userContext, const char *configurationId, INT64 *remainingQuota )
{
    TRACE( "iface %p, userContext %p, configurationId %s, remainingQuota %p\n", iface, userContext, debugstr_a( configurationId ), remainingQuota );
    if (remainingQuota) *remainingQuota = 1024LL * 1024LL * 1024LL * 10LL;
    return S_OK;
}

static const struct IXGameSaveImpl3Vtbl x_game_save_vtbl =
{
    x_game_save_QueryInterface,
    x_game_save_AddRef,
    x_game_save_Release,
    /* IXGameSaveImpl methods */
    x_game_save_XGameSaveInitializeProvider,
    x_game_save_XGameSaveInitializeProviderAsync,
    x_game_save_XGameSaveInitializeProviderResult,
    x_game_save_XGameSaveCloseProvider,
    x_game_save_XGameSaveGetRemainingQuota,
    x_game_save_XGameSaveGetRemainingQuotaAsync,
    x_game_save_XGameSaveGetRemainingQuotaResult,
    x_game_save_XGameSaveDeleteContainer,
    x_game_save_XGameSaveDeleteContainerAsync,
    x_game_save_XGameSaveDeleteContainerResult,
    x_game_save_XGameSaveGetContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfoByName,
    x_game_save_XGameSaveCreateContainer,
    x_game_save_XGameSaveCloseContainer,
    x_game_save_XGameSaveEnumerateBlobInfo,
    x_game_save_XGameSaveEnumerateBlobInfoByName,
    x_game_save_XGameSaveReadBlobData,
    x_game_save_XGameSaveReadBlobDataAsync,
    x_game_save_XGameSaveReadBlobDataResult,
    x_game_save_XGameSaveCreateUpdate,
    x_game_save_XGameSaveCloseUpdate,
    x_game_save_XGameSaveSubmitBlobWrite,
    x_game_save_XGameSaveSubmitBlobDelete,
    x_game_save_XGameSaveSubmitUpdate,
    x_game_save_XGameSaveSubmitUpdateAsync,
    x_game_save_XGameSaveSubmitUpdateResult,
    /* IXGameSaveImpl2 methods */
    x_game_save_XGameSaveFilesGetFolderWithUiAsync,
    x_game_save_XGameSaveFilesGetFolderWithUiResult,
    /* IXGameSaveImpl3 methods */
    x_game_save_XGameSaveFilesGetRemainingQuota,
};

static struct x_game_save x_game_save =
{
    {&x_game_save_vtbl},
    0,
};

IXGameSaveImpl *x_game_save_impl = (IXGameSaveImpl *)&x_game_save.IXGameSaveImpl3_iface;
