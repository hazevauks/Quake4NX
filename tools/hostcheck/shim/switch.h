/*
===========================================================================

switch.h - just enough of libnx for the port's own code to run on a PC.

With this header in place of the real one, src/sys/switch/ and src/renderer/deko3d/
compile unchanged for the machine that builds them. See tools/hostcheck/README.md.

===========================================================================
*/

#ifndef HOSTCHECK_SWITCH_H
#define HOSTCHECK_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <arpa/inet.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;
typedef int8_t		s8;
typedef int16_t		s16;
typedef int32_t		s32;
typedef int64_t		s64;
typedef u32			Result;
typedef u32			Handle;

#define R_SUCCEEDED( rc )	( ( rc ) == 0 )
#define R_FAILED( rc )		( ( rc ) != 0 )
#define BIT( n )			( 1U << ( n ) )
#define BITL( n )			( 1UL << ( n ) )

// time, virtual: see switch_shim.cpp
u64		armGetSystemTick( void );
u64		armGetSystemTickFreq( void );
u64		armTicksToNs( u64 ticks );
u64		armNsToTicks( u64 ns );
void	armDCacheFlush( void *addr, size_t size );
void	svcSleepThread( s64 ns );

// process
#define CUR_PROCESS_HANDLE	0
enum { InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7 };
enum { Perm_R = 1 };
enum { MemType_Unmapped = 0 };
typedef struct { u64 addr; u64 size; u32 type; u32 attr; u32 perm; u32 ipc_refcount; u32 device_refcount; u32 padding; } MemoryInfo;
Result	svcGetInfo( u64 *out, u32 type, Handle handle, u64 sub );
Result	svcQueryMemory( MemoryInfo *info, u32 *pageInfo, u64 addr );
void	svcExitProcess( void );
Result	svcSetThreadCoreMask( Handle thread, s32 preferred, u32 affinity );
Handle	threadGetCurHandle( void );
Result	socketInitializeDefault( void );
void	socketExit( void );
extern struct in_addr __nxlink_host;
int		nxlinkStdio( void );
Result	romfsInit( void );
Result	romfsExit( void );
Result	romfsMountFromFsdev( const char *path, u64 offset, const char *name );
Result	envSetNextLoad( const char *path, const char *argv );
void	randomGet( void *buf, size_t len );

// what a crash handler is given; nothing here calls it
typedef union { u64 x; } CpuRegister;
typedef struct {
	u32			error_desc;
	CpuRegister	cpu_gprs[29];
	CpuRegister	fp, lr, sp, pc;
	CpuRegister	far;
	u32			esr;
} ThreadExceptionDump;
enum { ThreadExceptionDesc_MisalignedPC = 0x102, ThreadExceptionDesc_MisalignedSP = 0x103 };

// console text
void	*consoleInit( void *console );
void	consoleUpdate( void *console );
void	consoleExit( void *console );

// applet and clocks
typedef enum { ApmCpuBoostMode_Normal = 0, ApmCpuBoostMode_FastLoad = 1 } ApmCpuBoostMode;
typedef enum { ApmPerformanceMode_Normal = 0, ApmPerformanceMode_Boost = 1 } ApmPerformanceMode;
typedef enum { AppletOperationMode_Handheld = 0, AppletOperationMode_Console = 1 } AppletOperationMode;
typedef enum { AppletFocusState_InFocus = 1, AppletFocusState_OutOfFocus = 2 } AppletFocusState;
bool	appletMainLoop( void );
Result	appletSetCpuBoostMode( ApmCpuBoostMode mode );
AppletOperationMode appletGetOperationMode( void );
AppletFocusState appletGetFocusState( void );
Result	apmInitialize( void );
void	apmExit( void );
Result	apmSetPerformanceConfiguration( ApmPerformanceMode mode, u32 config );
Result	apmGetPerformanceConfiguration( ApmPerformanceMode mode, u32 *config );

// the screen
void	*nwindowGetDefault( void );

// locks
typedef pthread_mutex_t Mutex;
#define mutexInit( m )		pthread_mutex_init( m, NULL )
#define mutexLock( m )		pthread_mutex_lock( m )
#define mutexUnlock( m )	pthread_mutex_unlock( m )

