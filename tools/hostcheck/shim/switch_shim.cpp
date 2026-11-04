/*
===========================================================================

switch_shim.cpp - runs the port on a PC, from a scenario instead of a player.

Time is virtual: the engine thread moves it, one 60 Hz frame for each time it asks
for the system's events, so the game sees a steady console whatever the PC can do.
Other threads wait for it, as they would for a real clock.

A scenario file has a line per event. A frame is one the game presented, so the
wait for a map to load does not count:

	120 press A
	125 release A
	300 stick L 0 32767		(the stick's x and y)
	900 shot name			(this frame is drawn, to name.png in the output folder)
	905 stats name			(what this frame asked of the GPU, in the output)
	950 cmd map game/airdefense1	(a console command)
	960 touch 640 360		(a finger down, in screen pixels; "touch -1 -1" lifts it)
	970 text hello			(what the system keyboard will answer)
	975 gyro 0 0.5			(turning speeds around z and x)
	976 probe 800 660		(what writes this pixel in the pictures that follow, in the log)
	980 docked				(or: handheld)
	2000 quit

A frame number starting with + counts from the event before it.

===========================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>

#include <atomic>

#include <switch.h>

#include "host_scenario.h"

extern void Host_AddCommand( const char *text );

static const u64 NS_PER_FRAME		= 16666667ULL;
static const u64 NS_PER_CLOCK_READ	= 1000ULL;
static const int MAX_EVENTS			= 8192;
static const int STALL_SECONDS		= 900;		// with no frame presented, the run has failed

typedef struct {
	int		frame;
	char	action[16];
	char	arg[64];
	char	line[192];
	int		x, y;
} event_t;

static event_t		s_events[MAX_EVENTS];
static int			s_numEvents = 0;
static int			s_nextEvent = 0;
static std::atomic<int>	s_frame( 0 );
static bool			s_quit = false;

static std::atomic<u64>	s_virtualNs( 1000000ULL );

static u64			s_buttons = 0;
static u64			s_buttonsLast = 0;
static HidAnalogStickState s_sticks[2] = {};
static int			s_touchX = -1, s_touchY = -1;
static int			s_probeX = -1, s_probeY = -1;
static float		s_gyroZ = 0.0f, s_gyroX = 0.0f;
static char			s_keyboardText[192] = "";
static bool			s_docked = false;
static bool			s_consoleOpen = false;

static const char *	s_outputDir = ".";
static char			s_shotName[64] = "";
static char			s_statsName[64] = "";
static std::atomic<int>	s_stallSeconds( 0 );

/*
================================================================================
Scenario
================================================================================
*/
void Shim_LoadScenario( const char *path, const char *outputDir ) {
	// the game changes directory as it starts
	static char outputPath[4096];
	s_outputDir = realpath( outputDir, outputPath );
	if ( !s_outputDir ) {
		fprintf( stderr, "no output folder: %s\n", outputDir );
		exit( 2 );
	}
	FILE *f = fopen( path, "r" );
	if ( !f ) {
		fprintf( stderr, "no scenario: %s\n", path );
		exit( 2 );
	}
	int previous = 0;
	char line[256];
	while ( s_numEvents < MAX_EVENTS && fgets( line, sizeof( line ), f ) ) {
		event_t *e = &s_events[s_numEvents];
		char when[16];
		if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' ) {
			continue;
		}
		e->arg[0] = 0;
		if ( sscanf( line, "%15s %15s %63s %d %d", when, e->action, e->arg, &e->x, &e->y ) >= 2 ) {
			const char *rest = strstr( line, e->action ) + strlen( e->action );
			e->frame = when[0] == '+' ? previous + atoi( when + 1 ) : atoi( when );
			previous = e->frame;
			snprintf( e->line, sizeof( e->line ), "%s", rest );
			s_numEvents++;
		}
	}
	fclose( f );
}

const char *Host_OutputDir( void ) {
	return s_outputDir;
}

void Host_ProbePixel( int *x, int *y ) {
	*x = s_probeX;
	*y = s_probeY;
}

