/*
===========================================================================

Quake4NX - networking over the system's BSD sockets (IPv4 only).

===========================================================================
*/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../idlib/precompiled.h"

#undef BIT
#include <switch.h>

idCVar net_ip( "net_ip", "localhost", CVAR_SYSTEM, "local IP address" );
idCVar net_port( "net_port", "", CVAR_SYSTEM | CVAR_INTEGER, "local IP port number" );

static bool s_socketsUp = false;
static in_addr_t s_localAddress = 0;

static void NetadrToSockadr( const netadr_t *a, struct sockaddr_in *s ) {
	memset( s, 0, sizeof( *s ) );
	s->sin_family = AF_INET;
	if ( a->type == NA_BROADCAST ) {
		s->sin_addr.s_addr = INADDR_BROADCAST;
	} else if ( a->type == NA_IP || a->type == NA_LOOPBACK ) {
		memcpy( &s->sin_addr, a->ip, 4 );
	}
	s->sin_port = htons( (unsigned short)a->port );
}

static void SockadrToNetadr( const struct sockaddr_in *s, netadr_t *a ) {
	const unsigned int ip = s->sin_addr.s_addr;
	memset( a, 0, sizeof( *a ) );
	memcpy( a->ip, &ip, 4 );
	a->port = ntohs( s->sin_port );
	a->type = ( ip == htonl( INADDR_LOOPBACK ) ) ? NA_LOOPBACK : NA_IP;
}

bool Sys_StringToNetAdr( const char *s, netadr_t *a, bool doDNSResolve ) {
	char host[256];
	idStr::Copynz( host, s, sizeof( host ) );

	int port = 0;
	char *colon = strchr( host, ':' );
	if ( colon ) {
		*colon = 0;
		port = atoi( colon + 1 );
	}

	struct in_addr addr;
	if ( !inet_aton( host, &addr ) ) {
		if ( !idStr::Icmp( host, "localhost" ) ) {
			addr.s_addr = htonl( INADDR_LOOPBACK );
		} else if ( doDNSResolve && s_socketsUp ) {
			const struct hostent *h = gethostbyname( host );
			if ( !h || !h->h_addr_list[0] ) {
				return false;
			}
			memcpy( &addr, h->h_addr_list[0], 4 );
		} else {
			return false;
		}
	}

	memset( a, 0, sizeof( *a ) );
	memcpy( a->ip, &addr, 4 );
	a->port = port;
	a->type = ( addr.s_addr == htonl( INADDR_LOOPBACK ) ) ? NA_LOOPBACK : NA_IP;
	return true;
}

int Sys_StringToNetAdrCandidates( const char *s, netadr_t *candidates, int maxCandidates, bool doDNSResolve ) {
	if ( maxCandidates < 1 || !Sys_StringToNetAdr( s, &candidates[0], doDNSResolve ) ) {
		return 0;
	}
	return 1;
}

const char *Sys_NetAdrToString( const netadr_t a ) {
	static char s[64];
	if ( a.type == NA_LOOPBACK ) {
		if ( a.port ) {
			idStr::snPrintf( s, sizeof( s ), "localhost:%i", a.port );
		} else {
			idStr::snPrintf( s, sizeof( s ), "localhost" );
		}
	} else if ( a.type == NA_IP ) {
		idStr::snPrintf( s, sizeof( s ), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port );
	} else {
		idStr::snPrintf( s, sizeof( s ), "(bad address)" );
	}
	return s;
}

// a private network address counts as local
bool Sys_IsLANAddress( const netadr_t adr ) {
	if ( adr.type == NA_LOOPBACK ) {
		return true;
	}
	if ( adr.type != NA_IP ) {
		return false;
	}
	return adr.ip[0] == 10 || ( adr.ip[0] == 172 && ( adr.ip[1] & 0xf0 ) == 16 ) || ( adr.ip[0] == 192 && adr.ip[1] == 168 );
}

bool Sys_GetLocalIPAddress( netadr_t &out ) {
	if ( !s_socketsUp || !s_localAddress ) {
		return false;
	}
	memset( &out, 0, sizeof( out ) );
	memcpy( out.ip, &s_localAddress, 4 );
	out.type = NA_IP;
	return true;
}

bool Sys_CompareNetAdrBase( const netadr_t a, const netadr_t b ) {
	if ( a.type != b.type ) {
		return false;
	}
	if ( a.type == NA_LOOPBACK ) {
		return true;
	}
	if ( a.type == NA_IP ) {
		return memcmp( a.ip, b.ip, 4 ) == 0;
	}
	return false;
}

void Sys_InitNetworking( void ) {
	// nxlink may have brought the socket service up already
	if ( __nxlink_host.s_addr != 0 ) {
		s_socketsUp = true;
	} else {
		s_socketsUp = R_SUCCEEDED( socketInitializeDefault() );
	}
	if ( s_socketsUp ) {
		s_localAddress = gethostid();
		struct in_addr addr;
		addr.s_addr = s_localAddress;
		common->Printf( "network: local address %s\n", inet_ntoa( addr ) );
		net_ip.SetString( inet_ntoa( addr ) );
	} else {
		common->Printf( "network: no socket service\n" );
	}
}

void Sys_ShutdownNetworking( void ) {
	if ( s_socketsUp && __nxlink_host.s_addr == 0 ) {
		socketExit();
	}
	s_socketsUp = false;
}

