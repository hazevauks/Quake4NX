/*
===========================================================================

Quake4NX - Nintendo Switch platform layer (libnx)

===========================================================================
*/

#ifndef __SWITCH_LOCAL_H__
#define __SWITCH_LOCAL_H__

// Install root on the SD card. Holds q4base/ (the player's retail .pk4 files),
// baseoq4/ (openQ4's own pack, and config and saves) and SD_LOG/ (logs and crash reports).
#ifndef SWITCH_BASE_PATH		// tools/hostcheck has its own
#define SWITCH_BASE_PATH		"sdmc:/switch/quake4"
#endif
#define SWITCH_LOG_PATH			SWITCH_BASE_PATH "/SD_LOG"
#define SWITCH_LOG_FILE			SWITCH_LOG_PATH "/quake4_log.txt"
#define SWITCH_LOG_PREVIOUS		SWITCH_LOG_PATH "/quake4_log_previous.txt"
#define SWITCH_CRASH_FILE		SWITCH_LOG_PATH "/quake4_crash.txt"
#define SWITCH_FATAL_FILE		SWITCH_LOG_PATH "/quake4_fatal.txt"

// Application cores. Core 3 belongs to the system.
//	0	game and render front end
//	1	render back end
//	2	async tick, sound mixer and library threads
static const int SWITCH_CORE_GAME		= 0;
static const int SWITCH_CORE_RENDER		= 1;
static const int SWITCH_CORE_WORKER		= 2;

// Events from input to Sys_GetEvent (engine thread only).
void		Switch_QueueEvent( sysEventType_t type, int value, int value2, int ptrLength, void *ptr );

// switch_main.cpp
void		Switch_FlushLog( void );
bool		Switch_IsLoading( void );
void		Sys_SetLoadingBoost( bool enable );

// switch_threads.cpp
void		Switch_InitThreads( void );
void		Switch_StartAsyncThread( void );
void		Switch_StopAsyncThread( void );
// Core for the next pthread_create only; the default is SWITCH_CORE_WORKER.
void		Switch_SetNextThreadCore( int core );

// switch_input.cpp
void		Switch_PollInput( void );
void		Switch_ApplyDefaultBinds( void );

#endif /* !__SWITCH_LOCAL_H__ */
