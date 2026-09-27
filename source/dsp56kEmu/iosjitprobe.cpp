#include "iosjitprobe.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE && defined(__aarch64__)

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dsp56kBase/logging.h"

extern "C" int csops(pid_t _pid, unsigned int _ops, void* _useraddr, size_t _usersize);

namespace dsp56k
{
	namespace
	{
		constexpr uint32_t g_csValid = 0x00000001;
		constexpr uint32_t g_csGetTaskAllow = 0x00000004;
		constexpr uint32_t g_csHard = 0x00000100;
		constexpr uint32_t g_csKill = 0x00000200;
		constexpr uint32_t g_csEnforcement = 0x00001000;
		constexpr uint32_t g_csDebugged = 0x10000000;

		// mov w0, #42 ; ret
		constexpr uint32_t g_code[] = {0x52800540, 0xd65f03c0};

		FILE* g_probeFile = nullptr;

		void probeLog(const std::string& _msg)
		{
			LOG("JITPROBE " << _msg);
			fprintf(stderr, "JITPROBE %s\n", _msg.c_str());
			fflush(stderr);
			if (g_probeFile)
			{
				fprintf(g_probeFile, "%s\n", _msg.c_str());
				fflush(g_probeFile);
				fsync(fileno(g_probeFile));
			}
		}

		std::string errnoStr(const int _e)
		{
			return std::to_string(_e) + " (" + strerror(_e) + ")";
		}

		uint32_t csFlags()
		{
			uint32_t flags = 0;
			if (csops(getpid(), 0, &flags, sizeof(flags)) != 0)
				return 0xffffffff;
			return flags;
		}

		void logCsFlags(const char* _when)
		{
			const auto f = csFlags();
			char buf[256];
			snprintf(buf, sizeof(buf), "csops[%s] flags=0x%08x VALID=%d GET_TASK_ALLOW=%d HARD=%d KILL=%d ENFORCEMENT=%d DEBUGGED=%d",
				_when, f, (f & g_csValid) != 0, (f & g_csGetTaskAllow) != 0, (f & g_csHard) != 0, (f & g_csKill) != 0,
				(f & g_csEnforcement) != 0, (f & g_csDebugged) != 0);
			probeLog(buf);
		}

		bool execAllowed()
		{
			if (csFlags() & g_csDebugged)
				return true;
			const char* e = getenv("GEARMULATOR_JIT_PROBE_EXEC");
			return e && *e == '1';
		}

		void tryExec(const char* _name, void* _rx)
		{
			if (!execAllowed())
			{
				probeLog(std::string(_name) + ": exec skipped (not CS_DEBUGGED, GEARMULATOR_JIT_PROBE_EXEC not set)");
				return;
			}
			probeLog(std::string(_name) + ": calling generated code now (a SIGKILL right after this line = code-signing kill)");
			using Func = int(*)();
			const int r = reinterpret_cast<Func>(_rx)();
			probeLog(std::string(_name) + ": generated code returned " + std::to_string(r) + (r == 42 ? " OK" : " WRONG"));
		}

		void probeRwx(const char* _name, const int _extraFlags, const bool _exec)
		{
			const size_t size = getpagesize();
			errno = 0;
			void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | _extraFlags, -1, 0);
			if (p == MAP_FAILED)
			{
				probeLog(std::string(_name) + ": mmap RWX FAILED errno=" + errnoStr(errno));
				return;
			}
			probeLog(std::string(_name) + ": mmap RWX ok");
			memcpy(p, g_code, sizeof(g_code));
			if (_exec)
			{
				sys_icache_invalidate(p, sizeof(g_code));
				tryExec(_name, p);
			}
			else
			{
				probeLog(std::string(_name) + ": not executed (executing an RWX page faults with KERN_PROTECTION_FAILURE on iOS, see crash logs)");
			}
			munmap(p, size);
		}