void Sys_NetImpairmentReport( void ) {
	common->Printf( "net_impairment is not available on this platform\n" );
}

/*
==================
idPort
==================
*/
static const int MAX_PORT_TRIES = 32;

static int IPSocket( int port, netadr_t *bound_to ) {
	const int s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if ( s < 0 ) {
		return 0;
	}
	if ( fcntl( s, F_SETFL, fcntl( s, F_GETFL, 0 ) | O_NONBLOCK ) < 0 ) {
		close( s );
		return 0;
	}
	int on = 1;
	setsockopt( s, SOL_SOCKET, SO_BROADCAST, &on, sizeof( on ) );

	struct sockaddr_in address;
	memset( &address, 0, sizeof( address ) );
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons( (unsigned short)port );
	if ( bind( s, (struct sockaddr *)&address, sizeof( address ) ) < 0 ) {
		close( s );
		return 0;
	}

	socklen_t len = sizeof( address );
	getsockname( s, (struct sockaddr *)&address, &len );
	if ( bound_to ) {
		memset( bound_to, 0, sizeof( *bound_to ) );
		memcpy( bound_to->ip, &s_localAddress, 4 );
		bound_to->port = ntohs( address.sin_port );
		bound_to->type = NA_IP;
	}
	return s;
}

idPort::idPort() {
	netSocket = 0;
	netSocket6 = 0;
	platformData = NULL;
	memset( &bound_to, 0, sizeof( bound_to ) );
	packetsRead = bytesRead = packetsWritten = bytesWritten = 0;
}

idPort::~idPort() {
	Close();
}

bool idPort::InitForPort( int portNumber ) {
	if ( !s_socketsUp ) {
		return false;
	}
	if ( portNumber == PORT_ANY ) {
		netSocket = IPSocket( 0, &bound_to );
	} else {
		for ( int i = 0; i < MAX_PORT_TRIES && !netSocket; i++ ) {
			netSocket = IPSocket( portNumber + i, &bound_to );
		}
	}
	return netSocket != 0;
}

void idPort::Close() {
	if ( netSocket ) {
		close( netSocket );
		netSocket = 0;
		memset( &bound_to, 0, sizeof( bound_to ) );
	}
}

bool idPort::GetPacket( netadr_t &from, void *data, int &size, int maxSize ) {
	if ( !netSocket ) {
		return false;
	}
	struct sockaddr_in addr;
	socklen_t addrLen = sizeof( addr );
	const int ret = recvfrom( netSocket, data, maxSize, 0, (struct sockaddr *)&addr, &addrLen );
	if ( ret < 0 ) {
		return false;
	}
	SockadrToNetadr( &addr, &from );
	size = ret;
	packetsRead++;
	bytesRead += ret;
	return true;
}

bool idPort::GetPacketBlocking( netadr_t &from, void *data, int &size, int maxSize, int timeout ) {
	if ( !netSocket ) {
		return false;
	}
	if ( timeout >= 0 ) {
		fd_set set;
		FD_ZERO( &set );
		FD_SET( netSocket, &set );
		struct timeval tv;
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = ( timeout % 1000 ) * 1000;
		if ( select( netSocket + 1, &set, NULL, NULL, &tv ) <= 0 ) {
			return false;
		}
	}
	return GetPacket( from, data, size, maxSize );
}

void idPort::SendPacket( const netadr_t to, const void *data, int size ) {
	if ( !netSocket || ( to.type != NA_BROADCAST && to.type != NA_IP && to.type != NA_LOOPBACK ) ) {
		return;
	}
	struct sockaddr_in addr;
	NetadrToSockadr( &to, &addr );
	if ( sendto( netSocket, data, size, 0, (struct sockaddr *)&addr, sizeof( addr ) ) >= 0 ) {
		packetsWritten++;
		bytesWritten += size;
	}
}

/*
==================
idTCP
==================
*/
idTCP::idTCP() {
	fd = 0;
	memset( &address, 0, sizeof( address ) );
}

idTCP::~idTCP() {
	Close();
}

bool idTCP::Init( const char *host, int port ) {
	if ( !s_socketsUp || !Sys_StringToNetAdr( host, &address, true ) ) {
		return false;
	}
	if ( !address.port ) {
		address.port = port;
	}
	if ( ( fd = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 ) {
		fd = 0;
		return false;
	}
	struct sockaddr_in s;
	NetadrToSockadr( &address, &s );
	if ( connect( fd, (struct sockaddr *)&s, sizeof( s ) ) < 0 ) {
		Close();
		return false;
	}
	fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK );
	return true;
}

void idTCP::Close() {
	if ( fd ) {
		close( fd );
	}
	fd = 0;
}

int idTCP::Read( void *data, int size ) {
	if ( !fd ) {
		return -1;
	}
	const int nbytes = read( fd, data, size );
	if ( nbytes == -1 ) {
		if ( errno == EAGAIN ) {
			return 0;
		}
		Close();
		return -1;
	}
	if ( nbytes == 0 ) {
		Close();
		return -1;
	}
	return nbytes;
}

int idTCP::Write( void *data, int size ) {
	if ( !fd ) {
		return -1;
	}
	const int nbytes = write( fd, data, size );
	if ( nbytes == -1 ) {
		Close();
		return -1;
	}
	return nbytes;
}
