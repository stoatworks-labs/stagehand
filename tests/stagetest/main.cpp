/*
	stagetest -- the library, against the synthetic source, with no graphics
	API anywhere.

	  stagetest [--source PATH]

	Everything here runs against tests/testsource, which is built from this
	repo, so the whole suite needs nothing downloaded and nothing installed.
	That is the point: stagehand has no dependency on any particular source,
	and a test suite that needed one would quietly disprove it.
*/
#include "stagehand/Diag.h"
#include "stagehand/Present.h"
#include "stagehand/Sidecar.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef STAGEHAND_TESTSOURCE_PATH
	#define STAGEHAND_TESTSOURCE_PATH "./libstagehand_testsource.dylib"
#endif

namespace
{

int g_checks   = 0;
int g_failures = 0;

void ok( bool condition, const char* what )
{
	g_checks += 1;
	std::printf( condition ? "  ok    %s\n" : "  FAIL  %s\n", what );
	if( !condition )
		g_failures += 1;
}

using stagehand::Sidecar;

/*
	Advances the source by roughly `ticks` and waits for it to park again.

	Waiting on the park counter rather than on "a frame appeared" is the whole
	trick, and it is what makes the checks below reproducible: a source may
	draw several frames while spending one tick, so sampling on the first new
	frame catches a different moment depending on how the threads interleave.
	A park means it has spent everything it was given and stopped.

	Pump is driven at a high speed over real elapsed time rather than calling
	Grant directly, because Pump is the thing under test.
*/
bool step( Sidecar& s, int ticks = 1 )
{
	s.Pump( 0.0f ); // take a fresh elapsed baseline, granting nothing

	for( int i = 0; i < ticks; ++i )
	{
		/*
			Sample the counter, grant, then wait -- per tick, not once around
			the whole loop. Sampling once at the top passes on the FIRST park
			of the sequence, which happens while later grants are still to
			come, so the helper returns with budget unspent and the next check
			sees the source advance when it was supposed to be idle.
		*/
		const uint32_t parked = s.Parks();

		std::this_thread::sleep_for( std::chrono::milliseconds( 3 ) );
		s.Pump( 10.0f ); // 3 ms at 50 Hz x 10 is comfortably over one tick

		bool spent = false;
		for( int spin = 0; spin < 2000 && !spent; ++spin )
		{
			spent = s.Parks() != parked;
			if( !spent )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		if( !spent )
			return false;
	}
	return true;
}

struct Pixel
{
	uint8_t b, g, r, a;
};

Pixel at( const std::vector< uint8_t >& buf, const StagehandInfo& info, uint32_t x,
		  uint32_t y )
{
	// Buffers are bottom-up; (x, y) here is in screen coordinates.
	const size_t i = ( size_t( info.height - 1 - y ) * info.width + x ) * 4;
	return { buf[ i ], buf[ i + 1 ], buf[ i + 2 ], buf[ i + 3 ] };
}

bool isRed( Pixel p )   { return p.r > 200 && p.g < 60 && p.b < 60; }
bool isGreen( Pixel p ) { return p.g > 200 && p.r < 60 && p.b < 60; }
bool isBlue( Pixel p )  { return p.b > 200 && p.r < 60 && p.g < 60; }
bool isWhite( Pixel p ) { return p.r > 200 && p.g > 200 && p.b > 200; }

int SelfTest( const std::string& sourcePath )
{
	std::printf( "stagetest: source %s\n\n", sourcePath.c_str() );

	/* --- refusing things that are not sources --------------------- */
	{
		Sidecar s;
		ok( !s.Open( "/nonexistent/nope.dylib", {} ),
			"a library that is not there is refused" );
		ok( !s.Status().empty(), "...with a message saying so" );
	}

	/* --- a real open ---------------------------------------------- */
	std::vector< uint8_t > frame;
	{
		Sidecar s;
		ok( s.Open( sourcePath, {} ), "the synthetic source opens" );

		const StagehandInfo& info = s.Info();
		ok( info.width == 64 && info.height == 32, "Describe reports its size" );
		ok( info.frameBytes == 64 * 32 * 4, "...and its frame size" );
		ok( info.rateNumerator == 50 && info.rateDenominator == 1,
			"...and its rate as an exact fraction" );

		frame.resize( info.frameBytes );

		ok( step( s, 4 ), "granted time is spent and the source parks" );
		ok( s.Frame( frame.data(), frame.size() ), "a frame arrives" );
		ok( s.Running(), "the source reports RUNNING" );

		/*
			The four corners. All four differ and none is symmetric, so this
			single check catches both a BGRA/RGBA swap and a vertical flip --
			the two mistakes that otherwise produce a picture that looks
			plausible until somebody notices the colours are wrong.
		*/
		ok( isRed( at( frame, info, 0, 0 ) ), "top-left is red (no flip, no swap)" );
		ok( isGreen( at( frame, info, info.width - 1, 0 ) ), "top-right is green" );
		ok( isBlue( at( frame, info, 0, info.height - 1 ) ), "bottom-left is blue" );
		ok( isWhite( at( frame, info, info.width - 1, info.height - 1 ) ),
			"bottom-right is white" );

		bool opaque = true;
		for( size_t i = 3; i < frame.size(); i += 4 )
			if( frame[ i ] != 0xFF )
				opaque = false;
		ok( opaque, "every pixel is opaque" );

		/* --- time really is granted, not read --------------------- */
		uint32_t tick = 0;
		s.Frame( frame.data(), frame.size(), &tick );
		const uint32_t before = tick;

		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
		s.Frame( frame.data(), frame.size(), &tick );
		ok( tick == before,
			"the source does not advance while it is granted nothing" );

		ok( step( s, 3 ), "more time can be granted afterwards" );
		s.Frame( frame.data(), frame.size(), &tick );
		ok( tick > before, "...and the source advances again" );
	}

	/* --- a source that refuses to open ----------------------------- */
	{
		Sidecar s;
		std::vector< StagehandOption > opts = { { "refuse", "1" } };
		ok( !s.Open( sourcePath, opts ), "a source that refuses is reported as failed" );
		ok( s.Status().find( "refuse" ) != std::string::npos,
			"...and the source's own reason is kept" );
	}

	/* --- a source that fails AFTER opening ------------------------- */
	{
		/*
			The failure mode a host will actually hit. Open succeeds, because
			all it did was start a thread; the source rejects its input a
			moment later. Nothing notices unless the host asks every frame.
		*/
		Sidecar s;
		std::vector< StagehandOption > opts = { { "failafter", "2" } };
		ok( s.Open( sourcePath, opts ), "a source that will fail later still opens" );

		bool sawFailure = false;
		for( int i = 0; i < 200 && !sawFailure; ++i )
		{
			s.Pump( 1000.0f );
			std::this_thread::sleep_for( std::chrono::milliseconds( 3 ) );
			sawFailure = s.Failed();
		}
		ok( sawFailure, "...and the host can see it give up" );
		ok( !s.SourceStatus().empty(), "...with the source's own account of why" );
	}

	/* --- two Sidecars are two instances ---------------------------- */
	{
		/*
			The claim the whole private-copy arrangement exists to make. Two
			Sidecars on one loaded image would share one set of globals, so
			the second Open would be refused outright -- the synthetic source
			enforces one run per copy precisely so this is testable.
		*/
		Sidecar a, b;
		ok( a.Open( sourcePath, {} ), "a first instance opens" );
		ok( b.Open( sourcePath, {} ), "a second instance opens alongside it" );

		ok( a.LibraryPath() == b.LibraryPath(),
			"both were asked for the same library on disk" );

		step( a, 12 );

		uint32_t ta = 0, tb = 0;
		std::vector< uint8_t > fa( a.Info().frameBytes ), fb( b.Info().frameBytes );
		a.Frame( fa.data(), fa.size(), &ta );
		b.Frame( fb.data(), fb.size(), &tb );

		ok( ta > tb, "driving one does not advance the other" );
	}

	/* --- the fitting maths ----------------------------------------- */
	{
		using stagehand::ComputeFit;
		using stagehand::Fit;

		float sx = 0, sy = 0;

		/*
			320x200 with pixels 5/6 as wide as they are tall is a 4:3 picture.
			Note the ratio is the RECIPROCAL of "1.2 times taller" -- writing
			1.2 here gives a 1.92 display aspect, a picture wider than 16:9,
			and a letterbox where there should be a pillarbox.
		*/
		const float doomPar = 5.0f / 6.0f;

		// 4:3 in 16:9: narrower than the frame, so pillarboxed, full height.
		ComputeFit( Fit::Contain, 1920, 1080, 320, 200, doomPar, sx, sy );
		ok( sy > 0.999f && sx < 0.999f, "Contain pillarboxes a 4:3 picture in 16:9" );

		// The same picture in a square frame: letterboxed, full width. Testing
		// both is the point -- a sign error in one branch is invisible in the
		// other, which is exactly how this class of bug survives review.
		ComputeFit( Fit::Contain, 720, 720, 320, 200, doomPar, sx, sy );
		ok( sx > 0.999f && sy < 0.999f, "...and letterboxes it in a square frame" );

		ComputeFit( Fit::Cover, 1920, 1080, 320, 200, doomPar, sx, sy );
		ok( sx > 0.999f && sy > 0.999f, "Cover leaves nothing uncovered" );

		ComputeFit( Fit::Stretch, 1920, 1080, 320, 200, doomPar, sx, sy );
		ok( sx > 0.999f && sy > 0.999f, "Stretch fills both axes" );

		// 64x32 into 1000x700: 15x by width, 21x by height, so 15x. A non-1.0
		// pixel aspect is passed deliberately -- Integer must ignore it, or the
		// whole multiple it exists to guarantee is not whole any more.
		ComputeFit( Fit::Integer, 1000, 700, 64, 32, doomPar, sx, sy );
		const float expectedX = float( 64 * 15 ) / 1000.0f;
		const float expectedY = float( 32 * 15 ) / 700.0f;
		ok( sx > expectedX - 0.001f && sx < expectedX + 0.001f
				&& sy > expectedY - 0.001f && sy < expectedY + 0.001f,
			"Integer picks whole pixel multiples and ignores pixel aspect" );

		/*
			A picture bigger than the frame cannot be shown at 1x, so Integer
			has to fall back to Contain. The trap is that Contain assigns only
			ONE axis -- so without resetting both first, the other keeps the
			oversized multiple and the picture overflows on one side.
		*/
		ComputeFit( Fit::Integer, 100, 100, 320, 200, 1.0f, sx, sy );
		ok( sx <= 1.001f && sy <= 1.001f,
			"Integer falls back cleanly when the picture will not fit at 1x" );
	}

	std::printf( "\nstagetest: %d checks, %d failures\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}

/*
	Dumps one frame as a PPM. Not part of the self-test: it is for whoever is
	writing a source and wants to see what it is actually publishing, which is
	the fastest way to find a flip or a channel swap in their own code.
*/
int DumpFrame( const std::string& sourcePath, const std::string& out, int ticks )
{
	Sidecar s;
	if( !s.Open( sourcePath, {} ) )
	{
		std::fprintf( stderr, "stagetest: %s\n", s.Status().c_str() );
		return 1;
	}

	std::vector< uint8_t > frame( s.Info().frameBytes );
	if( !step( s, ticks ) )
	{
		std::fprintf( stderr, "stagetest: the source never parked\n" );
		return 1;
	}
	if( !s.Frame( frame.data(), frame.size() ) )
	{
		std::fprintf( stderr, "stagetest: no frame\n" );
		return 1;
	}

	FILE* fp = std::fopen( out.c_str(), "wb" );
	if( !fp )
	{
		std::fprintf( stderr, "stagetest: cannot write %s\n", out.c_str() );
		return 1;
	}

	const StagehandInfo& info = s.Info();
	std::fprintf( fp, "P6\n%u %u\n255\n", info.width, info.height );

	// Published frames are bottom-up BGRA, shaped for a texture upload. PPM is
	// top-down RGB, so un-flip here and the file looks like the screen.
	for( int y = int( info.height ) - 1; y >= 0; --y )
		for( uint32_t x = 0; x < info.width; ++x )
		{
			const uint8_t* px = frame.data() + ( size_t( y ) * info.width + x ) * 4;
			std::fputc( px[ 2 ], fp );
			std::fputc( px[ 1 ], fp );
			std::fputc( px[ 0 ], fp );
		}
	std::fclose( fp );

	std::printf( "stagetest: wrote %s (%ux%u)\n", out.c_str(), info.width, info.height );
	return 0;
}

} // namespace

int main( int argc, char** argv )
{
	std::string source = STAGEHAND_TESTSOURCE_PATH;
	std::string out;
	int         ticks = 8;

	for( int i = 1; i < argc; ++i )
	{
		if( !std::strcmp( argv[ i ], "--source" ) && i + 1 < argc )
			source = argv[ ++i ];
		else if( !std::strcmp( argv[ i ], "--out" ) && i + 1 < argc )
			out = argv[ ++i ];
		else if( !std::strcmp( argv[ i ], "--ticks" ) && i + 1 < argc )
			ticks = std::atoi( argv[ ++i ] );
		else
		{
			std::fprintf( stderr,
						  "usage: stagetest [--source PATH]\n"
						  "                 [--out FILE.ppm [--ticks N]]\n" );
			return 2;
		}
	}

	stagehand::diag::Init( "stagetest" );

	if( !out.empty() )
		return DumpFrame( source, out, ticks );

	return SelfTest( source );
}
