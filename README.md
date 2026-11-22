# k3lock: a small program that hard-locks a SpacemiT K3 (X100) core

## What this is and why it exists

Fedora's RISC-V build system uses SpacemiT K3 Pico-ITX boards as package builders among other systems. When the Rust compiler 
package was built for the RVA23 profile on these builders, the builder repeatedly locked up hard: one core (hart) stopped 
executing, the kernel reported a hard lock-up on that CPU, and the machine had to be power-cycled. The same package built for 
the older rv64gc baseline completed on the same boards and kernels. The problem is tracked in
https://forge.fedoraproject.org/riscv/planning/issues/40.

This repository contains a small standalone program that causes the same lock-up within seconds, without a compiler, a build
root or any Fedora files. It was made by recording the last 37,709 instructions the Rust compiler executed on a hart before one
such lock-up, replaying them in a loop, and then removing everything that could be removed while the lock-up still occurred.
What is left is 298 instructions per loop, still at their original addresses, and 16 words of data. The on-chip instruction
trace shows that this program stops at the same instruction as the real compiler run did.

What it is for: giving someone who can look inside the core (or its firmware) a short, readable, fast test case. It shows
WHAT has to be present for the lock-up; it does not explain WHY the core stops.

What is and is not known about RVA23: the lock-up was found with RVA23 code, and the 298 instructions include five
vector-extension instructions (inside the function `F1`, two small vector copies). In one test, turning those five into
no-ops stopped the lock-up. Everything else in the program is plain rv64gc code, and the other things the lock-up needs (see
"The elements" below) have nothing to do with RVA23: a page that ends in half a branch instruction with no page mapped after
it, two-instruction far calls, and other cores of the same cluster busy with memory traffic. Whether code without any vector
instruction can trigger it has not been established either way.

