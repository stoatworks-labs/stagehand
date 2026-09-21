#include "stagehand/Sidecar.h"
#include "stagehand/Diag.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>

#if defined( _WIN32 )
	#include <windows.h>
#else
	#include <dlfcn.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

namespace stagehand
{
namespace
{

std::atomic< unsigned > gCopyCounter { 0 };

uint64_t nowNanoseconds()
{
	return uint64_t( std::chrono::duration_cast< std::chrono::nanoseconds >(
						 std::chrono::steady_clock::now().time_since_epoch() )
						 .count() );
}

bool copyFile( const std::string& from, const std::string& to )
{
	std::error_code ec;
	std::filesystem::copy_file( from, to,
								std::filesystem::copy_options::overwrite_existing, ec );
	if( ec )
		return false;
#if !defined( _WIN32 )
	::chmod( to.c_str(), 0755 );
#endif
	return true;
}

std::string temporaryDirectory()
{
	std::error_code ec;
	auto            dir = std::filesystem::temp_directory_path( ec );
	return ec ? std::string( "/tmp" ) : dir.string();
}

} // namespace

std::string BinaryDirectory()
{
#if defined( _WIN32 )
	HMODULE module = nullptr;
	if( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
								 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 reinterpret_cast< LPCSTR >( &nowNanoseconds ), &module ) )
		return {};

	char path[ MAX_PATH ] = { 0 };
	if( GetModuleFileNameA( module, path, MAX_PATH ) == 0 )
		return {};
	return std::filesystem::path( path ).parent_path().string();
#else
	Dl_info info {};
	if( dladdr( reinterpret_cast< const void* >( &nowNanoseconds ), &info ) == 0
		|| info.dli_fname == nullptr )
		return {};
	return std::filesystem::path( info.dli_fname ).parent_path().string();
#endif
}

Sidecar::~Sidecar()
{
	Close();
}

bool Sidecar::Open( const std::string& libraryPath,
					const std::vector< StagehandOption >& options )
{
	Close();

	std::error_code ec;
	if( libraryPath.empty() || !std::filesystem::exists( libraryPath, ec ) )
	{
		mStatus = "no source library at '" + libraryPath + "'";
		diag::error( mStatus );
		return false;
	}
	mSourcePath = libraryPath;

	/*
		One copy per instance, under a name nothing else can collide with.
		Without this, dlopen hands back an image another Sidecar is already
		driving -- see the class comment.
	*/
	const std::string leaf =
		std::filesystem::path( libraryPath ).filename().string();
	char unique[ 160 ];
	std::snprintf( unique, sizeof( unique ), "/stagehand-%d-%u-%s", int( ::getpid() ),
				   gCopyCounter.fetch_add( 1 ), leaf.c_str() );
	mCopyPath = temporaryDirectory() + unique;

	if( !copyFile( libraryPath, mCopyPath ) )
	{
		mStatus = "could not stage a private copy at '" + mCopyPath + "'";
		mCopyPath.clear();
		diag::error( mStatus );
		return false;
	}

#if defined( _WIN32 )
	mHandle = (void*)LoadLibraryA( mCopyPath.c_str() );
#else
	mHandle = dlopen( mCopyPath.c_str(), RTLD_NOW | RTLD_LOCAL );
#endif
	if( !mHandle )
	{
#if defined( _WIN32 )
		mStatus = "could not load the source library";
#else
		const char* err = dlerror();
		mStatus = std::string( "could not load the source library: " )
				+ ( err ? err : "unknown" );
#endif
		diag::error( mStatus );
		Close();
		return false;
	}

#if defined( _WIN32 )
	auto entry = (stagehand_source_api_fn)GetProcAddress( (HMODULE)mHandle,
														  STAGEHAND_SOURCE_ENTRY );
#else
	auto entry = (stagehand_source_api_fn)dlsym( mHandle, STAGEHAND_SOURCE_ENTRY );
#endif
	if( !entry )
	{
		mStatus = "'" + leaf + "' has no " + STAGEHAND_SOURCE_ENTRY
				+ " -- it is not a stagehand source";
		diag::error( mStatus );
		Close();
		return false;
	}

	mApi = entry();
	if( !mApi || mApi->abiVersion != STAGEHAND_ABI_VERSION )
	{
		// A stale source beside a new host reads a struct of the wrong shape
		// and calls through whatever is at that offset. Refusing is the only
		// safe answer, and the two numbers name the fix.
		char msg[ 200 ];
		std::snprintf( msg, sizeof( msg ),
					   "source ABI %u does not match the host's %u -- mismatched parts",
					   mApi ? mApi->abiVersion : 0u, STAGEHAND_ABI_VERSION );
		mStatus = msg;
		mApi    = nullptr;
		diag::error( mStatus );
		Close();
		return false;
	}

	mApi->Describe( &mInfo );
	if( mInfo.frameBytes == 0 || mInfo.width == 0 || mInfo.height == 0 )
	{
		mStatus = "the source describes an empty frame";
		diag::error( mStatus );
		Close();
		return false;
	}
	if( mInfo.rateDenominator == 0 )
		mInfo.rateDenominator = 1;

	if( mApi->Open( options.empty() ? nullptr : options.data(), int( options.size() ) ) != 0 )
	{
		mStatus = mApi->Status() ? mApi->Status() : "the source refused to open";
		diag::error( "source refused to open: " + mStatus );
		Close();
		return false;
	}

	mLastSeq       = 0;
	mLastPumpNs    = nowNanoseconds();
	mTickRemainder = 0.0;
	mStatus        = "open";
	diag::info( "source open: " + leaf
				+ " -- most sources load their input on their own thread, so watch "
				  "for a failure after this" );
	return true;
}

