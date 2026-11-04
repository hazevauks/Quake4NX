/*
===========================================================================

host_commands.cpp - what the host check needs from the engine's headers.

===========================================================================
*/

#include "idlib/precompiled.h"
#pragma hdrstop

#include "sys/EventDisposition.h"
#include "sys/EventRetirement.h"

void Host_AddCommand( const char *text ) {
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, text );
}

/*
Nothing calls these on the console, where the linker drops the code that refers to
them. The PC's linker keeps it, so it needs them to exist.
*/
sysEventTransfer_t Sys_PeekEventDispositionTag( sysEventDispositionTag_t & ) noexcept {
	return sysEventTransfer_t::Refused;
}

sysEventTransfer_t Sys_TakeEventWithDisposition( sysEvent_t &, sysEventDispositionTag_t & ) noexcept {
	return sysEventTransfer_t::Refused;
}

sysEventTransfer_t Sys_PeekEventForRetirement( openq4::NativeInputHead & ) noexcept {
	return sysEventTransfer_t::Refused;
}

sysEventTransfer_t Sys_TakeEventForRetirement( openq4::NativeInputRoute &, const openq4::NativeInputRoute::CancellationPermit &, sysEvent_t &, sysEventDispositionTag_t & ) noexcept {
	return sysEventTransfer_t::Refused;
}

struct srfTriangles_s;
void RB_DrawElementsImmediate( const srfTriangles_s * ) {
}