// controller
#define HOST_BIT( n )	( 1ULL << ( n ) )
enum {
	HidNpadButton_A			= HOST_BIT( 0 ),
	HidNpadButton_B			= HOST_BIT( 1 ),
	HidNpadButton_X			= HOST_BIT( 2 ),
	HidNpadButton_Y			= HOST_BIT( 3 ),
	HidNpadButton_StickL	= HOST_BIT( 4 ),
	HidNpadButton_StickR	= HOST_BIT( 5 ),
	HidNpadButton_L			= HOST_BIT( 6 ),
	HidNpadButton_R			= HOST_BIT( 7 ),
	HidNpadButton_ZL		= HOST_BIT( 8 ),
	HidNpadButton_ZR		= HOST_BIT( 9 ),
	HidNpadButton_Plus		= HOST_BIT( 10 ),
	HidNpadButton_Minus		= HOST_BIT( 11 ),
	HidNpadButton_Left		= HOST_BIT( 12 ),
	HidNpadButton_Up		= HOST_BIT( 13 ),
	HidNpadButton_Right		= HOST_BIT( 14 ),
	HidNpadButton_Down		= HOST_BIT( 15 )
};
enum {
	HidNpadStyleTag_NpadFullKey		= BIT( 0 ),
	HidNpadStyleTag_NpadHandheld	= BIT( 1 ),
	HidNpadStyleTag_NpadJoyDual		= BIT( 2 ),
	HidNpadStyleSet_NpadStandard	= 7
};
typedef enum { HidNpadIdType_No1 = 0, HidNpadIdType_Handheld = 0x20 } HidNpadIdType;
typedef struct { int unused; } PadState;
typedef struct { s32 x, y; } HidAnalogStickState;
#define JOYSTICK_MAX	32767
void	padConfigureInput( u32 maxPlayers, u32 styles );
void	padInitializeDefault( PadState *pad );
void	padUpdate( PadState *pad );
u64		padGetButtons( const PadState *pad );
u64		padGetButtonsDown( const PadState *pad );
u32		padGetStyleSet( const PadState *pad );
HidAnalogStickState padGetStickPos( const PadState *pad, u32 id );

// motion
typedef struct { u32 value; } HidSixAxisSensorHandle;
typedef struct { float x, y, z; } HidVector;
typedef struct { HidVector acceleration; HidVector angular_velocity; } HidSixAxisSensorState;
Result	hidGetSixAxisSensorHandles( HidSixAxisSensorHandle *handles, s32 count, HidNpadIdType id, u32 style );
Result	hidStartSixAxisSensor( HidSixAxisSensorHandle handle );
size_t	hidGetSixAxisSensorStates( HidSixAxisSensorHandle handle, HidSixAxisSensorState *states, size_t count );

// rumble
typedef struct { u32 value; } HidVibrationDeviceHandle;
typedef struct { float amp_low, freq_low, amp_high, freq_high; } HidVibrationValue;
Result	hidInitializeVibrationDevices( HidVibrationDeviceHandle *handles, s32 count, HidNpadIdType id, u32 style );
Result	hidSendVibrationValues( const HidVibrationDeviceHandle *handles, const HidVibrationValue *values, s32 count );

// touch screen
typedef struct { u32 x, y; } HidTouchState;
typedef struct { s32 count; HidTouchState touches[16]; } HidTouchScreenState;
void	hidInitializeTouchScreen( void );
size_t	hidGetTouchScreenStates( HidTouchScreenState *states, size_t count );

// system keyboard: a scenario answers it
typedef struct { int unused; } SwkbdConfig;
Result	swkbdCreate( SwkbdConfig *config, s32 maxDictWords );
void	swkbdConfigMakePresetDefault( SwkbdConfig *config );
void	swkbdConfigSetHeaderText( SwkbdConfig *config, const char *text );
Result	swkbdShow( SwkbdConfig *config, char *out, size_t outSize );
void	swkbdClose( SwkbdConfig *config );

#ifdef __cplusplus
}
#endif

#endif // HOSTCHECK_SWITCH_H