It works on both K3 boards tried: a K3 CoM260 (8 x X100, Fedora 45, kernel 7.3.5), where all the experiments described
below were done, and a K3 Pico-ITX, the board type of the builders, where the lock-up was confirmed as well.
It works under both operating systems tried, Fedora and Bianbu (SpacemiT's own distribution).
Run counts are given below; several statements rest on one or two runs and are marked as such.

## Build and run

Build on any machine (cross build; needs `make`, `clang`, `lld`, `python3`; on Fedora: `dnf install make clang lld python3`):
```
make
```
Copy the one file `k3lock` to the K3 system and run it there:
```
./k3lock                    # six workers (harts 0,1,3,4,5,6) + two readers (harts 2,7); expect a stop within seconds
./k3lock -c 3               # one worker on hart 3 + readers on harts 0,1,2; can take minutes
./k3lock mapped             # control: must run to the end
                            # -c LIST worker harts   -l LIST|none reader harts   -t SECONDS (default 60)
```
Every worker runs the same 298 instructions over and over (one run-through is called a "pass" in this README) and prints
once a second how many passes it has done. A locked worker's count stops; on Fedora the kernel then reports a hard
lock-up on that CPU on the serial console (see below), and the rest of the system usually freezes within seconds because
it waits for the dead hart, so run it from the serial console or expect the ssh session to hang.

**A locked hart cannot be recovered: the board needs a power cycle. Do not run this on a machine you cannot power-cycle.**

## What happens when it runs

`./k3lock` starts six "worker" processes pinned to harts 0, 1, 3, 4, 5, 6 and two
"reader" processes on harts 2 and 7 (each reads a 64 MB buffer in a loop). Every worker repeats the pass forever and
prints its pass count once a second. On the CoM260 one or more workers stop within 6 to 10 seconds (4 of 4 runs; one to
three harts locked each time): such a hart no longer executes instructions, does not take interrupts, and cannot be halted
through the debug module (seen on this program's ancestors: dmstatus 0x00400ca3, "running", not "unavailable"; halt requests
are not acknowledged). The kernel reports a hard lock-up on that CPU a few seconds later, and the rest of the system usually
freezes soon after because it waits for the dead hart.

`./k3lock -c 3` is the smallest setup that works: one worker on hart 3 and readers on harts 0, 1, 2. It takes longer:
four runs locked after 5.5 s, 25 s, 170 s and 182 s.

`./k3lock mapped` (or `-c 3 mapped`) is the control: identical, except that the one page the reproducer leaves unmapped is
mapped. It has never locked (six workers: 120 s, 150 M passes in total; one worker: 2 runs of 240 s, 53 M passes each).

### What the kernel prints
With k3lock on Bianbu the kernel names the stopped hart like this (here hart 3):
```
[  127.401177] rcu: INFO: rcu_preempt detected stalls on CPUs/tasks:
[  127.404710] rcu:     3-...0: (7 ticks this GP) idle=79dc/1/0x4000000000000000 softirq=1457/1459 fqs=1942
[  127.414049] rcu:              hardirqs   softirqs   csw/system
[  127.419689] rcu:      number:        0          0            0
[  127.425330] rcu:     cputime:        0          0            0   ==> 10528(ms)
[  127.432386] rcu:     (detected by 5, t=5259 jiffies, g=2793, q=38 ncpus=16)
```
On Fedora it is `watchdog: CPUn: Watchdog detected hard LOCKUP on cpu 3`, followed by soft lock-ups of the other CPUs.

## The files

| file | what |
|---|---|
| `window.S` | The code that triggers the lock-up: the 298 instructions taken from the Rust compiler run, in assembly. A hart runs through them once, start to end, and then starts over; this README calls one such run-through a "pass". Each piece of the code is placed at the same memory address it had inside the compiler, because the lock-up depends on those addresses. |
| `data.S` | The memory contents that code needs: the 16 data words it reads, and one page of memory that is never executed and ends in the first half of a branch instruction (see "The elements" below). |
| `k3lock.c` | The driver, ordinary C. It puts the contents of `window.S` and `data.S` at their addresses, starts the worker and reader processes, and makes each worker repeat the pass and print how many passes it has done. |
| `k3lock_asm.S` | A few lines of assembly the driver needs: the program's entry point, the system-call helper, and the jump from the driver into the pass and back. |
| `build-assets/` | Used only while building; nothing in here runs on the target. `layout.ld` lists the memory address of every piece of `window.S` and `data.S`; `mkpages.py` assembles the two files for those addresses and packs the result into a table that is compiled into the driver. |

`make` produces one static binary, `k3lock` (no libc, no dynamic loader). Generated files go to `build/`.

`window.S` was produced by a tool from a program that had been cut down step by step from a recorded rustc run; the tool
checked that the file assembles to exactly the bytes that were tested. Labels of the form `L_2a9415e` are the offset of that
instruction in the original library (librustc_driver, mapped at 0x3ff3000000 in the recorded process).

## One pass (298 instructions, 55 of them no-ops; the driver adds about 21,000 between passes, nearly all of it the page copy)

```
driver:   copy the object's data page back from a pristine copy, 8 times   (plain C loop, 2,560 instructions per copy)
          jump to a `ret` whose return address is piece_A
piece A:  far call  helper1                (helper1 is just `ret`)
          far call  stub_F1  -> F1         (F1: an ordinary compiled function; 94 instructions on the path taken, 61 of them loads/stores)
          far call  stub_helper2 -> helper2 (`ret`)      <- the call whose wrong target guess matters, see below
          jump to piece B
piece B:  two jumps, four branches, three instructions that pick one branch's direction; no calls
glue:     set F1's cursor to another array element
piece C:  far call  helper1
          far call  stub_F1  -> F1
          far call  far_target             <- on a pass that locks, this call never completes
far_target: 2 instructions, jump back to the driver
```
"far call" = `auipc ra, ...` + `jalr ra, ...(ra)`, the two-instruction call compilers emit when the target may be out of
reach of `jal`. The stubs are PLT-style: `auipc t3; ld t3, ...(t3); jalr t1, t3`.

Where it stops was recorded with the on-chip instruction trace (vendor 6.18 kernel) on this program: the last
branch message is F1's return to lib+0x2a9415e in piece C; after that the trace is silent until the read-out 3.2 s later,
whose closing message reports 4 more instructions completed. Those are the four no-ops between the return and the last far
call, so the `auipc`/`jalr` pair of that call at lib+0x2a94166 never completes. This is the same ending as the trace of a real
rustc build that locked up (2026-11-11) and of a 588-instruction ancestor of this program: same return address, same
count of 4.

## The elements, and what happens when each is removed

Each line is a variant that differs from a locking one in that element only. "No lock" figures are passes without a stop.
Most were single runs of 120 s with six workers on six harts (see "How it was tested"); a single surviving run is weak
evidence (the 588-instruction ancestor survived a 4-minute single-worker run about one time in ten).

1. **A page that is never executed, ends in the first half of a 4-byte conditional branch, and is followed by an unmapped
   page.** `data.S`, section `.front` (0x3ff5a1f000; the unmapped page is 0x3ff5a20000).
   - next page mapped: never locked, in every setup tried (more than 12 runs on earlier versions; this
     version: 2 control runs of 240 s, 53 M passes each, no stop);
   - the last two bytes changed to a no-op, or to the first half of `addi`, `jal`, `jalr`, `auipc` instead of a conditional
     branch: no lock (2 runs each, earlier version); three different conditional-branch encodings: lock.
   The program never jumps there. The far call in piece A at lib+0x2a98a0c goes to `stub_helper2` at lib+0x1ffe0, 42 MB away;
   the page in front of the unmapped page covers lib+0x2a1f000..0x2a1ffff, i.e. the same offset (0x1ffe0) inside the call
   site's own 2 MB region. Our reading (not proven): the core predicts the call's target with the upper address bits of the
   call site and the lower 21 bits of the real target, fetches from lib+0x2a1ffe0 on that wrong guess, runs through the
   no-ops to the end of the page and starts on the half branch.
2. **The far calls must be the two-instruction `auipc`+`jalr` form, and the first helper call and the call to helper2 must
   stay** (replacing a far call by `nop; jal`: no lock, 2 runs each on the earlier version; removing the first helper1 call,
   or the helper1 call in piece C: no lock, 1 run each).
3. **F1 must run as it is.** F1 is ordinary compiler output: it saves 13 registers, reads a cursor and a 32-byte array
   element, copies fields around (some with 16-byte and 8-byte vector copies, some through two stack slots that are written
   with one size and read back with another), restores the registers and returns.
   - all of F1's loads and stores turned into no-ops, everything else kept: no lock (6 harts x 6 M passes);
   - only F1's loads and stores kept and all other data handling in the pass removed: all six harts locked within 2 s;
   - hand-written stand-ins with the same register saves/restores and the same number of plain loads and stores
     (57-58, with or without a walk into the array): no lock, 3 variants, about 100 M passes each;
   - small edits inside F1, each alone (the 8-byte vector copy; the 16-byte vector copy; all vector instructions; the two
     stack stores `sd a2,0x28(sp)` / `sw a4,0x30(sp)`; the one narrow reload `lw a0,0x28(sp)`; three byte/halfword
     stores): no lock, 53 M passes each, while the unmodified program on a neighbouring hart of the same kind of run locked
     at 453k passes.
   So it is not the amount of memory activity; F1's particular sequence matters, down to single instructions. Which property
   of the sequence is the one that counts is not known. (On larger ancestors of this program some of F1's loads and stores
   could be removed and it still locked, only more rarely.)
4. **F1 is called twice and its second call reads a different array element** (no cursor change: no lock, 2 x 2.6 M;
   first call removed: no lock, 1 run).
5. **Piece B** (now only jumps and branches): skipping it: no lock (1 run, 6 M passes).
6. **Between passes the driver copies F1's data page.** No copy: no lock in 51 M passes. Copies of other pages only: no lock
   (9 M). 8 or 16 copies of F1's page: lock; 6: lock (once, 1.15 M passes); 4 or 2: no lock (5.7 M / 8 M). F1's page once plus 7 or 15
   copies of an unrelated page: lock. So: F1's page must be among the copied ones, and about 6-8 page copies in total are needed. The same copy loop
   placed as code inside the pass instead of in the driver: no lock (about ten variants, 7-13 M passes each). Unexplained.
7. **Other harts of the same cluster must be busy with memory traffic.** One worker with one reader: no lock in 107 M
   passes (2 runs). One worker with three readers: lock (5 of 5 runs, 5 s to 182 s). Three workers and one reader: lock.
   For the larger ancestors one reader was enough, and a worker entirely alone never locked.
8. **The address of the driver's code: not critical for this build.** Built at ten different addresses (`TEXT` = 0x20000,
   0x31000, 0xe0000, 0x100000, 0x200000, 0x201000, 0x210000, 0x300000, 0x400000, 0x800000) every one locked at least once
   (six workers on six harts, each lock within 0.5-1.6 M passes). Earlier intermediate variants did react to it: one never
   locked in 7.4 M passes at one address and locked within 300k passes one page further. Unexplained.

