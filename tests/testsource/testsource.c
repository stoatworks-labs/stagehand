/*
	A synthetic stagehand source.

	It exists so the library can be tested against something that is entirely
	ours, entirely deterministic, and has no external dependency at all -- no
	emulator, no game data, nothing to download. It also stands as the worked
	example of what implementing the ABI looks like.

	**Every element of its test pattern pins one specific host mistake**, which
	is the only reason to draw a pattern rather than a flat colour:

	  - the four corner primaries catch a channel swap (BGRA read as RGBA) and
	    a vertical flip, because all four differ and none is symmetric;
	  - a bar whose column IS the tick count catches a stale frame, a
	    double-step, and a host that grants time it was not given;
	  - the alpha byte is written opaque, so a host that passes an undefined
	    alpha through has something to fail against.
*/
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "stagehand/SourceAbi.h"

#define TS_WIDTH  64
#define TS_HEIGHT 32
#define TS_BYTES  ( TS_WIDTH * TS_HEIGHT * 4 )
#define TS_RATE   50

typedef struct Source
{
	pthread_t       thread;
	bool            threadLive;

	atomic_int      state;
	atomic_bool     quit;
	atomic_uint     budget;  /* ticks released by the host  */
	atomic_uint     spent;   /* ticks the source has used   */
	atomic_uint     seq;
	atomic_uint     parks;

	pthread_mutex_t gate;
	pthread_cond_t  gateCv;

	/* Two buffers and an index: the host must never block to read one. */
	uint8_t         slots[ 2 ][ TS_BYTES ];
	atomic_int      ready;
	int             writing;

	atomic_int      events; /* running total of Event(code, value) */

	char            status[ 256 ];
	bool            everOpened;
} Source;

static Source g;

static void paint( uint8_t* out, uint32_t tick, int events )
{
	/* Opaque black, not zeroed: a source must publish a real alpha byte. A
	   zeroed buffer is transparent, and a host that passed it through would
	   render an invisible layer -- which looks exactly like a host that is
	   doing nothing at all. */
	for( size_t i = 0; i < TS_BYTES; i += 4 )
	{
		out[ i ] = 0; out[ i + 1 ] = 0; out[ i + 2 ] = 0; out[ i + 3 ] = 255;
	}

	/* Written bottom-up: screen row y lives at buffer row (H-1-y). */
	#define PUT( x, y, b, gr, r )                                              \
		do {                                                                   \
			uint8_t* p = out + ( ( size_t )( TS_HEIGHT - 1 - ( y ) ) * TS_WIDTH \
								 + ( x ) ) * 4;                                \
			p[ 0 ] = ( b ); p[ 1 ] = ( gr ); p[ 2 ] = ( r ); p[ 3 ] = 255;     \
		} while( 0 )

	/* The moving bar first, so the corners always win where they overlap. */
	const uint32_t column = tick % TS_WIDTH;
	for( int y = 0; y < TS_HEIGHT; ++y )
		PUT( column, y, 0, 255, 255 ); /* yellow */

	/* An events marker, so a host can prove input arrived at all. */
	if( events > 0 )
		for( int y = 0; y < 4; ++y )
			for( int x = 0; x < 4; ++x )
				PUT( TS_WIDTH / 2 + x, TS_HEIGHT / 2 + y, 255, 0, 255 );

	PUT( 0, 0, 0, 0, 255 );                          /* top-left     red   */
	PUT( TS_WIDTH - 1, 0, 0, 255, 0 );               /* top-right    green */
	PUT( 0, TS_HEIGHT - 1, 255, 0, 0 );              /* bottom-left  blue  */
	PUT( TS_WIDTH - 1, TS_HEIGHT - 1, 255, 255, 255 ); /* bottom-right white */

	#undef PUT
}

static void* run( void* unused )
{
	(void)unused;

	for( ;; )
	{
		/* Wait for the host to release time. Timed, and signalled without the
		   mutex, so a missed wakeup costs 2 ms and can never wedge the host. */
		bool counted = false;
		for( ;; )
		{
			if( atomic_load( &g.quit ) )
				return NULL;
			if( atomic_load( &g.budget ) > atomic_load( &g.spent ) )
				break;

			if( !counted )
			{
				atomic_fetch_add( &g.parks, 1 );
				counted = true;
			}

			struct timespec deadline;
			struct timeval  now;
			gettimeofday( &now, NULL );
			deadline.tv_sec  = now.tv_sec;
			deadline.tv_nsec = ( now.tv_usec + 2000 ) * 1000;
			if( deadline.tv_nsec >= 1000000000L )
			{
				deadline.tv_sec += 1;
				deadline.tv_nsec -= 1000000000L;
			}
			pthread_mutex_lock( &g.gate );
			pthread_cond_timedwait( &g.gateCv, &g.gate, &deadline );
			pthread_mutex_unlock( &g.gate );
		}

		const uint32_t tick = atomic_fetch_add( &g.spent, 1 ) + 1;

		paint( g.slots[ g.writing ], tick, atomic_load( &g.events ) );
		atomic_fetch_add( &g.seq, 1 );

		int previous = atomic_exchange( &g.ready, g.writing );
		g.writing    = ( previous >= 0 ) ? previous : ( g.writing ^ 1 );

		atomic_store( &g.state, STAGEHAND_RUNNING );
	}
}

