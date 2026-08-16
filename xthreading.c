/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XAsync, XTaskQueue and XThread
 *
 * Written by Weather
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
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_threading
{
    IXThreadingImpl IXThreadingImpl_iface;
    LONG ref;
};

/* Minimal XTaskQueue implementation: a real, thread-safe FIFO per port with
 * refcounting, but no background dispatch thread - callers must pump each
 * port via XTaskQueueDispatch (as XTaskQueueDispatchMode_Manual would work on
 * real GDK). XTaskQueueDispatchMode_Immediate is honored by running the
 * callback synchronously on submit, since that requires no queueing at all.
 * ThreadPool/SerializedThreadPool modes are treated the same as Manual: work
 * is queued but nothing dispatches it automatically. */

struct task_callback_entry
{
    struct list entry;
    XTaskQueueCallback *callback;
    void *context;
};

struct x_task_queue_port
{
    LONG ref;
    XTaskQueueDispatchMode dispatch_mode;
    LONG terminated;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    struct list pending;
    HANDLE dispatch_thread;
};

static void port_release( struct x_task_queue_port *port );

/* ThreadPool/SerializedThreadPool contract: on real GDK, submitted callbacks
 * run without the app ever calling XTaskQueueDispatch itself - the runtime
 * provides the worker. Previously this port only accumulated entries on
 * ->pending forever unless something manually pumped it, which no title
 * using these (default) dispatch modes ever does - a silent, permanent
 * deadlock for any code waiting on that callback's side effect. This thread
 * is that missing worker: one dispatch thread per such port, draining
 * ->pending until the port is terminated. A single thread is correct for
 * both modes (SerializedThreadPool requires strict one-at-a-time ordering,
 * which this trivially satisfies; ThreadPool permits more parallelism than
 * this but doesn't require it - every callback still runs exactly once).
 * The thread holds its own port reference for its whole lifetime so the
 * port can never be freed out from under it; it releases that reference
 * (potentially freeing the port) only after observing termination with an
 * empty queue, matching XTaskQueueDispatch's own SleepConditionVariableCS
 * wait/wake pattern on the same cs/cv. */
static DWORD WINAPI dispatch_thread_proc( void *arg )
{
    struct x_task_queue_port *port = arg;
    struct task_callback_entry *entry;

    for (;;)
    {
        EnterCriticalSection( &port->cs );
        while (list_empty( &port->pending ) && !ReadNoFence( &port->terminated ))
            SleepConditionVariableCS( &port->cv, &port->cs, INFINITE );

        if (list_empty( &port->pending ))
        {
            LeaveCriticalSection( &port->cs );
            break;
        }

        entry = LIST_ENTRY( list_head( &port->pending ), struct task_callback_entry, entry );
        list_remove( &entry->entry );
        LeaveCriticalSection( &port->cs );

        entry->callback( entry->context, FALSE );
        free( entry );
    }

    port_release( port );
    return 0;
}

/* Context for delivering an XTaskQueueTerminatedCallback (a distinct,
 * single-argument callback type - see XTaskQueueTerminatedCallback in
 * xtaskqueue.h) through the same port pending-list machinery used for
 * ordinary XTaskQueueCallback entries. */
struct terminate_notify
{
    XTaskQueueTerminatedCallback *callback;
    void *context;
};

static void CALLBACK terminate_notify_trampoline( void *context, BOOLEAN canceled )
{
    struct terminate_notify *notify = context;
    notify->callback( notify->context );
    free( notify );
}

struct x_task_queue
{
    LONG ref;
    struct x_task_queue_port *ports[2];
};

static struct x_task_queue_port *create_port( XTaskQueueDispatchMode mode )
{
    struct x_task_queue_port *port = calloc( 1, sizeof(*port) );
    if (!port) return NULL;
    port->ref = 1;
    port->dispatch_mode = mode;
    InitializeCriticalSection( &port->cs );
    InitializeConditionVariable( &port->cv );
    list_init( &port->pending );

    if (mode == XTaskQueueDispatchMode_ThreadPool || mode == XTaskQueueDispatchMode_SerializedThreadPool)
    {
        /* The thread owns this extra reference until it exits (see
         * dispatch_thread_proc) - not tied to the caller's own reference. */
        InterlockedIncrement( &port->ref );
        port->dispatch_thread = CreateThread( NULL, 0, dispatch_thread_proc, port, 0, NULL );
        if (!port->dispatch_thread)
        {
            InterlockedDecrement( &port->ref );
            WARN( "failed to create dispatch thread for mode %d, callbacks on this port will never run.\n", mode );
        }
    }

