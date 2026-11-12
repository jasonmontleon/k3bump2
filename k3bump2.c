/*
 * k3bump2 - the last 37,709 instructions of a rustc whose SpacemiT K3 (X100) hart stopped dead, replayed
 * in a loop on the dead process's own data, with the code mapping that process really had.
 *
 * Page contents and registers come from the memory dump of the dead process and are mapped at its
 * addresses; one pass runs the original instructions from its last kernel entry to the point where it
 * stopped.  The pass executes 134 code pages of librustc_driver.  The dead process had 3,648 of that
 * library's code pages mapped, and whether a run hangs is decided by one page that was NOT:
 * librustc_driver+0x2a20000
 * ("the page"; never executed; with a page-table entry no hang in 5 series of 300 runs, without one a
 * hang in 8 of 8 series).  The pass makes 13 far calls through PLT stubs whose predicted target, if the
 * predictor keeps only the low 21 bits of a target, is that page or the function start 32 bytes in
 * front of it; the last two, 2,845 and 2,474 instructions before the hart stopped, are of the second
 * kind.  Every code page that had an entry in the dead process has one here (bytes of the library
 * file), including the page in front of it, and the page has none.
 *
 * Before the loop a child process, in which the page IS mapped, measures whether this cpu fetches from
 * the page during a pass: after each of 40 rounds it times one load from six cache lines, among them
 * two the pass executes (these must show up, or the measurement cannot see anything) and one that
 * nothing points at (this must not show up).
 *
 * usage: k3bump2 [-t seconds] [-c cpu] [mapped] [nocheck] [-n] [-q]
 *        -t  how long to run (default 60)        -c  pin to this cpu (default 3, -1 = do not pin)
 *        mapped   control: the page HAS a page-table entry        nocheck   skip the measurement
 *        -n  keep the fresh heap page mapped, i.e. do not re-create the page fault that the original
 *            took right after its mprotect() call        -q  no progress lines
 * Every line also goes to ./k3bump2.log, written with O_SYNC.  If the hart hangs, the output stops.
 * Build: see Makefile.  Freestanding: no libc, no dynamic loader.
 */
typedef unsigned long u64;
typedef long s64;
#include "snap.h"
#include "extra.h"

extern const unsigned char snap_blob[], extra_blob[];
struct ctx { u64 regs[32]; u64 t0save; s64 cnt; u64 exit; };	/* offsets 0, 256, 264, 272 are known to the exit stub */
extern void enter_window(struct ctx *);
extern void window_exit(void);
extern void sled(void);							/* 128 KB of nops */
extern long sys6(long nr, long a, long b, long c, long d, long e, long f);

#define LIB  0x3ff3000000UL			/* librustc_driver-c614564fc90c622d.so in the dead process */
#define PAGE (LIB + 0x2a20000)			/* the page */

#define SYS_openat 56
#define SYS_write 64
#define SYS_pread64 67
#define SYS_exit_group 94
#define SYS_nanosleep 101
#define SYS_clock_gettime 113
#define SYS_sched_setaffinity 122
#define SYS_clone 220
#define SYS_mmap 222
#define SYS_mprotect 226
#define SYS_madvise 233
#define SYS_riscv_flush_icache 259
#define SYS_wait4 260

static struct ctx ctx;
static unsigned long npristine;
static const unsigned char *pristine[SNAP_NPAGES];
static unsigned char *live[SNAP_NPAGES];
static int logfd = -1, pagemap = -1;

