/* k3lock: driver for the SpacemiT X100 hart lock-up reproducer.  See README.md.
 *
 * Maps the pages of the "window" (the code one pass executes, at the addresses it had in the original process, plus the few data
 * pages it uses), then loops:  copy the data page of the window's one real function back from a pristine copy NCOPY times,
 * load the starting registers, jump into the window, come back, count.
 * Run:  k3lock             six workers (harts 0,1,3,4,5,6) + two readers (harts 2,7)
 *       k3lock -c 3        one worker on hart 3 + readers on the three other harts of its cluster
 *       ... mapped          control: also map the page behind the straddling branch; must never lock
 * Options: -c LIST  -l LIST|none  -t SECONDS   (see k3main)
 * Freestanding: no libc. */
typedef unsigned long u64;
#include "build/pages.h"		/* generated: NPAGES, page_table[] */
extern const unsigned char page_blob[];	/* generated: the contents of those pages (build/pages.bin) */
struct ctx { u64 regs[32]; u64 t0save; long cnt; u64 exit; u64 fftab; };	/* offsets 256, 264, 272 are used by the exit stub */
extern void enter_window(struct ctx *);
extern void window_exit(void);
extern long sys6(long nr, long a, long b, long c, long d, long e, long f);
#define LIB  0x3ff3000000UL
#define PAGE (LIB + 0x2a20000)			/* the page behind the straddling branch: must have no mapping */
enum { SYS_openat = 56, SYS_write = 64, SYS_exit_group = 94, SYS_clock_gettime = 113, SYS_sched_setaffinity = 122, SYS_kill = 129,
       SYS_prctl = 167, SYS_clone = 220, SYS_wait4 = 260, SYS_mmap = 222, SYS_mprotect = 226, SYS_riscv_flush_icache = 259 };
static struct ctx ctx;
static const unsigned char zero_page[4096];
/* registers at the start of every pass; everything else is 0 */
static const u64 start_regs[32] = {
	[1]  = LIB + 0x2a9894c,		/* ra: piece_A in window.S, the first instruction of the pass */
	[2]  = 0x3feb8465b0UL,		/* sp: STACK_TOP in data.S */
	[18] = 0x3feb8499c0UL,		/* s2: OBJECT in data.S, handed to F1 */
	[31] = 0x3ff2ee276eUL,		/* t6: the `ret` the driver jumps to (entry_ret in window.S) */
};
static unsigned long ncopy;
static const u64 *pristine[NPAGES];
static u64 *live[NPAGES];
static int logfd = -1;

void *memset(void *d, int c, unsigned long n) { unsigned char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static void pr(const char *fmt, ...)		/* %s %d %x only */
{
	char b[300]; int n = 0; __builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt && n < 260; fmt++) {
		if (*fmt != '%') { b[n++] = *fmt; continue; }
		fmt++;
		if (*fmt == 's') { for (const char *s = __builtin_va_arg(ap, const char *); *s && n < 260; s++) b[n++] = *s; }
		else { u64 v = __builtin_va_arg(ap, u64), base = *fmt == 'x' ? 16 : 10; char t[24]; int i = 0;
		       do t[i++] = "0123456789abcdef"[v % base]; while (v /= base);
		       while (i) b[n++] = t[--i]; }
	}
	__builtin_va_end(ap);
	sys6(SYS_write, 1, (long)b, n, 0, 0, 0);
	if (logfd >= 0) sys6(SYS_write, logfd, (long)b, n, 0, 0, 0);
}
static void die(const char *s, u64 v) { pr("k3lock: %s 0x%x\n", s, v); sys6(SYS_exit_group, 2, 0, 0, 0, 0, 0); }
static u64 now_ms(void) { u64 ts[2]; sys6(SYS_clock_gettime, 1, (long)ts, 0, 0, 0, 0); return ts[0] * 1000 + ts[1] / 1000000; }
static long num(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return v; }
static void map_page(u64 a, const unsigned char *bytes, int prot)
{
	if ((u64)sys6(SYS_mmap, a, 4096, 3, 0x32 /* PRIVATE|ANONYMOUS|FIXED */, -1, 0) != a) die("cannot map page at", a);
	for (int i = 0; i < 512; i++) ((u64 *)a)[i] = ((const u64 *)bytes)[i];
	if (prot != 3 && sys6(SYS_mprotect, a, 4096, prot, 0, 0, 0)) die("mprotect failed at", a);
}
static void pin(long cpu)
{
	u64 m[16] = { 0 };
	m[cpu / 64] = 1UL << (cpu % 64);
	if (sys6(SYS_sched_setaffinity, 0, sizeof(m), (long)m, 0, 0, 0) < 0) die("cannot pin to cpu", cpu);
}