Not needed (removed without losing the lock-up): everything else the original 37,000-instruction window did; vector
instructions outside F1; any data outside the 16 words in `data.S`; all register contents except ra, sp and one pointer;
the code of helper1, helper2 and of a third helper (they just return); all pages except the 19 the pass uses (12 code pages, 2 pages with the stubs' address words, 4 data pages, and the
page that ends in half a branch).

## How it was tested

- "Six-hart run": six different variants, one per hart (0, 1, 3, 4, 5, 6), readers on harts 2 and 7, 120 or 240 s. After a
  stop the harts are asked to halt over JTAG; a hart that does not halt and whose pass counter had stopped is counted as
  locked. The other variants of that run are then undecided (the board has to be power-cycled).
- This program, default mode (`./k3lock`): 4 of 4 runs stopped within 6-10 s (one to three harts locked each time).
  `./k3lock mapped`: 2 of 2 runs of 120 s without a stop (about 25 M passes on each of the six workers).
- This program, one worker (`./k3lock -c 3`): 6 of 6 runs locked (5.5 s, 8 s, 25 s, 35 s, 170 s, 182 s).
  `./k3lock -c 3 mapped`: 2 of 2 runs of 240 s without a stop (53 M passes each).

## The earlier reproducer

The first reproducer in this repository, a replay of all 37,709 recorded instructions (15 MB), is on the branch
`v1-37k-instruction-replay`.