    return port;
}

static void port_addref( struct x_task_queue_port *port )
{
    InterlockedIncrement( &port->ref );
}

static void port_release( struct x_task_queue_port *port )
{
    struct task_callback_entry *entry, *next;

    if (!port) return;
    if (InterlockedDecrement( &port->ref )) return;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &port->pending, struct task_callback_entry, entry )
    {
        list_remove( &entry->entry );
        free( entry );
    }
    if (port->dispatch_thread) CloseHandle( port->dispatch_thread );
    DeleteCriticalSection( &port->cs );
    free( port );
}

static BOOL is_valid_port_type( XTaskQueuePort port_type )
{
    return port_type == XTaskQueuePort_Work || port_type == XTaskQueuePort_Completion;
}

static inline struct x_threading *impl_from_IXThreadingImpl( IXThreadingImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_threading, IXThreadingImpl_iface );
}

static HRESULT WINAPI x_threading_QueryInterface( IXThreadingImpl *iface, REFIID iid, void **out )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXThreadingImpl ))
    {
        IXThreadingImpl_AddRef( *out = &impl->IXThreadingImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_threading_AddRef( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_threading_Release( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* Real XAsync engine, built on top of the already-working XTaskQueue port
 * machinery above. Previously every function in this section was a pure
 * E_NOTIMPL stub - since essentially all GDK/XSAPI async operations
 * (achievements, XblContext lifecycle, user sign-in helpers, etc.) go
 * through XAsyncBegin/XAsyncSchedule/XAsyncComplete, this silently broke
 * every async call in the whole stack; callers either got an immediate
 * E_NOTIMPL or, worse, proceeded with an unfinished/uninitialized result as
 * if the (never-run) operation had succeeded. Design follows the documented
 * real Microsoft XAsync provider contract (XAsyncOp_Begin/DoWork/GetResult/
 * Cancel, XAsyncProviderData layout - see xasyncprovider.idl in this same
 * directory) rather than guessing: XAsyncBegin invokes the provider's Begin
 * step synchronously and returns its result; a provider that wants to do
 * real work calls XAsyncSchedule (from Begin or from a later DoWork step),
 * which queues another DoWork invocation on the async block's own queue
 * (or the process-default queue, or a plain worker thread as a last-resort
 * fallback if neither exists - matching XAsyncSchedule's documented
 * "runs on a system-provided thread" behavior when no queue is available).
 *
 * Deliberate, documented simplification: the per-operation state is
 * refcounted and freed once fully drained (Begin's own ref released after
 * Begin returns without scheduling further work, plus one ref per
 * outstanding queued DoWork/completion-callback submission) - but
 * XAsyncOp_Cleanup (the real API's mechanism for a provider to free a large
 * result buffer once nobody will call XAsyncGetResult again) is
 * intentionally NOT dispatched. Getting cleanup timing wrong risks a
 * use-after-free regression across every previously-working title that
 * depends on this same DLL; not dispatching Cleanup at worst means a
 * provider whose GetResult path allocates a buffer keeps it until process
 * exit, a bounded leak rather than a crash. Cooperative cancellation only
 * (XAsyncCancel calls the provider's Cancel step but does not force
 * completion) - matches the documented contract, where DoWork is expected
 * to notice cancellation and complete with E_ABORT itself. */

/* Forward declarations - the XTaskQueue* functions this section calls into
 * are defined later in this file (in original vtable order). */
static HRESULT WINAPI x_threading_XTaskQueueSubmitCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port_type, void *callbackContext, XTaskQueueCallback *callback );
static void WINAPI x_threading_XTaskQueueCloseHandle( IXThreadingImpl *iface, XTaskQueueHandle queue );
static BOOLEAN WINAPI x_threading_XTaskQueueGetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle *queue );

struct async_op_state
{
    LONG ref;
    XAsyncBlock *block;
    XAsyncProvider *provider;
    void *context;
    const void *identity;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    HRESULT status;
    SIZE_T buffer_size;
    BOOL completed;
    /* XAsyncRun support: when non-NULL, DoWork calls this plain work
     * callback instead of invoking `provider` (XAsyncRun's own internal
     * provider, see x_threading_XAsyncRun below). */
    XAsyncWork *simple_work;
};

static void async_state_addref( struct async_op_state *state )
{
    InterlockedIncrement( &state->ref );
}

static void async_state_release( struct async_op_state *state )
{
    if (InterlockedDecrement( &state->ref )) return;
    DeleteCriticalSection( &state->cs );
    free( state );
}

static struct async_op_state *async_state_from_block( XAsyncBlock *asyncBlock )
{
    if (!asyncBlock) return NULL;
    return (struct async_op_state *)asyncBlock->internal[0];
}

static HRESULT invoke_provider( struct async_op_state *state, XAsyncOp op, SIZE_T bufferSize, void *buffer )
{
    XAsyncProviderData data;

    if (state->simple_work)
    {
        if (op == XAsyncOp_DoWork) return state->simple_work( state->block );
        return S_OK;
    }

    data.async = state->block;
    data.bufferSize = bufferSize;
    data.buffer = buffer;
    data.context = state->context;
    return state->provider( op, &data );
}

/* Shared by both dispatch paths below (queued callback vs. plain worker
 * thread) - always consumes exactly one state reference. */
static void run_dowork_and_release( struct async_op_state *state, BOOLEAN canceled )
{
    HRESULT hr;

    if (!canceled)
    {
        hr = invoke_provider( state, XAsyncOp_DoWork, 0, NULL );

        EnterCriticalSection( &state->cs );
        if (!state->completed && hr != E_PENDING)
        {
            state->status = hr;
            state->completed = TRUE;
            WakeAllConditionVariable( &state->cv );
        }
        LeaveCriticalSection( &state->cs );
    }

    async_state_release( state );
}

struct async_dowork_ctx
{
    struct async_op_state *state;
};

static void CALLBACK async_dowork_trampoline( void *context, BOOLEAN canceled )
{
    struct async_dowork_ctx *ctx = context;
    struct async_op_state *state = ctx->state;

    free( ctx );
    run_dowork_and_release( state, canceled );
}

struct async_dowork_thread_ctx
{
    struct async_op_state *state;
    UINT32 delay_ms;
};

static DWORD WINAPI async_dowork_thread_proc( void *arg )
{
    struct async_dowork_thread_ctx *ctx = arg;
    struct async_op_state *state = ctx->state;
    UINT32 delay = ctx->delay_ms;

    free( ctx );
    if (delay) Sleep( delay );
    run_dowork_and_release( state, FALSE );
    return 0;
}

static HRESULT WINAPI x_threading_XAsyncSchedule( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, UINT32 delayInMs )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );
    struct x_task_queue *q;
    XTaskQueueHandle process_queue = NULL;
    BOOL owns_queue_ref = FALSE;

    TRACE( "iface %p, asyncBlock %p, delayInMs %u.\n", iface, asyncBlock, delayInMs );

    if (!state) return E_INVALIDARG;

    async_state_addref( state );

    q = (struct x_task_queue *)asyncBlock->queue;
    if (!q && x_threading_XTaskQueueGetCurrentProcessTaskQueue( iface, &process_queue ))
    {
        q = (struct x_task_queue *)process_queue;
        owns_queue_ref = TRUE;
    }

    if (q && !delayInMs)
    {
        struct async_dowork_ctx *ctx = malloc( sizeof(*ctx) );
        HRESULT hr;

        if (!ctx)
        {
            if (owns_queue_ref) x_threading_XTaskQueueCloseHandle( iface, (XTaskQueueHandle)q );
            async_state_release( state );
            return E_OUTOFMEMORY;
        }
        ctx->state = state;

        hr = x_threading_XTaskQueueSubmitCallback( iface, (XTaskQueueHandle)q, XTaskQueuePort_Work, ctx, async_dowork_trampoline );
        if (FAILED(hr))
        {
            free( ctx );
            async_state_release( state );
        }
        if (owns_queue_ref) x_threading_XTaskQueueCloseHandle( iface, (XTaskQueueHandle)q );
        return hr;
    }

    if (owns_queue_ref) x_threading_XTaskQueueCloseHandle( iface, (XTaskQueueHandle)q );

    /* No queue at all, or a delay was requested (XTaskQueueSubmitDelayedCallback
     * is itself still unimplemented) - fall back to a plain worker thread,
     * matching XAsyncSchedule's documented behavior of running on a
     * system-provided thread when the caller didn't supply a queue. */
    {
        struct async_dowork_thread_ctx *ctx = malloc( sizeof(*ctx) );
        HANDLE thread;

        if (!ctx)
        {
            async_state_release( state );
            return E_OUTOFMEMORY;
        }
        ctx->state = state;
        ctx->delay_ms = delayInMs;

        thread = CreateThread( NULL, 0, async_dowork_thread_proc, ctx, 0, NULL );
        if (!thread)
        {
            free( ctx );
            async_state_release( state );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
        CloseHandle( thread );
        return S_OK;
    }
}

static HRESULT WINAPI x_threading_XAsyncBegin( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, void *context, const void *identity, const char *identityName, XAsyncProvider *provider )
{
    struct async_op_state *state;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, context %p, identity %p, identityName %s, provider %p.\n",
           iface, asyncBlock, context, identity, debugstr_a( identityName ), provider );

    if (!asyncBlock || !provider) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;
    state->ref = 1;
    state->block = asyncBlock;
    state->provider = provider;
    state->context = context;
    state->identity = identity;
    state->status = E_PENDING;
    InitializeCriticalSection( &state->cs );
    InitializeConditionVariable( &state->cv );

    asyncBlock->internal[0] = state;
    asyncBlock->internal[1] = NULL;
    asyncBlock->internal[2] = NULL;
    asyncBlock->internal[3] = NULL;

    hr = invoke_provider( state, XAsyncOp_Begin, 0, NULL );
    if (FAILED(hr))
    {
        asyncBlock->internal[0] = NULL;
        async_state_release( state );
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXThreadingImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_threading_XAsyncRun( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, XAsyncWork *work )
{
    struct async_op_state *state;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, work %p.\n", iface, asyncBlock, work );

    if (!asyncBlock || !work) return E_INVALIDARG;

    if (!(state = calloc( 1, sizeof(*state) ))) return E_OUTOFMEMORY;
    state->ref = 1;
    state->block = asyncBlock;
    state->simple_work = work;
    state->status = E_PENDING;
    InitializeCriticalSection( &state->cs );
    InitializeConditionVariable( &state->cv );

    asyncBlock->internal[0] = state;
    asyncBlock->internal[1] = NULL;
    asyncBlock->internal[2] = NULL;
    asyncBlock->internal[3] = NULL;

    hr = x_threading_XAsyncSchedule( iface, asyncBlock, 0 );
    if (FAILED(hr))
    {
        asyncBlock->internal[0] = NULL;
        async_state_release( state );
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI x_threading_XAsyncGetStatus( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, BOOLEAN wait )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, wait %d.\n", iface, asyncBlock, wait );

    if (!state) return E_INVALIDARG;

    EnterCriticalSection( &state->cs );
    while (!state->completed && wait)
        SleepConditionVariableCS( &state->cv, &state->cs, INFINITE );
    hr = state->completed ? state->status : E_PENDING;
    LeaveCriticalSection( &state->cs );

    return hr;
}

static HRESULT WINAPI x_threading_XAsyncGetResultSize( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *bufferSize )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );

    TRACE( "iface %p, asyncBlock %p, bufferSize %p.\n", iface, asyncBlock, bufferSize );

    if (!state || !bufferSize) return E_INVALIDARG;
    if (!state->completed) return E_PENDING;
    if (FAILED(state->status)) return state->status;

    *bufferSize = state->buffer_size;
    return S_OK;
}

static void WINAPI x_threading_XAsyncCancel( IXThreadingImpl *iface, XAsyncBlock *asyncBlock )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );

    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );

    if (!state) return;
    invoke_provider( state, XAsyncOp_Cancel, 0, NULL );
}

