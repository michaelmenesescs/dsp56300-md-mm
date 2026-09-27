#pragma once

#include <string>

namespace dsp56k
{
	// iOS only (no-op elsewhere): logs code-signing flags and the result of every executable-memory
	// strategy, then executes a tiny generated function if the process is CS_DEBUGGED.
	void runIosJitProbe();
	void logIosJitStatus(const std::string& _msg);
}
