#pragma once

#include "SourceAbi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace stagehand
{

/**
	One loaded source: finds the library, loads a private copy of it, and
	spends its clock against the host's.

	**Every Sidecar loads its own copy of the library, from its own path on
	disk, and that is the whole reason this class exists.** `dlopen` keys on
	path, so opening a library that is already loaded returns the same image
	with a bumped refcount rather than a second set of globals -- and the kind
	of code worth hosting this way is usually nothing but globals. Two hosts
	sharing an image would share one instance, one buffer and one game.

	A copy also solves reopening. Many sources can only run once per loaded
	image, because releasing memory does not put a library's globals back; the
	answer is to throw the image away and take a fresh one, which `Open` does
	on every call.
*/
class Sidecar
{
public:
	Sidecar() = default;
	~Sidecar();

	Sidecar( const Sidecar& )            = delete;
	Sidecar& operator=( const Sidecar& ) = delete;

	/// Loads a fresh private copy of `libraryPath` and opens it with `options`.
	/// Any previous instance is torn down first. False sets Status().
	bool Open( const std::string& libraryPath,
			   const std::vector< StagehandOption >& options );

	void Close();

	bool Loaded() const { return mApi != nullptr; }
	bool Running() const;

	/*
		True once the source has given up on its own.

		`Open` only reports that the library loaded and the call returned.
		Sources do their real work on their own thread a moment later, so a bad
		input is a success followed by a failure. Nothing notices unless the
		host asks, every frame.
	*/
	bool Failed() const;

	/// The host side's own account. Never empty once anything was attempted.
	const std::string& Status() const { return mStatus; }

	/// The source's own account, which after a failure is the useful one.
	std::string SourceStatus() const;

	const StagehandInfo& Info() const { return mInfo; }

	/*
		Releases as much of the source's clock as has passed on the wall,
		scaled by `speed`. Call once per rendered frame. A speed of zero grants
		nothing, which parks the source rather than spinning it.

		**Driving from elapsed time rather than counting calls is not a detail.**
		A host may render the same logical frame more than once -- to an output,
		a preview and a thumbnail -- and anything advancing once per call then
		runs at two or three times speed, but only while the preview is open.
		Two calls a microsecond apart release a microsecond of source time
		between them, which is the correct answer to both.
	*/
	void Pump( float speed );

	/// Copies the newest frame into the caller's buffer. False if nothing newer.
	bool Frame( void* dst, size_t dstBytes, uint32_t* outTick = nullptr );

	void Event( int code, int value );
	void ReleaseInputs();

	/// Test hook: see StagehandSourceApi::Parks.
	uint32_t Parks() const;

	/// The library that is loaded, or empty. For diagnostics.
	const std::string& LibraryPath() const { return mSourcePath; }

private:
	void* mHandle = nullptr;
	const StagehandSourceApi* mApi = nullptr;

	std::string   mSourcePath;
	std::string   mCopyPath;
	std::string   mStatus = "nothing loaded";
	StagehandInfo mInfo {};

	uint32_t mLastSeq = 0;

	/// Wall-clock nanoseconds at the last Pump, and the leftover fraction of a
	/// tick. Keeping the remainder is what stops a 60 Hz host asking for a
	/// 35 Hz source (0.583 ticks a frame) from truncating to zero for ever.
	uint64_t mLastPumpNs   = 0;
	double   mTickRemainder = 0.0;
};

/*
	Where a host's own binary lives, which is where it should look for the
	sources it ships with.

	Resolved with `dladdr` on a symbol in this library rather than anything
	involving a bundle name or a search of known folders: the bundle may have
	been renamed, there may be several versions installed, and on Windows the
	whole concept differs -- but the loader always knows where it got this code.
*/
std::string BinaryDirectory();

} // namespace stagehand