void *memset(void *d, int c, unsigned long n) { unsigned char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static const int has_fault = SNAP_HAS_ECALL && SNAP_NEWHEAP != 0;	/* the window starts with mprotect() on a fresh heap page */

static void pr(const char *fmt, ...)		/* %s %d %x only; goes to stdout and to the O_SYNC log */
{
	char b[500]; int n = 0; __builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt && n < 460; fmt++) {
		if (*fmt != '%') { b[n++] = *fmt; continue; }
		fmt++;
		if (*fmt == 's') { for (const char *s = __builtin_va_arg(ap, const char *); *s && n < 460; s++) b[n++] = *s; }
		else { u64 v = __builtin_va_arg(ap, u64), base = *fmt == 'x' ? 16 : 10; char t[24]; int i = 0;
		       do t[i++] = "0123456789abcdef"[v % base]; while (v /= base);
		       while (i) b[n++] = t[--i]; }
	}
	__builtin_va_end(ap);
	sys6(SYS_write, 1, (long)b, n, 0, 0, 0);
	if (logfd >= 0) sys6(SYS_write, logfd, (long)b, n, 0, 0, 0);
}
static void die(const char *s, u64 v) { pr("k3bump2: %s 0x%x\n", s, v); sys6(SYS_exit_group, 2, 0, 0, 0, 0, 0); }
static u64 now_ms(void) { u64 ts[2]; sys6(SYS_clock_gettime, 1, (long)ts, 0, 0, 0, 0); return ts[0] * 1000 + ts[1] / 1000000; }
static void sleep_ms(u64 ms) { u64 ts[2] = { ms / 1000, ms % 1000 * 1000000 }; sys6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0); }
static long num(const char *s) { long v = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return neg ? -v : v; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void copy_page(unsigned char *d, const unsigned char *s)
{
	u64 *dd = (u64 *)d; const u64 *ss = (const u64 *)s;
	for (int i = 0; i < 512; i++) dd[i] = ss[i];
}
static u64 sum_written(void)
{
	u64 h = 0xcbf29ce484222325UL;
	for (unsigned long i = 0; i < npristine; i++) {
		const u64 *p = (const u64 *)live[i];
		for (int j = 0; j < 512; j++) h = (h ^ p[j]) * 0x100000001b3UL;
	}
	return h;
}
/* 1: the page at this address has a page-table entry, 0: it has none, -1: cannot tell */
static int has_pte(u64 addr)
{
	u64 e = 0;
	if (pagemap < 0 || sys6(SYS_pread64, pagemap, (long)&e, 8, addr / 4096 * 8, 0, 0) != 8) return -1;
	return e >> 63;
}
static const char *state(u64 addr)
{
	int st = has_pte(addr);
	return st < 0 ? "state unknown (cannot read /proc/self/pagemap)" : st ? "has a page-table entry" : "NO page-table entry";
}
static void map_page(u64 a, const unsigned char *bytes, int prot)
{
	long r = sys6(SYS_mmap, a, 4096, 3 /* RW */, 0x32 /* PRIVATE|ANONYMOUS|FIXED */, -1, 0);
	if ((u64)r != a) die("cannot map page at", a);
	copy_page((unsigned char *)a, bytes);
	if (prot != 3 && sys6(SYS_mprotect, a, 4096, prot, 0, 0, 0)) die("mprotect failed at", a);
}
static void map_the_page(void)
{
	map_page(PAGE, extra_blob + (u64)EXTRA_NPAGES * 4096, 5);
	sys6(SYS_riscv_flush_icache, PAGE, PAGE + 4096, 0, 0, 0, 0);
}

static void one_pass(int nofault)
{
	for (unsigned long i = 0; i < npristine; i++) copy_page(live[i], pristine[i]);
	if (has_fault && !nofault) {
		/* as in the original: the page is not there yet; the window's own mprotect() makes it usable
		 * and its first store takes the page fault */
		sys6(SYS_madvise, SNAP_NEWHEAP, 4096, 4 /* MADV_DONTNEED */, 0, 0, 0);
		sys6(SYS_mprotect, SNAP_NEWHEAP, 4096, 0 /* PROT_NONE */, 0, 0, 0);
	}
	for (int i = 0; i < 32; i++) ctx.regs[i] = snap_regs[i];
	ctx.cnt = SNAP_EXIT_COUNT; ctx.exit = (u64)window_exit;
	enter_window(&ctx);
}

/* ---- the measurement (child process, the page mapped) ----
 * Each round: push everything out of the caches (no flush instruction for user code on this board:
 * read 96 MB, then run 128 KB of nops for the instruction cache), run three passes, then either sleep
 * 1 ms or run the nops again (two ways of getting lines that only the instruction cache holds into the
 * levels a data load can see), then time one load from each line below. */
static inline u64 ticks(void) { u64 t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }
static u64 time_load(const volatile unsigned char *p) { u64 a = ticks(); unsigned char v = *p; u64 b = ticks(); (void)v; return b - a; }
static u64 median(u64 *v, long n)
{
	for (long i = 1; i < n; i++) { u64 x = v[i]; long j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
	return v[n / 2];
}
#define SCRUB (96UL << 20)
#define ROUNDS 40
static const struct { u64 addr; const char *what; } probe[6] = {
	{ PAGE + 0x800,	   "page+0x800 (never executed; predicted target of the 4 calls through stub +0x20800)" },
	{ PAGE + 0x000,	   "page+0x000 (never executed; where the function in front of the page runs on to)" },
	{ LIB + 0x2a1ffc0, "the function start in front of the page (never executed; predicted target of the 8 calls through stub +0x1ffe0)" },
	{ PAGE + 0xc00,	   "page+0xc00 (never executed, nothing points at it: must stay near 0)" },
	{ LIB + 0x2aae7c0, "Parser::bump's last lines (executed in every pass: must be well above 0)" },
	{ LIB + 0x2a94140, "the call site at the death point (executed in every pass: must be well above 0)" },
};
static void check(int nofault)
{
	volatile unsigned char *scrub;
	static u64 cold[200], hot[200], hit[2][6];
	u64 mc, mh, thr;
	long r = sys6(SYS_mmap, 0, SCRUB, 3, 0x22 /* PRIVATE|ANONYMOUS */, -1, 0);

	if (r < 0 && r > -4096) { pr("k3bump2: check: no memory\n"); return; }
	scrub = (volatile unsigned char *)r;
	for (u64 a = 0; a < SCRUB; a += 4096) scrub[a] = 1;
	if (has_pte(PAGE) != 1) map_the_page();
	for (int i = 0; i < 200; i++) {				/* what does a load from RAM / from the cache look like on this timer? */
		const volatile unsigned char *p = scrub + (i * 0x9E3779B97F4A7C15UL >> 50) * 4096 + 1024;
		cold[i] = time_load(p); hot[i] = time_load(p);
	}
	mc = median(cold, 200); mh = median(hot, 200); thr = (mc + mh + 1) / 2;
	pr("k3bump2: check, in a child process whose copy of the page is mapped: timer ticks per load, from RAM %d, from the cache %d\n", mc, mh);
	if (mc < mh + 2) { pr("k3bump2: check: this timer cannot tell the two apart here, no measurement\n"); return; }
	for (int round = 0; round < ROUNDS; round++) {
		for (u64 a = 0; a < SCRUB; a += 64) (void)scrub[a];
		sled();
		for (int k = 0; k < 3; k++) one_pass(nofault);
		if (round & 1) sled(); else sleep_ms(1);
		for (int j = 0; j < 6; j++) {			/* first a load from another line of the same page, so that the timed load does not pay for the data TLB */
			u64 a = probe[j].addr;
			(void)*(volatile unsigned char *)((a & ~4095UL) == PAGE ? PAGE + 0x5c0 : a ^ 0x800);
			hit[round & 1][j] += time_load((const volatile unsigned char *)a) < thr;
		}
	}
	for (int j = 0; j < 6; j++)
		pr("k3bump2: check: in the cache after 3 passes in %d of %d rounds (then 1 ms asleep), %d of %d (then 128 KB of other code): %s\n",
		   hit[0][j], (u64)ROUNDS / 2, hit[1][j], (u64)ROUNDS / 2, probe[j].what);
}

void k3main(long argc, char **argv)
{
	long secs = 60, cpu = 3, nofault = 0, quiet = 0, control = 0, nocheck = 0;
	u64 iter = 0, t0, tnext, first = 0, diverged = 0;
	int st;

	for (long i = 1; i < argc; i++) {
		if (eq(argv[i], "mapped")) control = 1;
		else if (eq(argv[i], "nocheck")) nocheck = 1;
		else if (argv[i][0] != '-') continue;
		else if (argv[i][1] == 't' && i + 1 < argc) secs = num(argv[++i]);
		else if (argv[i][1] == 'c' && i + 1 < argc) cpu = num(argv[++i]);
		else if (argv[i][1] == 'n') nofault = 1;
		else if (argv[i][1] == 'q') quiet = 1;
	}
	logfd = sys6(SYS_openat, -100, (long)"k3bump2.log", 01 | 0100 | 02000 | 04010000 /* O_WRONLY|O_CREAT|O_APPEND|O_SYNC */, 0644, 0, 0);
	pagemap = sys6(SYS_openat, -100, (long)"/proc/self/pagemap", 0, 0, 0, 0);
	if (cpu >= 0) {
		u64 mask[16] = { 0 };
		mask[cpu / 64] = 1UL << (cpu % 64);
		if (sys6(SYS_sched_setaffinity, 0, sizeof(mask), (long)mask, 0, 0, 0) < 0) die("cannot pin to cpu", cpu);
	}
	/* put every page where the dead process had it */
	for (unsigned long i = 0; i < SNAP_NPAGES; i++) {
		u64 a = snap_pages[i].addr; int k = snap_pages[i].kind;
		map_page(a, snap_blob + i * 4096, k == 0 ? 5 /* R+X */ : k == 1 ? 1 /* R */ : 3);
		if (k == 3) { pristine[npristine] = snap_blob + i * 4096; live[npristine++] = (unsigned char *)a; }
	}
	/* and the rest of the library code that had a page-table entry there */
	for (unsigned long i = 0; i < EXTRA_NPAGES; i++) map_page(extra_pages[i], extra_blob + i * 4096, 5);
	if (control) map_the_page();
	sys6(SYS_riscv_flush_icache, LIB, LIB + 0x338b000, 0, 0, 0, 0);

	pr("k3bump2: %d pages of the dead process plus %d more of its mapped library code; one pass = %d instructions%s; %s%d\n",
	   (u64)SNAP_NPAGES, (u64)EXTRA_NPAGES, (u64)SNAP_STEPS,
	   has_fault && !nofault ? " (starting with its mprotect() call and the page fault after it)" : "",
	   cpu >= 0 ? "pinned to cpu " : "not pinned, cpu argument -", (u64)(cpu >= 0 ? cpu : 1));
	pr("k3bump2:   code in front of the page, 0x%x: %s\n", PAGE - 32, state(PAGE - 4096));
	pr("k3bump2:   the page, 0x%x: %s%s\n", PAGE, state(PAGE), control ? " (CONTROL run)" : "");
	st = has_pte(PAGE);
	if (st >= 0 && st != control) die("the page is not in the wanted state", PAGE);

	if (!nocheck) {
		long pid = sys6(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
		int status = 0;
		if (pid == 0) { check(nofault); sys6(SYS_exit_group, 0, 0, 0, 0, 0, 0); }
		if (pid > 0) sys6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
		if (pid < 0 || status) pr("k3bump2: check: the measurement could not be made (child status 0x%x)\n", (u64)(unsigned)status);
		st = has_pte(PAGE);
		if (st >= 0 && st != control) die("the page changed state during the check", PAGE);
	}
	pr("k3bump2: now looping for %d s with the page %s\n", (u64)secs, control ? "mapped" : "without a page-table entry");
	sleep_ms(20);

	t0 = now_ms(); tnext = t0 + 1000;
	for (;;) {
		one_pass(nofault);
		iter++;
		if (iter <= 2 || (iter & 0xffff) == 0) {
			u64 h = sum_written();
			if (iter == 1) first = h;
			else if (h != first && !diverged) { diverged = iter; pr("k3bump2: MEMORY AFTER PASS %d DIFFERS from pass 1\n", iter); }
		}
		if ((iter & 0x3f) == 0 || iter == 1) {
			u64 t = now_ms();
			if (t >= tnext || iter == 1) {
				if (!quiet) pr("k3bump2: %d passes, %d s\n", iter, (t - t0) / 1000);
				tnext = t + 1000;
			}
			if (t - t0 >= (u64)secs * 1000) break;
		}
	}
	st = has_pte(PAGE);
	pr("k3bump2: finished, %d passes in %d s, no hang; memory after each checked pass %s; the page %s\n", iter, (u64)secs,
	   diverged ? "DIFFERED" : "identical", st < 0 ? "state unknown" : st ? "has a page-table entry" : "still has no page-table entry");
	sys6(SYS_exit_group, diverged ? 1 : 0, 0, 0, 0, 0, 0);
}