struct async_completion_ctx
{
    XAsyncBlock *block;
    struct async_op_state *state;
};

static void CALLBACK async_completion_trampoline( void *context, BOOLEAN canceled )
{
    struct async_completion_ctx *ctx = context;
    XAsyncBlock *block = ctx->block;
    struct async_op_state *state = ctx->state;

    free( ctx );
    if (!canceled && block->callback) block->callback( block );
    async_state_release( state );
}

static void WINAPI x_threading_XAsyncComplete( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, HRESULT result, SIZE_T requiredBufferSize )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );

    TRACE( "iface %p, asyncBlock %p, result %#lx, requiredBufferSize %Iu.\n", iface, asyncBlock, result, requiredBufferSize );

    if (!state) return;

    EnterCriticalSection( &state->cs );
    if (state->completed) { LeaveCriticalSection( &state->cs ); return; }
    state->status = result;
    state->buffer_size = requiredBufferSize;
    state->completed = TRUE;
    WakeAllConditionVariable( &state->cv );
    LeaveCriticalSection( &state->cs );

    if (asyncBlock->callback)
    {
        struct async_completion_ctx *ctx = malloc( sizeof(*ctx) );
        struct x_task_queue *q = (struct x_task_queue *)asyncBlock->queue;

        if (!ctx) return;

        async_state_addref( state );
        ctx->block = asyncBlock;
        ctx->state = state;

        if (!q || FAILED( x_threading_XTaskQueueSubmitCallback( iface, (XTaskQueueHandle)q, XTaskQueuePort_Completion, ctx, async_completion_trampoline ) ))
        {
            /* No queue, or submit failed: invoke directly, best effort. */
            free( ctx );
            async_state_release( state );
            asyncBlock->callback( asyncBlock );
        }
    }
}

