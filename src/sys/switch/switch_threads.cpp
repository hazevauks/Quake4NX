/*
===========================================================================

Quake4NX - threads, critical sections and events.

libnx starts every thread on core 0, where the engine runs. The link wraps
pthread_create (-Wl,--wrap=pthread_create) so library threads too, such as the
OpenAL mixer and std::thread, start through a trampoline that moves them to their core.

===========================================================================
*/

#include "../../idlib/precompiled.h"

#include <pthread.h>

#undef BIT
#include <switch.h>

#include "switch_local.h"

static pthread_mutex_t	s_criticalSections[MAX_CRITICAL_SECTIONS];
static pthread_mutex_t	s_eventMutex;
static pthread_cond_t	s_eventConds[MAX_TRIGGER_EVENTS];
static bool				s_eventSignaled[MAX_TRIGGER_EVENTS];

xthreadInfo *			g_threads[MAX_THREADS];
int						g_thread_count = 0;

static volatile int		s_nextThreadCore = -1;
static __thread xthreadInfo *s_currentThread = NULL;

void Sys_EnterCriticalSection( int index ) {
	pthread_mutex_lock( &s_criticalSections[index] );
}

void Sys_LeaveCriticalSection( int index ) {
	pthread_mutex_unlock( &s_criticalSections[index] );
}

void Sys_WaitForEvent( int index ) {
	pthread_mutex_lock( &s_eventMutex );
	while ( !s_eventSignaled[index] ) {
		pthread_cond_wait( &s_eventConds[index], &s_eventMutex );
	}
	s_eventSignaled[index] = false;
	pthread_mutex_unlock( &s_eventMutex );
}

void Sys_TriggerEvent( int index ) {
	pthread_mutex_lock( &s_eventMutex );
	s_eventSignaled[index] = true;
	pthread_cond_signal( &s_eventConds[index] );
	pthread_mutex_unlock( &s_eventMutex );
}

/*
==================
Sys_CreateThread
==================
*/
typedef struct {
	xthread_t		function;
	void *			parms;
	xthreadInfo *	info;
} threadStart_t;

static void *Switch_ThreadMain( void *arg ) {
	threadStart_t start = *(threadStart_t *)arg;
	free( arg );
	s_currentThread = start.info;
	start.function( start.parms );
	return NULL;
}

void Sys_CreateThread( xthread_t function, void *parms, xthreadPriority priority, xthreadInfo &info, const char *name, xthreadInfo *threads[MAX_THREADS], int *thread_count ) {
	threadStart_t *start = (threadStart_t *)malloc( sizeof( *start ) );
	start->function = function;
	start->parms = parms;
	start->info = &info;

	info.stopRequested = false;

	pthread_t thread;
	if ( pthread_create( &thread, NULL, Switch_ThreadMain, start ) != 0 ) {
		free( start );
		common->Error( "pthread_create failed for thread %s", name );
		return;
	}
	info.name = name;
	info.threadHandle = (uintptr_t)thread;
	info.threadId = (uint32_t)(uintptr_t)thread;

	if ( *thread_count < MAX_THREADS ) {
		threads[( *thread_count )++] = &info;
	}
}

void Sys_DestroyThread( xthreadInfo &info ) {
	if ( !info.threadHandle ) {
		return;
	}
	Sys_RequestThreadStop( info );
	pthread_join( (pthread_t)info.threadHandle, NULL );

	for ( int i = 0; i < g_thread_count; i++ ) {
		if ( g_threads[i] == &info ) {
			g_threads[i] = g_threads[--g_thread_count];
			break;
		}
	}
	info.threadHandle = 0;
	info.name = NULL;
}

void Sys_RequestThreadStop( xthreadInfo &info ) {
	info.stopRequested = true;
	// a thread asleep on an event has to wake up to see it
	for ( int i = 0; i < MAX_TRIGGER_EVENTS; i++ ) {
		Sys_TriggerEvent( i );
	}
}

bool Sys_IsThreadStopRequested( const xthreadInfo &info ) {
	return info.stopRequested;
}