/* ------------------------------------------------------------------ */

static const char* option( const StagehandOption* options, int count, const char* key )
{
	for( int i = 0; i < count; ++i )
		if( options[ i ].key && strcmp( options[ i ].key, key ) == 0 )
			return options[ i ].value;
	return NULL;
}

static int api_open( const StagehandOption* options, int count )
{
	if( g.threadLive )
		return -1;

	/* Mirrors the constraint most real sources have: one run per loaded copy.
	   Refusing loudly is what stops a host debugging a silent half-start. */
	if( g.everOpened )
	{
		snprintf( g.status, sizeof( g.status ),
				  "this copy has already run -- load a fresh copy of the library" );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}

	if( option( options, count, "refuse" ) )
	{
		snprintf( g.status, sizeof( g.status ), "refused on request (the 'refuse' option)" );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}

	atomic_store( &g.state, STAGEHAND_STARTING );
	atomic_store( &g.quit, false );
	atomic_store( &g.budget, 0 );
	atomic_store( &g.spent, 0 );
	atomic_store( &g.seq, 0 );
	atomic_store( &g.parks, 0 );
	atomic_store( &g.ready, -1 );
	atomic_store( &g.events, 0 );
	g.writing = 0;
	snprintf( g.status, sizeof( g.status ), "open" );

	pthread_mutex_init( &g.gate, NULL );
	pthread_cond_init( &g.gateCv, NULL );

	if( pthread_create( &g.thread, NULL, run, NULL ) != 0 )
	{
		snprintf( g.status, sizeof( g.status ), "could not start the thread" );
		atomic_store( &g.state, STAGEHAND_FAILED );
		return -1;
	}
	g.threadLive = true;
	g.everOpened = true;

	/* A source that fails asynchronously, on request, so a host can prove it
	   notices. Open succeeds; the thread gives up a moment later. */
	if( option( options, count, "failafter" ) )
	{
		int after = atoi( option( options, count, "failafter" ) );
		if( after <= 0 )
			after = 1;
		atomic_store( &g.budget, (unsigned)after );
		/* The thread will run those and then we mark failure from here on the
		   next State() -- simplest honest simulation. */
		snprintf( g.status, sizeof( g.status ),
				  "will fail after %d ticks (the 'failafter' option)", after );
	}
	return 0;
}

static void api_close( void )
{
	if( g.threadLive )
	{
		atomic_store( &g.quit, true );
		pthread_cond_broadcast( &g.gateCv );
		pthread_join( g.thread, NULL );
		g.threadLive = false;
		pthread_cond_destroy( &g.gateCv );
		pthread_mutex_destroy( &g.gate );
	}
	atomic_store( &g.state, STAGEHAND_CLOSED );
}

static int api_state( void )
{
	/* The asynchronous-failure simulation: once the promised ticks are spent,
	   report FAILED the way a real source would after rejecting its input. */
	if( strncmp( g.status, "will fail after", 15 ) == 0 && g.threadLive )
	{
		if( atomic_load( &g.spent ) >= atomic_load( &g.budget )
			&& atomic_load( &g.spent ) > 0 )
		{
			snprintf( g.status, sizeof( g.status ),
					  "gave up on its own, as the 'failafter' option asked" );
			atomic_store( &g.state, STAGEHAND_FAILED );
		}
	}
	return atomic_load( &g.state );
}

static const char* api_status( void )
{
	return g.status;
}

static void api_describe( StagehandInfo* out )
{
	if( !out )
		return;
	out->width           = TS_WIDTH;
	out->height          = TS_HEIGHT;
	out->rateNumerator   = TS_RATE;
	out->rateDenominator = 1;
	out->pixelAspect     = 1.0f;
	out->frameBytes      = TS_BYTES;
}

static void api_grant( int ticks )
{
	if( ticks <= 0 )
		return;
	atomic_fetch_add( &g.budget, (unsigned)ticks );
	pthread_cond_broadcast( &g.gateCv );
}

static int api_frame( void* dst, size_t dstBytes, uint32_t lastSeq, uint32_t* outSeq,
					  uint32_t* outTick )
{
	if( !dst || dstBytes < TS_BYTES )
		return 0;

	int ready = atomic_exchange( &g.ready, -1 );
	if( ready < 0 )
		return 0;

	const uint32_t seq = atomic_load( &g.seq );
	memcpy( dst, g.slots[ ready ], TS_BYTES );
	g.writing = ready;

	if( outSeq )
		*outSeq = seq;
	if( outTick )
		*outTick = atomic_load( &g.spent );

	return seq != lastSeq;
}

static void api_event( int code, int value )
{
	(void)code;
	atomic_fetch_add( &g.events, value ? 1 : -1 );
}

static void api_release_inputs( void )
{
	atomic_store( &g.events, 0 );
}

static uint32_t api_parks( void )
{
	return atomic_load( &g.parks );
}

static const StagehandSourceApi kApi = {
	STAGEHAND_ABI_VERSION,
	api_open,
	api_close,
	api_state,
	api_status,
	api_describe,
	api_grant,
	api_frame,
	api_event,
	api_release_inputs,
	api_parks,
};

const StagehandSourceApi* stagehand_source_api( void )
{
	return &kApi;
}
