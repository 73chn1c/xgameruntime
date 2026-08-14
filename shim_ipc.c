/*
 * IPC client for the "shim" - see shim_ipc.h for why this connects over
 * loopback TCP/winsock rather than spawning a child process.
 */

#include "private.h"
#include "shim_ipc.h"
#include <winsock2.h>
#include <ws2tcpip.h>

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static BOOL socket_send_full( SOCKET s, const void *buf, int len )
{
    const char *ptr = buf;
    int sent;

    while (len > 0)
    {
        sent = send( s, ptr, len, 0 );
        if (sent <= 0) return FALSE;
        ptr += sent;
        len -= sent;
    }
    return TRUE;
}

static BOOL socket_recv_full( SOCKET s, void *buf, int len )
{
    char *ptr = buf;
    int got;

    while (len > 0)
    {
        got = recv( s, ptr, len, 0 );
        if (got <= 0) return FALSE;
        ptr += got;
        len -= got;
    }
    return TRUE;
}

HRESULT shim_start( struct shim_channel *shim, const WCHAR *envVar )
{
    struct sockaddr_in addr = { 0 };
    WSADATA wsaData;
    SOCKET sock;

    TRACE( "shim %p, envVar %s (unused - always connects to 127.0.0.1:%d).\n", shim, debugstr_w( envVar ), SHIM_TCP_PORT );

    if (WSAStartup( MAKEWORD(2, 2), &wsaData )) return HRESULT_FROM_WIN32( WSAGetLastError() );

    if ((sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) == INVALID_SOCKET)
    {
        WSACleanup();
        return HRESULT_FROM_WIN32( WSAGetLastError() );
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons( SHIM_TCP_PORT );
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == SOCKET_ERROR)
    {
        HRESULT hr = HRESULT_FROM_WIN32( WSAGetLastError() );
        ERR( "connect() to 127.0.0.1:%d failed, hr %#lx - is xodus-service running?\n", SHIM_TCP_PORT, hr );
        closesocket( sock );
        WSACleanup();
        return hr;
    }

    InitializeCriticalSection( &shim->lock );
    shim->sock = sock;
    shim->active = TRUE;
    return S_OK;
}

void shim_stop( struct shim_channel *shim )
{
    if (!shim->active) return;

    TRACE( "shim %p.\n", shim );

    closesocket( shim->sock );
    WSACleanup();
    DeleteCriticalSection( &shim->lock );
    shim->active = FALSE;
}

HRESULT shim_call( struct shim_channel *shim, UINT16 type, const char *payload, UINT16 payloadLen,
                    UINT16 *respType, char **resp, UINT16 *respLen )
{
    struct shim_header hdr = { XML_MAGIC, type, payloadLen };
    struct shim_header rhdr;
    HRESULT hr = S_OK;

    TRACE( "shim %p, type %u, payload %p, payloadLen %u.\n", shim, type, payload, payloadLen );

    EnterCriticalSection( &shim->lock );

    if (!shim->active)
    {
        hr = E_ABORT;
        goto done;
    }

    if (!socket_send_full( shim->sock, &hdr, sizeof(hdr) ) ||
        (payloadLen && !socket_send_full( shim->sock, payload, payloadLen )))
    {
        int wsaErr = WSAGetLastError();
        ERR( "socket_send_full failed, WSAGetLastError %d.\n", wsaErr );
        shim->active = FALSE;
        hr = HRESULT_FROM_WIN32( wsaErr );
        goto done;
    }

    if (!socket_recv_full( shim->sock, &rhdr, sizeof(rhdr) ))
    {
        ERR( "socket_recv_full (header) failed, WSAGetLastError %d.\n", WSAGetLastError() );
        shim->active = FALSE;
        hr = E_FAIL;
        goto done;
    }
    if (rhdr.magic != XML_MAGIC)
    {
        ERR( "bad response magic %#x (expected %#x).\n", rhdr.magic, XML_MAGIC );
        shim->active = FALSE;
        hr = E_FAIL;
        goto done;
    }

    if (!(*resp = calloc( 1, (SIZE_T)rhdr.length + 1 )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (rhdr.length && !socket_recv_full( shim->sock, *resp, rhdr.length ))
    {
        free( *resp );
        *resp = NULL;
        shim->active = FALSE;
        hr = E_FAIL;
        goto done;
    }
    *respType = rhdr.type;
    *respLen = rhdr.length;

done:
    LeaveCriticalSection( &shim->lock );
    return hr;
}