static int in_list(const char *l, long h)		/* is hart h in the comma-separated list? */
{
	while (*l) { if (*l >= '0' && *l <= '9' && num(l) == h) return 1; while (*l && *l != ',') l++; if (*l) l++; }
	return 0;
}
static void reader(long h)			/* a neighbour: read a 64 MB buffer in a loop, pinned to hart h */
{
	volatile unsigned char *b;
	sys6(SYS_prctl, 1 /* PR_SET_PDEATHSIG */, 9, 0, 0, 0, 0);
	pin(h);
	b = (volatile unsigned char *)sys6(SYS_mmap, 0, 64UL << 20, 3, 0x22 /* PRIVATE|ANONYMOUS */, -1, 0);
	for (u64 a = 0; a < (64UL << 20); a += 4096) b[a] = 1;
	for (;;) for (u64 a = 0; a < (64UL << 20); a += 64) (void)b[a];
}
static void worker(long cpu, long secs, int control, int many)
{
	u64 iter = 0, t0, tnext;

	if (many) sys6(SYS_prctl, 1 /* PR_SET_PDEATHSIG */, 9, 0, 0, 0, 0);
	pin(cpu);
	for (unsigned long i = 0; i < NPAGES; i++) {	/* kind 0: code (this includes the page that ends in half a branch), 1: read-only data, 2: data, 3: data copied back before every pass */
		u64 a = page_table[i].addr; int k = page_table[i].kind;
		map_page(a, page_blob + i * 4096, k == 0 ? 5 : k == 1 ? 1 : 3);
		if (k == 3) { pristine[ncopy] = (const u64 *)(page_blob + i * 4096); live[ncopy++] = (u64 *)a; }
	}
	if (control) map_page(PAGE, zero_page, 5);
	sys6(SYS_riscv_flush_icache, LIB, LIB + 0x338b000, 0, 0, 0, 0);
	pr("k3lock: cpu %d: %d pages, %d page copies per pass, the page behind the branch is %s\n", (u64)cpu, (u64)NPAGES, ncopy, control ? "MAPPED (control)" : "not mapped");
	t0 = now_ms(); tnext = t0 + 1000;
	for (;;) {
		for (unsigned long i = 0; i < ncopy; i++) { u64 *d = live[i]; const u64 *s = pristine[i]; for (int j = 0; j < 512; j++) d[j] = s[j]; }
		for (int i = 0; i < 32; i++) ctx.regs[i] = start_regs[i];
		ctx.cnt = 1; ctx.exit = (u64)window_exit;
		enter_window(&ctx);
		iter++;
		if ((iter & 0x3f) == 0 || iter == 1) {
			u64 t = now_ms();
			if (t >= tnext || iter == 1) {
				if (many) pr("k3lock: cpu %d: %d passes, %d s\n", (u64)cpu, iter, (t - t0) / 1000);
				else pr("k3lock: %d passes, %d s\n", iter, (t - t0) / 1000);
				tnext = t + 1000;
			}
			if (t - t0 >= (u64)secs * 1000) break;
		}
	}
	pr("k3lock: cpu %d finished, %d passes in %d s, no hang\n", (u64)cpu, iter, (u64)secs);
}

/* no arguments: workers on harts 0,1,3,4,5,6 and readers on harts 2,7 (two per 4-hart cluster share the cache with three workers).
 * -c LIST  worker harts.  With ONE worker the readers default to the three other harts of its cluster.
 * -l LIST  reader harts (`-l none`: no readers).   -t SECONDS (default 60).   mapped: control. */
void k3main(long argc, char **argv)
{
	long secs = 60, control = 0, nw = 0, pids[64], np = 0, one = -1;
	const char *cpus = "0,1,3,4,5,6", *load = 0;

	for (long i = 1; i < argc; i++) {
		if (argv[i][0] == 'm') control = 1;
		else if (argv[i][0] != '-' || i + 1 >= argc) continue;
		else if (argv[i][1] == 't') secs = num(argv[++i]);
		else if (argv[i][1] == 'c') cpus = argv[++i];
		else if (argv[i][1] == 'l') load = argv[++i];
	}
	for (long h = 0; h < 64; h++) if (in_list(cpus, h)) { nw++; one = h; }
	if (nw == 0) die("no worker hart in the -c list", 0);
	if (nw == 1) logfd = sys6(SYS_openat, -100, (long)"k3lock.log", 01 | 0100 | 02000 | 04010000 /* O_WRONLY|O_CREAT|O_APPEND|O_SYNC */, 0644, 0, 0);
	for (long h = 0; h < 64; h++) {				/* the readers */
		int want = load ? in_list(load, h) : nw == 1 ? (h / 4 == one / 4 && h != one) : (h == 2 || h == 7);
		if (!want || in_list(cpus, h) || np >= 60) continue;
		pids[np] = sys6(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
		if (pids[np] == 0) reader(h);
		pr("k3lock: reader process (pid %d) pinned to cpu %d reads a 64 MB buffer in a loop\n", (u64)pids[np], (u64)h); np++;
	}
	if (nw == 1) worker(one, secs, control, 0);
	else {
		long nr = np, left = 0;
		for (long h = 0; h < 64; h++) {
			if (!in_list(cpus, h) || np >= 64) continue;
			pids[np] = sys6(SYS_clone, 17, 0, 0, 0, 0, 0);
			if (pids[np] == 0) { worker(h, secs, control, 1); sys6(SYS_exit_group, 0, 0, 0, 0, 0, 0); }
			np++; left++;
		}
		while (left-- > 0) sys6(SYS_wait4, -1, 0, 0, 0, 0, 0);		/* a locked worker never exits: then this never returns either */
		np = nr;
	}
	for (long i = 0; i < np; i++) sys6(SYS_kill, pids[i], 9, 0, 0, 0, 0);
	sys6(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
