/*
===========================================================================

Quake4NX - native deko3D renderer: the back end on its own core.

Upstream's render thread, from its Vulkan module. EndFrame hands the finished
command list to this thread, which records and presents it on core 1, while the
main thread goes on to the next frame's events, console commands, sound and game
tic on core 0. The next BeginFrame waits for it and then completes the frame.

Only the game tic overlaps the back end. The next frame's front end still starts after
the back end has finished, so frame memory, the command chain and the vertex cache's
frame temps keep one owner at every moment. Main thread entries that are not safe
during the tic wait first (R_RenderThread_Sync).

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../tr_local.h"
#include "../RendererMetrics.h"

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#undef BIT
#include <switch.h>

#include "dk_local.h"
#include "../../sys/switch/switch_local.h"

idCVar r_dkRenderThread( "r_dkRenderThread", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
	"1 records and presents each frame on core 1 while core 0 runs the next game tic. 0 runs the back end inline" );

static const size_t RENDER_THREAD_STACK_BYTES = 2 * 1024 * 1024;

static pthread_t					rt_thread;
static bool							rt_threadRunning = false;
static std::mutex					rt_mutex;
static std::condition_variable		rt_wake;
static std::condition_variable		rt_done;
static const emptyCommand_t *		rt_commands = NULL;	// owned by the render thread while rt_busy
static bool							rt_busy = false;
static bool							rt_quit = false;
static bool							rt_failed = false;
static char							rt_failure[1024];
static int							rt_backEndMsec = 0;
static std::atomic<bool>			rt_inFlight( false );	// readable without the mutex

// counters for renderThreadStats; main thread only
static int							rt_framesThreaded = 0;
static int							rt_framesInline = 0;
static int							rt_syncWaits = 0;
static long long					rt_syncWaitUsec = 0;
static idStrList					rt_syncReasons;
static idList<int>					rt_syncReasonCounts;

static void *R_RenderThread_Main( void * ) {
	for ( ;; ) {
		const emptyCommand_t *cmds;
		{
			std::unique_lock<std::mutex> lock( rt_mutex );
			rt_wake.wait( lock, [] { return rt_quit || rt_commands != NULL; } );
			if ( rt_quit && rt_commands == NULL ) {
				return NULL;
			}
			cmds = rt_commands;
		}

		bool failed = false;
		const int start = Sys_Milliseconds();
		try {
			RB_ExecuteBackEndCommands( cmds );
		} catch ( idException &ex ) {
			// common->Error queued its message for the main thread and unwound
			// this thread only; R_RenderThread_Sync re-raises it there
			failed = true;
			idStr::Copynz( rt_failure, ex.error, sizeof( rt_failure ) );
		}
		const int elapsed = Sys_Milliseconds() - start;

		{
			std::lock_guard<std::mutex> lock( rt_mutex );
			rt_commands = NULL;
			rt_busy = false;
			rt_failed = rt_failed || failed;
			rt_backEndMsec = elapsed;
			rt_inFlight.store( false, std::memory_order_release );
		}
		rt_done.notify_all();
	}
}

static void R_RenderThread_Start( void ) {
	if ( rt_threadRunning ) {
		return;
	}
	rt_quit = false;
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, RENDER_THREAD_STACK_BYTES );
	Switch_SetNextThreadCore( SWITCH_CORE_RENDER );
	if ( pthread_create( &rt_thread, &attr, R_RenderThread_Main, NULL ) != 0 ) {
		common->FatalError( "could not start the render thread" );
	}
	pthread_attr_destroy( &attr );
	rt_threadRunning = true;
	common->Printf( "Render thread started, on core %i\n", SWITCH_CORE_RENDER );
}

/*
==================
R_RenderThread_OnRenderThread
==================
*/
bool R_RenderThread_OnRenderThread( void ) {
	return rt_threadRunning && pthread_equal( pthread_self(), rt_thread );
}

/*
==================
R_RenderThread_InFlight

True while a frame's backend may be running on the render thread.
==================
*/
bool R_RenderThread_InFlight( void ) {
	return rt_inFlight.load( std::memory_order_acquire );
}

