/*
===========================================================================

host_main.cpp - entry point of the host check: the console's main(), renamed by
the build, after a scenario was loaded for the shim to follow.

	quake4_host <scenario file> <output folder> [engine arguments]

===========================================================================
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include "host_scenario.h"

void Host_StartWatchdog( void );

static void OnFault( int sig, siginfo_t *info, void *context ) {
	const ucontext_t *uc = (const ucontext_t *)context;
	printf( "HOST CHECK ERROR: signal %d, address %p, pc %p\n", sig, info->si_addr, (void *)uc->uc_mcontext.rip );
	// everything is built with frame pointers; addr2line -f -C -e build_host/quake4_host.exe <address>
	const unsigned long long *fp = (const unsigned long long *)uc->uc_mcontext.rbp;
	for ( int depth = 0; depth < 48 && fp && ( (unsigned long long)fp & 7 ) == 0 && (unsigned long long)fp > 0x10000; depth++ ) {
		printf( "  #%i %p\n", depth, (void *)fp[1] );
		const unsigned long long *next = (const unsigned long long *)fp[0];
		if ( next <= fp ) {
			break;
		}
		fp = next;
	}
	fflush( stdout );
	_Exit( 128 + sig );
}

int main( int argc, char **argv ) {
	if ( argc < 3 ) {
		fprintf( stderr, "usage: %s <scenario> <output folder> [engine arguments]\n", argv[0] );
		return 2;
	}
	setvbuf( stdout, NULL, _IOLBF, 0 );

	struct sigaction action = {};
	action.sa_sigaction = OnFault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset( &action.sa_mask );
	sigaction( SIGILL, &action, NULL );
	sigaction( SIGSEGV, &action, NULL );
	sigaction( SIGBUS, &action, NULL );
	sigaction( SIGFPE, &action, NULL );
	sigaction( SIGABRT, &action, NULL );

	Shim_LoadScenario( argv[1], argv[2] );
	Host_StartWatchdog();

	// the engine's arguments, with what stands for the program's path first
	argv[2] = argv[0];
	const int result = Switch_Main( argc - 2, argv + 2 );
	const int errors = FakeDeko3D_Errors();
	printf( "host check finished: exit code %d, %d fake deko3D errors\n", result, errors );
	fflush( stdout );
	// static destruction order is the PC's own business, not the console's
	_Exit( errors ? 1 : result );
}