void Sidecar::Close()
{
	if( mApi )
	{
		mApi->Close();
		mApi = nullptr;
	}
	if( mHandle )
	{
#if defined( _WIN32 )
		FreeLibrary( (HMODULE)mHandle );
#else
		dlclose( mHandle );
#endif
		mHandle = nullptr;
	}
	if( !mCopyPath.empty() )
	{
		std::error_code ec;
		std::filesystem::remove( mCopyPath, ec );
		mCopyPath.clear();
	}
	mLastSeq = 0;
	mInfo    = {};
}

bool Sidecar::Running() const
{
	return mApi != nullptr && mApi->State() == STAGEHAND_RUNNING;
}

bool Sidecar::Failed() const
{
	return mApi != nullptr && mApi->State() == STAGEHAND_FAILED;
}

std::string Sidecar::SourceStatus() const
{
	if( !mApi )
		return {};
	const char* s = mApi->Status();
	return s ? s : "";
}

uint32_t Sidecar::Parks() const
{
	return mApi ? mApi->Parks() : 0u;
}

void Sidecar::Pump( float speed )
{
	if( !mApi )
		return;

	const uint64_t now     = nowNanoseconds();
	uint64_t       elapsed = now - mLastPumpNs;
	mLastPumpNs            = now;

	/*
		A long gap is not time the source owes anybody. A host that was paused,
		a layer that was not being drawn, or a machine that slept would
		otherwise hand over minutes of budget at once and the operator would
		watch the source fast-forward through whatever they were waiting for.
		Clamping to a quarter second keeps a dropped frame smooth and throws an
		outage away.
	*/
	constexpr uint64_t kMaxGapNs = 250ull * 1000ull * 1000ull;
	if( elapsed > kMaxGapNs )
		elapsed = kMaxGapNs;

	if( speed <= 0.0f )
		return; // paused: no time passes, so the source parks instead of spinning

	const double rate = double( mInfo.rateNumerator ) / double( mInfo.rateDenominator );
	mTickRemainder += ( double( elapsed ) / 1e9 ) * rate * double( speed );

	const int whole = int( mTickRemainder );
	if( whole > 0 )
	{
		mTickRemainder -= double( whole );
		mApi->Grant( whole );
	}
}

bool Sidecar::Frame( void* dst, size_t dstBytes, uint32_t* outTick )
{
	if( !mApi || !dst )
		return false;

	uint32_t seq  = 0;
	uint32_t tick = 0;
	if( !mApi->Frame( dst, dstBytes, mLastSeq, &seq, &tick ) )
		return false;

	mLastSeq = seq;
	if( outTick )
		*outTick = tick;
	return true;
}

void Sidecar::Event( int code, int value )
{
	if( mApi )
		mApi->Event( code, value );
}

void Sidecar::ReleaseInputs()
{
	if( mApi )
		mApi->ReleaseInputs();
}

} // namespace stagehand
