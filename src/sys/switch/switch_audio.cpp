/*
===========================================================================

Quake4NX - audio output for OpenAL Soft.

devkitPro's OpenAL Soft plays through SDL2's audio calls, and these seven
functions are all it uses. SDL2 itself would bring in its video driver and with
it all of Mesa, so they are implemented here over the system's audio service.

===========================================================================
*/

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// idlib and libnx both define BIT(); they compute the same value.
#undef BIT
#include <switch.h>
#include <SDL2/SDL_audio.h>

static const int NUM_BUFFERS	= 4;
static const int BUFFER_SAMPLES	= 1024;
static const int BUFFER_BYTES	= BUFFER_SAMPLES * 2 * sizeof( s16 );	// 4 KB, the size the service requires

static AudioOutBuffer		s_buffers[NUM_BUFFERS];
static SDL_AudioCallback	s_callback;
static void *				s_userdata;
static pthread_t			s_thread;
static volatile bool		s_running = false;
static volatile bool		s_paused = true;
static bool					s_opened = false;

static void *AudioThread( void * ) {
	while ( s_running ) {
		AudioOutBuffer *released = NULL;
		u32 count = 0;
		if ( R_FAILED( audoutWaitPlayFinish( &released, &count, 100000000ULL ) ) || !released || count == 0 ) {
			continue;
		}
		if ( s_paused ) {
			memset( released->buffer, 0, BUFFER_BYTES );
		} else {
			s_callback( s_userdata, (Uint8 *)released->buffer, BUFFER_BYTES );
		}
		armDCacheFlush( released->buffer, BUFFER_BYTES );
		released->data_size = BUFFER_BYTES;
		audoutAppendAudioOutBuffer( released );
	}
	return NULL;
}

extern "C" {

int SDL_InitSubSystem( Uint32 flags ) {
	return 0;
}

const char *SDL_GetError( void ) {
	return "audio service failure";
}

int SDL_GetNumAudioDevices( int iscapture ) {
	return iscapture ? 0 : 1;
}

const char *SDL_GetAudioDeviceName( int index, int iscapture ) {
	return "Nintendo Switch";
}

SDL_AudioDeviceID SDL_OpenAudioDevice( const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes ) {
	if ( iscapture || s_opened || R_FAILED( audoutInitialize() ) ) {
		return 0;
	}

	s_callback = desired->callback;
	s_userdata = desired->userdata;

	*obtained = *desired;
	obtained->freq = audoutGetSampleRate();
	obtained->format = AUDIO_S16LSB;
	obtained->channels = 2;
	obtained->samples = BUFFER_SAMPLES;
	obtained->size = BUFFER_BYTES;

	if ( R_FAILED( audoutStartAudioOut() ) ) {
		audoutExit();
		return 0;
	}
	for ( int i = 0; i < NUM_BUFFERS; i++ ) {
		s_buffers[i].next = NULL;
		s_buffers[i].buffer = aligned_alloc( 0x1000, BUFFER_BYTES );
		s_buffers[i].buffer_size = BUFFER_BYTES;
		s_buffers[i].data_size = BUFFER_BYTES;
		s_buffers[i].data_offset = 0;
		memset( s_buffers[i].buffer, 0, BUFFER_BYTES );
		armDCacheFlush( s_buffers[i].buffer, BUFFER_BYTES );
		audoutAppendAudioOutBuffer( &s_buffers[i] );
	}

	s_paused = true;
	s_running = true;
	s_opened = true;
	pthread_create( &s_thread, NULL, AudioThread, NULL );
	return 1;
}

void SDL_PauseAudioDevice( SDL_AudioDeviceID dev, int pause_on ) {
	s_paused = pause_on != 0;
}

void SDL_CloseAudioDevice( SDL_AudioDeviceID dev ) {
	if ( !s_opened ) {
		return;
	}
	s_running = false;
	pthread_join( s_thread, NULL );
	audoutStopAudioOut();
	audoutExit();
	for ( int i = 0; i < NUM_BUFFERS; i++ ) {
		free( s_buffers[i].buffer );
	}
	s_opened = false;
}

}	// extern "C"
