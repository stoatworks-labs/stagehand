/*
	SourceAbi.h -- the C ABI a stagehand "source" exposes.

	A source is a shared library that produces frames on its own thread and is
	paced by whoever is drawing it. An emulator, a game engine, a software
	renderer, a decoder: anything whose natural clock is not the host's.

	Deliberately plain C and plain POD. The host and the source are separate
	binaries, usually built at different times and sometimes with different
	compilers, and nothing here may depend on a C++ runtime being shared
	between them.

	## Two rules the whole design rests on

	**One loaded copy is one instance.** Most code worth hosting this way is a
	pile of file-scope globals with no instance handle -- that is exactly why it
	is a separate library rather than a class. `dlopen` keys on path, so opening
	one that is already loaded hands back the SAME image with a bumped refcount,
	not a second set of globals. A host that wants two instances must load two
	copies from two paths, which is what `stagehand::Sidecar` does.

	**Time is granted, not read.** A source must never consult the wall clock.
	It runs on the ticks it is given and stops when it runs out. Speed, pause,
	single-stepping in a test and bit-exact determinism then all fall out of the
	one mechanism instead of being four features.
*/
#ifndef STAGEHAND_SOURCE_ABI_H
#define STAGEHAND_SOURCE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAGEHAND_ABI_VERSION 1u

/* The one exported symbol a source library must have. */
#define STAGEHAND_SOURCE_ENTRY "stagehand_source_api"

#if defined( _WIN32 )
	#define STAGEHAND_EXPORT __declspec( dllexport )
#else
	#define STAGEHAND_EXPORT __attribute__( ( visibility( "default" ) ) )
#endif

typedef enum StagehandState
{
	STAGEHAND_IDLE     = 0, /* nothing opened yet                            */
	STAGEHAND_STARTING = 1, /* opened, no frame produced yet                 */
	STAGEHAND_RUNNING  = 2, /* at least one frame published                  */
	STAGEHAND_FAILED   = 3, /* gave up; Status() says why                    */
	STAGEHAND_CLOSED   = 4
} StagehandState;

/*
	Options are a key/value list rather than a struct, so a source can grow a
	setting without either side's ABI changing. Unknown keys are the source's
	to ignore; it should say so in Status() rather than fail.
*/
typedef struct StagehandOption
{
	const char* key;
	const char* value;
} StagehandOption;

typedef struct StagehandInfo
{
	uint32_t width;
	uint32_t height;

	/* The source's natural rate as a fraction, so 35/1 and 60000/1001 are both
	   exact. The host converts elapsed time into ticks with it. */
	uint32_t rateNumerator;
	uint32_t rateDenominator;

	/*
		Pixel aspect ratio, in the standard sense: the WIDTH of one pixel
		divided by its HEIGHT. 1.0 is square. Zero means "assume square".

		Spelled out because the other reading is just as natural and gives the
		reciprocal, silently. A 320x200 picture that was always displayed at
		4:3 has pixels TALLER than they are wide, so its ratio is 5/6 ≈ 0.833,
		not the 1.2 that "1.2 times taller" suggests. Getting it upside down
		produces a picture that is wrong in a way that looks deliberate.

		Display aspect works out as (width / height) * pixelAspect.
	*/
	float pixelAspect;

	/* Bytes per published frame: width * height * 4, BGRA. Carried explicitly
	   so a host can size its buffer without assuming the packing. */
	size_t frameBytes;
} StagehandInfo;

typedef struct StagehandSourceApi
{
	uint32_t abiVersion;

	/* Starts producing. Returns 0 on success. Must not block waiting for the
	   first frame -- poll State() for that.

	   **Opening may succeed and then fail.** Most sources do their real work on
	   their own thread a moment later, so a bad input is a successful call
	   followed by State() going to STAGEHAND_FAILED. Hosts must watch for it;
	   nothing else will report it. */
	int ( *Open )( const StagehandOption* options, int count );

	/* Stops and releases everything. Safe in any state, including after a
	   failure, and safe to call twice.

	   Whether the same loaded copy may be reopened is the source's business to
	   state. Many cannot -- see the note at the top -- and a source that cannot
	   should refuse a second Open with a Status() that says to load a fresh
	   copy, rather than half-working. */
	void ( *Close )( void );

	int ( *State )( void ); /* StagehandState */

	/* Human-readable, never NULL. After a failure this is where the source's
	   own account of it goes. */
	const char* ( *Status )( void );

	void ( *Describe )( StagehandInfo* out );

	/* Release `ticks` more ticks of the source's clock. This is the only thing
	   that makes time pass for it. */
	void ( *Grant )( int ticks );

	/*
		Copies the newest frame into `dst`, which must be at least
		`StagehandInfo::frameBytes`. Returns 1 if a frame newer than `lastSeq`
		was written, 0 otherwise. Must never block: the caller is usually a
		render thread.

		Pixels are BGRA, bottom-up -- the order and orientation a GL texture
		upload wants, so the common path costs no conversion. Alpha must be
		opaque unless the source genuinely has one.
	*/
	int ( *Frame )( void* dst, size_t dstBytes, uint32_t lastSeq, uint32_t* outSeq,
					uint32_t* outTick );

	/* An input event, in whatever vocabulary the source documents. `code` is
	   source-defined; `value` is typically 1 for down and 0 for up. */
	void ( *Event )( int code, int value );

	/* Release every held input. Hosts call this when a clip is deactivated, so
	   the source does not sit there with a key held down for ever. */
	void ( *ReleaseInputs )( void );

	/*
		How many times the source has run out of granted time and parked,
		counted once per episode rather than once per wakeup.

		It exists for tests. "Grant a tick then take the next frame" samples
		whenever the source happens to have published, which is a race -- many
		sources draw several frames while spending one tick -- so two identical
		runs disagree. Waiting for this to move means the source has spent
		everything it was given and stopped.
	*/
	uint32_t ( *Parks )( void );
} StagehandSourceApi;

STAGEHAND_EXPORT const StagehandSourceApi* stagehand_source_api( void );

typedef const StagehandSourceApi* ( *stagehand_source_api_fn )( void );

#ifdef __cplusplus
}
#endif

#endif /* STAGEHAND_SOURCE_ABI_H */