bool Sys_IsCurrentThreadStopRequested( void ) {
	return s_currentThread != NULL && s_currentThread->stopRequested;
}

const char *Sys_GetThreadName( int *index ) {
	const uint32_t self = (uint32_t)(uintptr_t)pthread_self();
	for ( int i = 0; i < g_thread_count; i++ ) {
		if ( g_threads[i]->threadId == self ) {
			if ( index ) {
				*index = i;
			}
			return g_threads[i]->name;
		}
	}
	if ( index ) {
		*index = -1;
	}
	return "main";
}

/*
==================
Async thread

The 16 ms tick that mixes sound and runs the network.
==================
*/
static xthreadInfo		s_asyncThread;
static volatile bool	s_asyncExit = false;

static unsigned int Switch_AsyncThread( void * ) {
	int ticked = Sys_Milliseconds() >> 4;

	while ( !s_asyncExit ) {
		const int now = Sys_Milliseconds();
		const int wait = ( ( now & ~15 ) + 16 ) - now - 1;
		if ( wait > 0 ) {
			Sys_Sleep( wait );
		}
		const int target = Sys_Milliseconds() >> 4;
		while ( ticked < target ) {
			common->Async();
			ticked++;
			Sys_TriggerEvent( TRIGGER_EVENT_ONE );
		}
	}
	return 0;
}

void Switch_StartAsyncThread( void ) {
	if ( s_asyncThread.threadHandle ) {
		return;
	}
	s_asyncExit = false;
	Switch_SetNextThreadCore( SWITCH_CORE_WORKER );
	Sys_CreateThread( Switch_AsyncThread, NULL, THREAD_NORMAL, s_asyncThread, "Async", g_threads, &g_thread_count );
}

void Switch_StopAsyncThread( void ) {
	if ( s_asyncThread.threadHandle ) {
		s_asyncExit = true;
		Sys_DestroyThread( s_asyncThread );
	}
}

void Switch_InitThreads( void ) {
	pthread_mutexattr_t recursive;
	pthread_mutexattr_init( &recursive );
	pthread_mutexattr_settype( &recursive, PTHREAD_MUTEX_RECURSIVE );
	for ( int i = 0; i < MAX_CRITICAL_SECTIONS; i++ ) {
		pthread_mutex_init( &s_criticalSections[i], &recursive );
	}
	pthread_mutexattr_destroy( &recursive );

	pthread_mutex_init( &s_eventMutex, NULL );
	for ( int i = 0; i < MAX_TRIGGER_EVENTS; i++ ) {
		pthread_cond_init( &s_eventConds[i], NULL );
		s_eventSignaled[i] = false;
	}
}

/*
==================
Core placement
==================
*/
typedef struct {
	void *	( *start )( void * );
	void *	arg;
	int		core;
} threadTrampoline_t;

extern "C" int __real_pthread_create( pthread_t *thread, const pthread_attr_t *attr, void *( *start )( void * ), void *arg );

static void *Switch_ThreadTrampoline( void *param ) {
	const threadTrampoline_t t = *(threadTrampoline_t *)param;
	free( param );
	svcSetThreadCoreMask( threadGetCurHandle(), t.core, 1u << t.core );
	return t.start( t.arg );
}

extern "C" int __wrap_pthread_create( pthread_t *thread, const pthread_attr_t *attr, void *( *start )( void * ), void *arg ) {
	int core = __atomic_exchange_n( &s_nextThreadCore, -1, __ATOMIC_SEQ_CST );
	if ( core < 0 ) {
		core = SWITCH_CORE_WORKER;
	}

	threadTrampoline_t *t = (threadTrampoline_t *)malloc( sizeof( *t ) );
	if ( !t ) {
		return __real_pthread_create( thread, attr, start, arg );
	}
	t->start = start;
	t->arg = arg;
	t->core = core;
	const int result = __real_pthread_create( thread, attr, Switch_ThreadTrampoline, t );
	if ( result != 0 ) {
		free( t );
	}
	return result;
}

void Switch_SetNextThreadCore( int core ) {
	__atomic_store_n( &s_nextThreadCore, core, __ATOMIC_SEQ_CST );
}
