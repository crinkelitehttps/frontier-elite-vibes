/*
 * crash_log.c - on a crash (segfault, bus error, abort...), writes where it
 * happened to crash.log (appended, next to glfrontier.cfg) and to the log:
 *
 *  - the faulting address, and if it lies in or past the game's RAM, the
 *    game address that was read or written (a bad pointer in game data);
 *  - the host stack as module+offset. The frames in the game code
 *    (Start680x0_at) map to lines of fe2/fe2_modded.s with
 *    tools/crash_lines.py, run against the same executable;
 *  - the game's registers, the routine the second VM was running
 *    (fe2vm_call: camera screen, atlas...) and the last host calls.
 *
 * Then the default action runs, so the core dump still happens.
 * Full detail on Linux/glibc; elsewhere the plain one-line message.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "main.h"
#include "host.h"
#include "hostcall.h"

extern int fe2vm_depth;
extern uint32_t fe2vm_cur_addr;

#if defined(__linux__) && defined(__GLIBC__)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static int crash_fd = -1;
static long exe_mtime;
static pid_t main_tid;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof buf)
		n = sizeof buf - 1;
	if (write(STDOUT_FILENO, buf, (size_t)n) < 0)
		{ /* nothing to do */ }
	if (crash_fd >= 0 && write(crash_fd, buf, (size_t)n) < 0)
		{ /* nothing to do */ }
}

/* "  #3  GLFrontier+0x1234 function" for one code address; i < 0: "  pc  ..." */
static void out_frame(int i, void *pc)
{
	Dl_info info;
	if (dladdr(pc, &info) && info.dli_fname)
	{
		const char *mod = strrchr(info.dli_fname, '/');
		mod = mod ? mod + 1 : info.dli_fname;
		char tag[8];
		snprintf(tag, sizeof tag, i < 0 ? "pc " : "#%-2d", i);
		out("  %s %s+0x%lx %s\n", tag, mod, (unsigned long)((char *)pc - (char *)info.dli_fbase),
			info.dli_sname ? info.dli_sname : "?");
	}
	else
		out("  #%-2d %p\n", i, pc);
}

static void crash_handler(int sig, siginfo_t *si, void *uc_v)
{
	static volatile sig_atomic_t in_handler;
	if (in_handler++)
		_exit(128 + sig); /* crashed while reporting */

	ucontext_t *uc = uc_v;
	void *pc = NULL;
#if defined(__x86_64__)
	pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	pc = (void *)uc->uc_mcontext.pc;
#endif

	time_t now = time(NULL);
	char when[32] = "";
	struct tm tm_now;
	if (localtime_r(&now, &tm_now))
		strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm_now);
	pid_t tid = (pid_t)syscall(SYS_gettid);

	out("\n==== CRASH %s: signal %d (%s), code %d\n", when, sig, strsignal(sig), si->si_code);
	out("thread %d (%s), exe mtime %ld\n", tid, tid == main_tid ? "main" : "not main", exe_mtime);
	out("fault address %p, where it happened:\n", si->si_addr);
	out_frame(-1, pc);

	/* game RAM is m68kram + a 32-bit game address: a fault in that 4 GB
	   window is the game code following a bad pointer */
	if (sig == SIGSEGV || sig == SIGBUS)
	{
		unsigned long long off = (unsigned long long)((char *)si->si_addr - (char *)m68kram);
		if ((char *)si->si_addr >= (char *)m68kram && off <= 0xffffffffull)
			out("=> game RAM address $%llx (RAM is $%x bytes)%s\n", off, MEM_SIZE,
				off >= MEM_SIZE ? ": past the end, a bad pointer" : "");
	}

	if (fe2vm_depth)
		out("second VM running game routine $%x (fe2vm_call)\n", fe2vm_cur_addr);
	else
		out("second VM idle (the main game was running)\n");

	out("game D: %08x %08x %08x %08x %08x %08x %08x %08x\n", Regs[0]._u32, Regs[1]._u32, Regs[2]._u32,
		Regs[3]._u32, Regs[4]._u32, Regs[5]._u32, Regs[6]._u32, Regs[7]._u32);
	out("game A: %08x %08x %08x %08x %08x %08x %08x %08x\n", Regs[8]._u32, Regs[9]._u32, Regs[10]._u32,
		Regs[11]._u32, Regs[12]._u32, Regs[13]._u32, Regs[14]._u32, Regs[15]._u32);
	uint32_t sp = Regs[15]._u32;
	if (sp + 32 <= MEM_SIZE)
	{
		out("game stack:");
		for (int i = 0; i < 8; i++)
			out(" %08x", (uint32_t)rdlong(sp + 4 * (uint32_t)i));
		out("\n");
	}

	void *frames[48];
	int n = backtrace(frames, 48);
	out("host stack (tools/crash_lines.py crash.log maps it to game lines):\n");
	for (int i = 0; i < n; i++)
		out_frame(i, frames[i]);

	hostcall_trace_dump(); /* goes through log_printf: stdout only */
	out("==== END\n");
	if (crash_fd >= 0)
		fsync(crash_fd);

	/* default action: the core dump */
	signal(sig, SIG_DFL);
	raise(sig);
}

void crash_log_install(void)
{
	main_tid = (pid_t)syscall(SYS_gettid);
	crash_fd = open("crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	struct stat st;
	if (stat("/proc/self/exe", &st) == 0)
		exe_mtime = (long)st.st_mtime;

	/* backtrace() loads libgcc on first use: do that now, not mid-crash */
	void *warm[2];
	backtrace(warm, 2);

	/* own stack, so a stack overflow is still reported */
	static char alt_stack[64 * 1024];
	stack_t ss = {.ss_sp = alt_stack, .ss_size = sizeof alt_stack};
	sigaltstack(&ss, NULL);

	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
	for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaction(sigs[i], &sa, NULL);
}

#else /* not Linux/glibc */

static void crash_handler(int sig)
{
	if (sig == SIGSEGV)
	{
		log_printf("Segfault! All is lost! Abandon ship!\n");
		if (fe2vm_depth)
			log_printf("second VM running game routine $%x\n", fe2vm_cur_addr);
		Call_DumpDebug();
		/* returning would re-run the faulting instruction forever */
		signal(SIGSEGV, SIG_DFL);
		raise(SIGSEGV);
	}
}

void crash_log_install(void)
{
	signal(SIGSEGV, crash_handler);
}

#endif