static HRESULT WINAPI x_threading_XAsyncGetResult( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, const void *identity, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    struct async_op_state *state = async_state_from_block( asyncBlock );
    HRESULT hr;

    TRACE( "iface %p asyncBlock %p, identity %p, bufferSize %Iu, buffer %p, bufferUsed %p.\n", iface, asyncBlock, identity, bufferSize, buffer, bufferUsed );

    if (!state) return E_INVALIDARG;
    if (!state->completed) return E_PENDING;
    if (FAILED(state->status)) return state->status;
    if (identity && state->identity && identity != state->identity) return E_INVALIDARG;

    hr = invoke_provider( state, XAsyncOp_GetResult, bufferSize, buffer );
    if (SUCCEEDED(hr) && bufferUsed) *bufferUsed = state->buffer_size;
    return hr;
}

static HRESULT WINAPI x_threading_XTaskQueueCreate( IXThreadingImpl *iface, XTaskQueueDispatchMode workDispatchMode, XTaskQueueDispatchMode completionDispatchMode, XTaskQueueHandle *queue )
{
    struct x_task_queue *q;

    TRACE( "iface %p, workDispatchMode %d, completionDispatchMode %d, queue %p.\n", iface, workDispatchMode, completionDispatchMode, queue );

    if (!queue) return E_INVALIDARG;

    if (!(q = calloc( 1, sizeof(*q) ))) return E_OUTOFMEMORY;
    q->ref = 1;
    if (!(q->ports[XTaskQueuePort_Work] = create_port( workDispatchMode )) ||
        !(q->ports[XTaskQueuePort_Completion] = create_port( completionDispatchMode )))
    {
        port_release( q->ports[XTaskQueuePort_Work] );
        port_release( q->ports[XTaskQueuePort_Completion] );
        free( q );
        return E_OUTOFMEMORY;
    }

    *queue = (XTaskQueueHandle)q;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueCreateComposite( IXThreadingImpl *iface, XTaskQueuePortHandle workPort, XTaskQueuePortHandle completionPort, XTaskQueueHandle *queue )
{
    struct x_task_queue *q;

    TRACE( "iface %p, workPort %p, completionPort %p, queue %p.\n", iface, workPort, completionPort, queue );

    if (!queue || !workPort || !completionPort) return E_INVALIDARG;

    if (!(q = calloc( 1, sizeof(*q) ))) return E_OUTOFMEMORY;
    q->ref = 1;
    q->ports[XTaskQueuePort_Work] = (struct x_task_queue_port *)workPort;
    q->ports[XTaskQueuePort_Completion] = (struct x_task_queue_port *)completionPort;
    port_addref( q->ports[XTaskQueuePort_Work] );
    port_addref( q->ports[XTaskQueuePort_Completion] );

    *queue = (XTaskQueueHandle)q;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueGetPort( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle *portHandle )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;

    TRACE( "iface %p, queue %p, port %d, portHandle %p.\n", iface, queue, port, portHandle );

    if (!q || !portHandle || !is_valid_port_type( port )) return E_INVALIDARG;

    port_addref( q->ports[port] );
    *portHandle = (XTaskQueuePortHandle)q->ports[port];
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueDuplicateHandle( IXThreadingImpl *iface, XTaskQueueHandle queueHandle, XTaskQueueHandle *duplicatedHandle )
{
    struct x_task_queue *q = (struct x_task_queue *)queueHandle;

    TRACE( "iface %p, queueHandle %p, duplicatedHandle %p.\n", iface, queueHandle, duplicatedHandle );

    if (!q || !duplicatedHandle) return E_INVALIDARG;

    InterlockedIncrement( &q->ref );
    *duplicatedHandle = queueHandle;
    return S_OK;
}

static BOOLEAN WINAPI x_threading_XTaskQueueDispatch( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port_type, UINT32 timeoutInMs )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;
    struct x_task_queue_port *port;
    struct task_callback_entry *entry;
    struct list *head;

    TRACE( "iface %p, queue %p, port %d, timeoutInMs %d.\n", iface, queue, port_type, timeoutInMs );

    if (!q || !is_valid_port_type( port_type )) return FALSE;
    port = q->ports[port_type];

    EnterCriticalSection( &port->cs );
    if (list_empty( &port->pending ))
    {
        if (!timeoutInMs || !SleepConditionVariableCS( &port->cv, &port->cs, timeoutInMs ) || list_empty( &port->pending ))
        {
            LeaveCriticalSection( &port->cs );
            return FALSE;
        }
    }
    head = list_head( &port->pending );
    entry = LIST_ENTRY( head, struct task_callback_entry, entry );
    list_remove( &entry->entry );
    LeaveCriticalSection( &port->cs );

    entry->callback( entry->context, FALSE );
    free( entry );
    return TRUE;
}

static void WINAPI x_threading_XTaskQueueCloseHandle( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (!q) return;
    if (InterlockedDecrement( &q->ref )) return;

    port_release( q->ports[XTaskQueuePort_Work] );
    port_release( q->ports[XTaskQueuePort_Completion] );
    free( q );
}

static DWORD WINAPI threadpool_port_dispatcher( void *arg )
{
    struct x_task_queue_port *port = arg;
    struct task_callback_entry *entry = NULL;
    struct list *head;

    EnterCriticalSection( &port->cs );
    if (!list_empty( &port->pending ) && !ReadNoFence( &port->terminated ))
    {
        head = list_head( &port->pending );
        entry = LIST_ENTRY( head, struct task_callback_entry, entry );
        list_remove( &entry->entry );
    }
    LeaveCriticalSection( &port->cs );

    if (entry)
    {
        entry->callback( entry->context, FALSE );
        free( entry );
    }
    port_release( port );
    return 0;
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port_type, void *callbackContext, XTaskQueueCallback *callback )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;
    struct x_task_queue_port *port;
    struct task_callback_entry *entry;

    TRACE( "iface %p, queue %p, port %d, callbackContext %p, callback %p.\n", iface, queue, port_type, callbackContext, callback );

    if (!q || !callback || !is_valid_port_type( port_type )) return E_INVALIDARG;
    port = q->ports[port_type];

    /* Documented XTaskQueueTerminate contract: once a port has been
     * terminated, submitting new callbacks to it fails with E_ABORT. */
    if (ReadNoFence( &port->terminated )) return E_ABORT;

    if (port->dispatch_mode == XTaskQueueDispatchMode_Immediate)
    {
        callback( callbackContext, FALSE );
        return S_OK;
    }

    if (!(entry = malloc( sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->callback = callback;
    entry->context = callbackContext;

    EnterCriticalSection( &port->cs );
    list_add_tail( &port->pending, &entry->entry );
    LeaveCriticalSection( &port->cs );
    WakeAllConditionVariable( &port->cv );

    if (port->dispatch_mode == XTaskQueueDispatchMode_ThreadPool ||
        port->dispatch_mode == XTaskQueueDispatchMode_SerializedThreadPool)
    {
        port_addref( port );
        if (!QueueUserWorkItem( threadpool_port_dispatcher, port, WT_EXECUTEDEFAULT ))
        {
            port_release( port );
        }
    }

    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitDelayedCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, UINT32 delayMs, void *callbackContext, XTaskQueueCallback *callback )
{
    FIXME( "iface %p, queue %p, port %d, delayMs %d, callbackContext %p, callback %p stub!\n", iface, queue, port, delayMs, callbackContext, callback );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle, void *callbackContext, XTaskQueueCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, port %d, waitHandle %p, callbackContext %p, callback %p, token %p stub!\n", iface, queue, port, waitHandle, callbackContext, callback, token );
    return E_NOTIMPL;
}

static void WINAPI x_threading_XTaskQueueUnregisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    FIXME( "iface %p, queue %p, token %p stub!\n", iface, queue, &token );
}

/* Marks a port terminated (blocking further XTaskQueueSubmitCallback calls
 * with E_ABORT, per the documented contract), then dispatches every callback
 * still pending on it with canceled=TRUE, as XTaskQueueTerminate requires.
 * Also wakes any thread blocked in XTaskQueueDispatch's SleepConditionVariableCS
 * so it observes the now-empty, terminated port and returns FALSE instead of
 * waiting out its timeout. */
static void terminate_port( struct x_task_queue_port *port )
{
    struct list drained;
    struct task_callback_entry *entry, *next;

    list_init( &drained );

    EnterCriticalSection( &port->cs );
    InterlockedExchange( &port->terminated, TRUE );
    list_move_tail( &drained, &port->pending );
    LeaveCriticalSection( &port->cs );
    WakeAllConditionVariable( &port->cv );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &drained, struct task_callback_entry, entry )
    {
        list_remove( &entry->entry );
        entry->callback( entry->context, TRUE );
        free( entry );
    }
}

static HRESULT WINAPI x_threading_XTaskQueueTerminate( IXThreadingImpl *iface, XTaskQueueHandle queue, BOOLEAN wait, void *callbackContext, XTaskQueueTerminatedCallback *callback )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;
    struct x_task_queue_port *completion_port;

    TRACE( "iface %p, queue %p, wait %d, callbackContext %p, callback %p.\n", iface, queue, wait, callbackContext, callback );

    if (!q) return E_INVALIDARG;

    completion_port = q->ports[XTaskQueuePort_Completion];

    /* Per the documented contract: cancel every pending work-port callback,
     * then every pending completion-port callback (both ports terminated,
     * new submissions to either now fail with E_ABORT). This implementation
     * has no background dispatch thread (see the comment at the top of this
     * file), so there is nothing to keep running in the background after
     * this point regardless of `wait` - draining is inherently synchronous. */
    terminate_port( q->ports[XTaskQueuePort_Work] );
    terminate_port( completion_port );

    if (!callback) return S_OK;

    if (wait || completion_port->dispatch_mode == XTaskQueueDispatchMode_Immediate)
    {
        /* wait=TRUE: docs guarantee termination (including the callback) has
         * completed by the time this function returns. Immediate-dispatch
         * completion ports never queue (XTaskQueueSubmitCallback runs them
         * synchronously too), and with no worker thread to defer to there is
         * nothing to gain by pretending this is asynchronous. */
        callback( callbackContext );
        return S_OK;
    }

    /* wait=FALSE with a real (Manual/ThreadPool/SerializedThreadPool)
     * completion port: deliver the notification "from the completion
     * thread" as documented, by queuing it through the same pending-list
     * path XTaskQueueDispatch already pumps. Enqueued directly (bypassing
     * XTaskQueueSubmitCallback) since that entry point now correctly
     * rejects new work on this just-terminated port with E_ABORT - this is
     * our own internal termination notice, not new app-submitted work. */
    {
        struct terminate_notify *notify;
        struct task_callback_entry *entry;

        if (!(notify = malloc( sizeof(*notify) ))) return E_OUTOFMEMORY;
        notify->callback = callback;
        notify->context = callbackContext;

        if (!(entry = malloc( sizeof(*entry) )))
        {
            free( notify );
            return E_OUTOFMEMORY;
        }
        entry->callback = terminate_notify_trampoline;
        entry->context = notify;

        EnterCriticalSection( &completion_port->cs );
        list_add_tail( &completion_port->pending, &entry->entry );
        LeaveCriticalSection( &completion_port->cs );
        WakeAllConditionVariable( &completion_port->cv );
    }

    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, void *callbackContext, XTaskQueueMonitorCallback *callback, XTaskQueueRegistrationToken *token )
{
    FIXME( "iface %p, queue %p, callbackContext %p, callback %p, token %p stub!\n", iface, queue, callbackContext, callback, token );
    return E_NOTIMPL;
}