/*
==================
R_RenderThread_Begin

Main thread, from EndFrame. Returns false to have the caller execute the
commands inline: the prototype is off, a capture needs the frame now, or
the configuration is outside the audited stage-1 set.
==================
*/
bool R_RenderThread_Begin( const emptyCommand_t *cmds ) {
	if ( !r_dkRenderThread.GetBool() || r_skipBackEnd.GetBool() || !R_RenderThread_FrameQualifies() ) {
		rt_framesInline++;
		return false;
	}
	R_RenderThread_Start();
	{
		std::lock_guard<std::mutex> lock( rt_mutex );
		assert( !rt_busy );
		rt_commands = cmds;
		rt_busy = true;
		rt_inFlight.store( true, std::memory_order_release );
	}
	rt_wake.notify_one();
	rt_framesThreaded++;
	return true;
}

static void R_RenderThread_CountReason( const char *reason ) {
	const int index = rt_syncReasons.FindIndex( reason );
	if ( index >= 0 ) {
		rt_syncReasonCounts[index]++;
		return;
	}
	if ( rt_syncReasons.Num() < 64 ) {
		rt_syncReasons.Append( reason );
		rt_syncReasonCounts.Append( 1 );
	}
}

/*
==================
R_RenderThread_Sync

Main thread. Waits for the frame on the render thread, if any, and completes
it. Every main-thread renderer entry that touches state the backend reads,
other than the stage-1 tic-safe set, calls this first. The render thread
itself never waits on itself.
==================
*/
void R_RenderThread_Sync( const char *reason ) {
	if ( !rt_inFlight.load( std::memory_order_acquire ) || R_RenderThread_OnRenderThread() ) {
		return;
	}
	const unsigned long long waitStart = R_RendererMetrics_CpuClock();
	bool failed;
	char failure[sizeof( rt_failure )];
	int backEndMsec;
	{
		std::unique_lock<std::mutex> lock( rt_mutex );
		rt_done.wait( lock, [] { return !rt_busy; } );
		failed = rt_failed;
		rt_failed = false;
		idStr::Copynz( failure, rt_failure, sizeof( failure ) );
		backEndMsec = rt_backEndMsec;
	}
	rt_syncWaits++;
	rt_syncWaitUsec += static_cast<long long>( R_RendererMetrics_CpuClock() - waitStart );	// microseconds
	if ( idStr::Cmp( reason, "frame" ) != 0 ) {
		R_RenderThread_CountReason( reason );
	}

	R_RendererMetrics_RecordSubmitMsec( backEndMsec );
	tr.CompleteBackEndFrame();

	if ( failed ) {
		common->Error( "render thread: %s", failure );
	}
}

/*
==================
R_RenderThread_Shutdown

Main thread. Completes any frame in flight and joins the thread; the device
and frame memory may be torn down afterwards.
==================
*/
void R_RenderThread_Shutdown( void ) {
	if ( R_RenderThread_OnRenderThread() ) {
		// a fatal error raised by the backend; the process is exiting
		return;
	}
	R_RenderThread_Sync( "shutdown" );
	if ( !rt_threadRunning ) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock( rt_mutex );
		rt_quit = true;
	}
	rt_wake.notify_all();
	pthread_join( rt_thread, NULL );
	rt_threadRunning = false;
	common->Printf( "Render thread stopped\n" );
}

/*
==================
R_RenderThreadStats_f
==================
*/
static void R_RenderThreadStats_f( const idCmdArgs &args ) {
	if ( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "reset" ) == 0 ) {
		rt_framesThreaded = rt_framesInline = rt_syncWaits = rt_syncWaitUsec = 0;
		rt_syncReasons.Clear();
		rt_syncReasonCounts.Clear();
		return;
	}
	common->Printf( "Render thread: enabled=%d running=%d threaded=%d inline=%d waits=%d waitMs=%.2f\n",
		r_dkRenderThread.GetBool() ? 1 : 0, rt_threadRunning ? 1 : 0, rt_framesThreaded, rt_framesInline,
		rt_syncWaits, rt_syncWaitUsec * 0.001f );
	for ( int i = 0; i < rt_syncReasons.Num(); i++ ) {
		common->Printf( "Render thread early sync: %s=%d\n", rt_syncReasons[i].c_str(), rt_syncReasonCounts[i] );
	}
}

void R_RenderThread_Init( void ) {
	cmdSystem->AddCommand( "renderThreadStats", R_RenderThreadStats_f, CMD_FL_RENDERER,
		"prints render-thread frame counts and the main-thread entries that waited for it; 'reset' clears them" );
}
