#pragma once

#include <string>

/**
	A log file, and deliberately nothing else.

	No crash handler and no console. A host plugin runs inside somebody else's
	application, where installing a signal handler fights that application for
	the same signals and where there is no stdout for anyone to read.

	It exists because every interesting failure in this kind of plugin looks
	identical from outside -- the layer is black. A source library that was
	missing, a source that rejected its input, an ABI mismatch, a shader that
	did not compile: one symptom, several causes.

		macOS    ~/Library/Logs/<app>/<app>.YYYY-MM-DD.log
		Windows  %LOCALAPPDATA%\<app>\Logs\...
		Linux    ~/.local/state/<app>/...

	`<APP>_LOG_DIR` overrides the directory, where `<APP>` is the name given to
	`Init` upper-cased.
*/
namespace stagehand
{
namespace diag
{

/// Names the log. Call once, early; later calls are ignored. Without it the
/// log is written under "stagehand".
void Init( const std::string& appName );

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Where the log is being written. Empty if the directory was unusable, in
/// which case logging is a no-op rather than an error the host must handle.
std::string LogPath();

/*
	Redirect the C-level stderr into the log for the rest of the process.

	Worth having because a hosted source is usually C that reports its failures
	with `fprintf(stderr, ...)` and then exits, from code the host does not
	control. Inside a GUI application that output goes nowhere at all, and it
	is routinely the only message that names the real problem.

	Note what it costs: this takes the WHOLE process's stderr, the host
	application's included. That is a real cost and usually the right trade,
	but it is the host's decision, which is why it is a separate call rather
	than part of Init.
*/
void CaptureStderr();

} // namespace diag
} // namespace stagehand