bool Host_ShotPending( void ) {
	return s_shotName[0] != 0;
}

const char *Host_TakeShotName( void ) {
	static char name[64];
	if ( !s_shotName[0] ) {
		return NULL;
	}
	strcpy( name, s_shotName );
	s_shotName[0] = 0;
	return name;
}

void Host_FrameStats( int draws, int indexes ) {
	if ( s_statsName[0] ) {
		struct mallinfo info = mallinfo();
		printf( "stats %s: frame %i, %i draws, %i indexes, %llu MB allocated\n", s_statsName, s_frame.load(), draws, indexes,
			(unsigned long long)( (u64)(unsigned)info.uordblks + (u64)(unsigned)info.hblkhd ) >> 20 );
		fflush( stdout );
		s_statsName[0] = 0;
	}
	s_frame++;
	s_stallSeconds = 0;
}

static u64 ButtonMask( const char *name ) {
	static const struct { const char *name; u64 mask; } table[] = {
		{ "A", HidNpadButton_A }, { "B", HidNpadButton_B }, { "X", HidNpadButton_X }, { "Y", HidNpadButton_Y },
		{ "L", HidNpadButton_L }, { "R", HidNpadButton_R }, { "ZL", HidNpadButton_ZL }, { "ZR", HidNpadButton_ZR },
		{ "PLUS", HidNpadButton_Plus }, { "MINUS", HidNpadButton_Minus }, { "L3", HidNpadButton_StickL }, { "R3", HidNpadButton_StickR },
		{ "UP", HidNpadButton_Up }, { "DOWN", HidNpadButton_Down }, { "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right }
	};
	for ( size_t i = 0; i < sizeof( table ) / sizeof( table[0] ); i++ ) {
		if ( !strcmp( table[i].name, name ) ) {
			return table[i].mask;
		}
	}
	fprintf( stderr, "scenario: no button %s\n", name );
	exit( 2 );
}

static void RunEvents( void ) {
	while ( s_nextEvent < s_numEvents && s_events[s_nextEvent].frame <= s_frame.load() ) {
		const event_t *e = &s_events[s_nextEvent++];
		if ( !strcmp( e->action, "press" ) ) {
			s_buttons |= ButtonMask( e->arg );
		} else if ( !strcmp( e->action, "release" ) ) {
			s_buttons &= ~ButtonMask( e->arg );
		} else if ( !strcmp( e->action, "stick" ) ) {
			HidAnalogStickState *s = &s_sticks[e->arg[0] == 'R'];
			s->x = e->x;
			s->y = e->y;
		} else if ( !strcmp( e->action, "shot" ) ) {
			snprintf( s_shotName, sizeof( s_shotName ), "%s", e->arg );
		} else if ( !strcmp( e->action, "stats" ) ) {
			snprintf( s_statsName, sizeof( s_statsName ), "%s", e->arg );
		} else if ( !strcmp( e->action, "cmd" ) ) {
			Host_AddCommand( e->line );
		} else if ( !strcmp( e->action, "probe" ) ) {
			s_probeX = atoi( e->arg );
			s_probeY = e->x;
		} else if ( !strcmp( e->action, "touch" ) ) {
			s_touchX = atoi( e->arg );
			s_touchY = e->x;
		} else if ( !strcmp( e->action, "gyro" ) ) {
			s_gyroZ = (float)atof( e->arg );
			s_gyroX = (float)e->x / 1000.0f;
		} else if ( !strcmp( e->action, "text" ) ) {
			snprintf( s_keyboardText, sizeof( s_keyboardText ), "%s", e->arg );
		} else if ( !strcmp( e->action, "docked" ) ) {
			s_docked = true;
		} else if ( !strcmp( e->action, "handheld" ) ) {
			s_docked = false;
		} else if ( !strcmp( e->action, "quit" ) ) {
			s_quit = true;
		} else {
			fprintf( stderr, "scenario: no action %s\n", e->action );
			exit( 2 );
		}
	}
}

/*
================================================================================
Time

The engine thread moves the clock. Any other thread waits for it, and moves it
itself when it has waited a while on a PC clock: the engine thread may be waiting for
that thread, and on a console the time would have passed.
================================================================================
*/
static pthread_t	s_engineThread;
static bool			s_engineThreadKnown = false;

u64 armGetSystemTick( void ) {
	return s_virtualNs.fetch_add( NS_PER_CLOCK_READ ) + NS_PER_CLOCK_READ;
}

u64 armGetSystemTickFreq( void ) {
	return 1000000000ULL;
}

u64 armTicksToNs( u64 ticks ) {
	return ticks;
}

u64 armNsToTicks( u64 ns ) {
	return ns;
}

void svcSleepThread( s64 ns ) {
	if ( ns <= 0 ) {
		return;
	}
	if ( !s_engineThreadKnown || pthread_equal( pthread_self(), s_engineThread ) ) {
		s_virtualNs += (u64)ns;
		return;
	}
	const u64 target = s_virtualNs.load() + (u64)ns;
	int waited = 0;
	while ( s_virtualNs.load() < target ) {
		usleep( 200 );
		if ( ++waited > 100 ) {
			u64 now = s_virtualNs.load();
			while ( now < target && !s_virtualNs.compare_exchange_weak( now, target ) ) {
			}
			break;
		}
	}
}

/*
================================================================================
Applet, process
================================================================================
*/
bool appletMainLoop( void ) {
	if ( !s_engineThreadKnown ) {
		s_engineThread = pthread_self();
		s_engineThreadKnown = true;
	}
	s_virtualNs += NS_PER_FRAME;
	return !s_quit;
}

Result appletSetCpuBoostMode( ApmCpuBoostMode mode ) {
	return 0;
}

AppletOperationMode appletGetOperationMode( void ) {
	return s_docked ? AppletOperationMode_Console : AppletOperationMode_Handheld;
}

AppletFocusState appletGetFocusState( void ) {
	return AppletFocusState_InFocus;
}

static bool s_apmOpen = false;

Result apmInitialize( void ) {
	s_apmOpen = true;
	return 0;
}

void apmExit( void ) {
	s_apmOpen = false;
}

// without the service open this fails, as it does on the console
Result apmSetPerformanceConfiguration( ApmPerformanceMode mode, u32 config ) {
	return s_apmOpen ? 0 : 0xE401;
}

Result apmGetPerformanceConfiguration( ApmPerformanceMode mode, u32 *config ) {
	*config = 0;
	return 0;
}

void *nwindowGetDefault( void ) {
	return NULL;
}

void armDCacheFlush( void *addr, size_t size ) {
}

Result svcGetInfo( u64 *out, u32 type, Handle handle, u64 sub ) {
	if ( type == InfoType_TotalMemorySize ) {
		*out = 3285ULL << 20;	// what an application gets
	} else {
		struct mallinfo info = mallinfo();
		*out = (u64)(unsigned)info.uordblks + (u64)(unsigned)info.hblkhd;
	}
	return 0;
}

Result svcQueryMemory( MemoryInfo *info, u32 *pageInfo, u64 addr ) {
	memset( info, 0, sizeof( *info ) );
	*pageInfo = 0;
	return 1;
}

void svcExitProcess( void ) {
	_Exit( 3 );
}

Result svcSetThreadCoreMask( Handle thread, s32 preferred, u32 affinity ) {
	return 0;
}

Handle threadGetCurHandle( void ) {
	return 0;
}

struct in_addr __nxlink_host;

int nxlinkStdio( void ) {
	return -1;
}

Result socketInitializeDefault( void ) {
	return 0;
}

void socketExit( void ) {
}

Result romfsInit( void ) {
	return 0;
}

Result romfsExit( void ) {
	return 0;
}

Result romfsMountFromFsdev( const char *path, u64 offset, const char *name ) {
	return 0;
}

Result envSetNextLoad( const char *path, const char *argv ) {
	printf( "next load: %s\n", argv );
	return 0;
}

void randomGet( void *buf, size_t len ) {
	memset( buf, 0x5a, len );
}

void *consoleInit( void *console ) {
	s_consoleOpen = true;
	return NULL;
}

void consoleUpdate( void *console ) {
	fflush( stdout );
}

void consoleExit( void *console ) {
	s_consoleOpen = false;
}

/*
================================================================================
Controller
================================================================================
*/
void padConfigureInput( u32 maxPlayers, u32 styles ) {
}

void padInitializeDefault( PadState *pad ) {
}

void padUpdate( PadState *pad ) {
	s_buttonsLast = s_buttons;
	RunEvents();
}

u64 padGetButtons( const PadState *pad ) {
	return s_buttons;
}

u64 padGetButtonsDown( const PadState *pad ) {
	// a message that waits for a button on the console's own screen: nobody is there
	if ( s_consoleOpen ) {
		return HidNpadButton_Plus;
	}
	return s_buttons & ~s_buttonsLast;
}

u32 padGetStyleSet( const PadState *pad ) {
	return s_docked ? HidNpadStyleTag_NpadFullKey : HidNpadStyleTag_NpadHandheld;
}

HidAnalogStickState padGetStickPos( const PadState *pad, u32 id ) {
	return s_sticks[id & 1];
}

Result hidGetSixAxisSensorHandles( HidSixAxisSensorHandle *handles, s32 count, HidNpadIdType id, u32 style ) {
	for ( int i = 0; i < count; i++ ) {
		handles[i].value = 0;
	}
	return 0;
}

Result hidStartSixAxisSensor( HidSixAxisSensorHandle handle ) {
	return 0;
}

size_t hidGetSixAxisSensorStates( HidSixAxisSensorHandle handle, HidSixAxisSensorState *states, size_t count ) {
	memset( states, 0, sizeof( *states ) );
	states->angular_velocity.z = s_gyroZ;
	states->angular_velocity.x = s_gyroX;
	return 1;
}

Result hidInitializeVibrationDevices( HidVibrationDeviceHandle *handles, s32 count, HidNpadIdType id, u32 style ) {
	return 0;
}

Result hidSendVibrationValues( const HidVibrationDeviceHandle *handles, const HidVibrationValue *values, s32 count ) {
	return 0;
}

void hidInitializeTouchScreen( void ) {
}

size_t hidGetTouchScreenStates( HidTouchScreenState *states, size_t count ) {
	states->count = 0;
	if ( s_touchX >= 0 ) {
		states->count = 1;
		states->touches[0].x = s_touchX;
		states->touches[0].y = s_touchY;
	}
	return 1;
}

/*
================================================================================
System keyboard
================================================================================
*/
Result swkbdCreate( SwkbdConfig *config, s32 maxDictWords ) {
	return 0;
}

void swkbdConfigMakePresetDefault( SwkbdConfig *config ) {
}

void swkbdConfigSetHeaderText( SwkbdConfig *config, const char *text ) {
}

Result swkbdShow( SwkbdConfig *config, char *out, size_t outSize ) {
	snprintf( out, outSize, "%s", s_keyboardText );
	return 0;
}

void swkbdClose( SwkbdConfig *config ) {
}

/*
================================================================================
Watchdog: a run that stops presenting frames has failed, even if it never ends
================================================================================
*/
static void *WatchdogThread( void * ) {
	while ( 1 ) {
		sleep( 1 );
		if ( ++s_stallSeconds > STALL_SECONDS ) {
			printf( "HOST CHECK ERROR: no frame presented in %i seconds, at frame %i\n", STALL_SECONDS, s_frame.load() );
			fflush( stdout );
			_Exit( 4 );
		}
	}
	return NULL;
}

extern "C" int __real_pthread_create( pthread_t *thread, const pthread_attr_t *attr, void *( *start )( void * ), void *arg );

void Host_StartWatchdog( void ) {
	pthread_t thread;
	__real_pthread_create( &thread, NULL, WatchdogThread, NULL );
}