		void probeMprotect()
		{
			const char* name = "rw->rx mprotect";
			const size_t size = getpagesize();
			void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (p == MAP_FAILED)
			{
				probeLog(std::string(name) + ": mmap RW FAILED errno=" + errnoStr(errno));
				return;
			}
			memcpy(p, g_code, sizeof(g_code));
			errno = 0;
			const int r = mprotect(p, size, PROT_READ | PROT_EXEC);
			probeLog(std::string(name) + ": mprotect(RX) returned " + std::to_string(r) + (r ? " errno=" + errnoStr(errno) : ""));
			if (r == 0)
			{
				sys_icache_invalidate(p, sizeof(g_code));
				tryExec(name, p);
			}
			errno = 0;
			const int r2 = mprotect(p, size, PROT_READ | PROT_WRITE | PROT_EXEC);
			probeLog(std::string(name) + ": mprotect(RWX) returned " + std::to_string(r2) + (r2 ? " errno=" + errnoStr(errno) : ""));
			munmap(p, size);
		}

		void probeVmRemap()
		{
			const char* name = "vm_remap dual map";
			const size_t size = getpagesize();
			vm_address_t rwAddr = 0;
			if (vm_allocate(mach_task_self(), &rwAddr, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
			{
				probeLog(std::string(name) + ": vm_allocate FAILED");
				return;
			}
			void* rw = reinterpret_cast<void*>(rwAddr);
			memcpy(rw, g_code, sizeof(g_code));
			vm_address_t rx = 0;
			vm_prot_t cur = 0, max = 0;
			const kern_return_t kr = vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
				reinterpret_cast<vm_address_t>(rw), FALSE, &cur, &max, VM_INHERIT_NONE);
			if (kr != KERN_SUCCESS)
			{
				probeLog(std::string(name) + ": vm_remap FAILED kr=" + std::to_string(kr));
				munmap(rw, size);
				return;
			}
			errno = 0;
			const int r = mprotect(reinterpret_cast<void*>(rx), size, PROT_READ | PROT_EXEC);
			probeLog(std::string(name) + ": vm_remap ok, max_prot=" + std::to_string(max) + ", mprotect(mirror RX) returned " +
				std::to_string(r) + (r ? " errno=" + errnoStr(errno) : ""));
			if (r == 0)
			{
				sys_icache_invalidate(reinterpret_cast<void*>(rx), sizeof(g_code));
				tryExec(name, reinterpret_cast<void*>(rx));
				if (execAllowed())
				{
					// coherence: patch the immediate through the RW view (mov w0, #7) and run the RX view again
					const uint32_t patched = 0x528000e0;
					memcpy(rw, &patched, sizeof(patched));
					sys_icache_invalidate(reinterpret_cast<void*>(rx), sizeof(g_code));
					using Func = int(*)();
					const int r2 = reinterpret_cast<Func>(rx)();
					probeLog(std::string(name) + ": after writing through RW view the RX view returned " + std::to_string(r2) + (r2 == 7 ? " (coherent) OK" : " NOT COHERENT"));
				}
			}
			munmap(reinterpret_cast<void*>(rx), size);
			munmap(rw, size);
		}
	}

	void runIosJitProbe()
	{
		static std::once_flag once;
		std::call_once(once, []
		{
			if (const char* home = getenv("HOME"))
			{
				const std::string path = std::string(home) + "/Documents/jitprobe.txt";
				g_probeFile = fopen(path.c_str(), "a");
			}
			probeLog("---- iOS JIT probe, pid " + std::to_string(getpid()));
			logCsFlags("start");
			probeMprotect();
			probeVmRemap();
			probeRwx("mmap RWX", 0, false);
			probeRwx("mmap RWX|MAP_JIT", MAP_JIT, false);
			logCsFlags("end");
		});
	}

	void logIosJitStatus(const std::string& _msg)
	{
		probeLog(_msg);
	}
}

#else

namespace dsp56k
{
	void runIosJitProbe() {}
	void logIosJitStatus(const std::string&) {}
}

#endif