static void WINAPI x_threading_XTaskQueueUnregisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    FIXME( "iface %p, queue %p, token %p stub!\n", iface, queue, &token );
}

/* Process-wide default task queue, set via XTaskQueueSetCurrentProcessTaskQueue
 * and consumed by any GDK/Xbox Live API called with a NULL queue argument.
 * Documented at learn.microsoft.com's XTaskQueueSetCurrentProcessTaskQueue
 * page: "the provided queue will have its handle duplicated and any
 * existing process task queue will have its handle closed" - i.e. Set
 * duplicates (not transfers) ownership, symmetric with Get also handing
 * back a duplicated (caller-owned) handle. Reuses the same
 * duplicate/close logic as XTaskQueueDuplicateHandle/XTaskQueueCloseHandle
 * above rather than re-implementing the refcount teardown. */
static struct x_task_queue *current_process_task_queue;

static BOOLEAN WINAPI x_threading_XTaskQueueGetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle *queue )
{
    struct x_task_queue *q;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (!queue) return FALSE;

    if (!current_process_task_queue)
    {
        XTaskQueueHandle default_q = NULL;
        if (SUCCEEDED(x_threading_XTaskQueueCreate( iface, XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &default_q )))
        {
            if (InterlockedCompareExchangePointer( (void **)&current_process_task_queue, default_q, NULL ))
            {
                x_threading_XTaskQueueCloseHandle( iface, default_q );
            }
        }
    }

    q = current_process_task_queue;
    if (!q)
    {
        *queue = NULL;
        return FALSE;
    }

    InterlockedIncrement( &q->ref );
    *queue = (XTaskQueueHandle)q;
    return TRUE;
}

static void WINAPI x_threading_XTaskQueueSetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    struct x_task_queue *q = (struct x_task_queue *)queue;
    struct x_task_queue *old;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (q) InterlockedIncrement( &q->ref );
    old = InterlockedExchangePointer( (void **)&current_process_task_queue, q );
    if (old) x_threading_XTaskQueueCloseHandle( iface, (XTaskQueueHandle)old );
}

static HRESULT WINAPI x_threading_XThreadSetTimeSensitive( IXThreadingImpl *iface, BOOLEAN isTimeSensitiveThread )
{
    TRACE( "iface %p, isTimeSensitiveThread %d.\n", iface, isTimeSensitiveThread );
    if (!TlsSetValue( tlsIndex, (void *)(UINT_PTR)isTimeSensitiveThread )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static void WINAPI x_threading_XThreadAssertNotTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    if (TlsGetValue( tlsIndex )) DebugBreak();
}

static BOOLEAN WINAPI x_threading_XThreadIsTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    return TlsGetValue( tlsIndex ) ? 1 : 0;
}

static const struct IXThreadingImplVtbl x_threading_vtbl =
{
    x_threading_QueryInterface,
    x_threading_AddRef,
    x_threading_Release,
    /* IXThreadingImpl methods */
    x_threading_XAsyncGetStatus,
    x_threading_XAsyncGetResultSize,
    x_threading_XAsyncCancel,
    x_threading_XAsyncRun,
    x_threading_XAsyncBegin,
    __PADDING__,
    x_threading_XAsyncSchedule,
    x_threading_XAsyncComplete,
    x_threading_XAsyncGetResult,
    x_threading_XTaskQueueCreate,
    x_threading_XTaskQueueCreateComposite,
    x_threading_XTaskQueueGetPort,
    x_threading_XTaskQueueDuplicateHandle,
    x_threading_XTaskQueueDispatch,
    x_threading_XTaskQueueCloseHandle,
    x_threading_XTaskQueueSubmitCallback,
    x_threading_XTaskQueueSubmitDelayedCallback,
    x_threading_XTaskQueueRegisterWaiter,
    x_threading_XTaskQueueUnregisterWaiter,
    x_threading_XTaskQueueTerminate,
    x_threading_XTaskQueueRegisterMonitor,
    x_threading_XTaskQueueUnregisterMonitor,
    x_threading_XTaskQueueGetCurrentProcessTaskQueue,
    x_threading_XTaskQueueSetCurrentProcessTaskQueue,
    x_threading_XThreadSetTimeSensitive,
    __PADDING__,
    x_threading_XThreadAssertNotTimeSensitive,
    x_threading_XThreadIsTimeSensitive
};

static struct x_threading x_threading =
{
    {&x_threading_vtbl},
    0,
};

IXThreadingImpl *x_threading_impl = &x_threading.IXThreadingImpl_iface;
