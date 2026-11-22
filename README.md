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
| `debug/reset-into-debug.tcl` | OpenOCD procedures for examining a hart after it has locked (see "Unlocking a locked hart with a debugger"). Not needed to reproduce the lock-up. |

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

## Unlocking a locked hart with a debugger

A locked hart ignores halt requests. It can be forced into debug mode by resetting that one core while the debug module's
"halt on reset" request is set for it. The hart then sits halted and its registers can be read. The reset clears the program
counter and the trap registers; it does not clear the integer registers (see the next section). The hart does not continue
the program afterwards.

Tested on 5 locked harts, 5 times successful (K3 CoM260).

### Requirements

- A JTAG probe connected to the K3's JTAG port.
- OpenOCD with the K3 target file (`target/spacemit-k3.cfg`, in OpenOCD since 2026), started AFTER the lock-up, with only the
  two X100 clusters:

  ```
  openocd -f <your-probe>.cfg -c "set CLUSTERS {0 1}" -f target/spacemit-k3.cfg
  ```
  OpenOCD prints for the locked hart, here hart 5:
  `Error: [k3.x100.5] Unable to halt. dmcontrol=0x00010001, dmstatus=0x00400ca3` and
  `Fatal: Hart 5 failed to halt during examine`. That is expected; the other harts are examined normally.
- A command connection to OpenOCD: `telnet localhost 4444`.

### Values for each hart

Find the row of the locked hart. Each command in the steps below is built from the values of that one row.

| N | PREFIX | DMCONTROL | DMSTATUS | COMMAND | DATA0 | DATA1 | HALTREQ | ARM | POP |
|---|---|---|---|---|---|---|---|---|---|
| 0 | `k3.x100.4` | 0x10 | 0x11 | 0x17 | 0x04 | 0x05 | 0x80000001 | 0x00000009 | 0x00000001 |
| 1 | `k3.x100.0` | 0x10 | 0x11 | 0x17 | 0x04 | 0x05 | 0x80010001 | 0x00010009 | 0x00000008 |
| 2 | `k3.x100.0` | 0x10 | 0x11 | 0x17 | 0x04 | 0x05 | 0x80020001 | 0x00020009 | 0x00000040 |
| 3 | `k3.x100.0` | 0x10 | 0x11 | 0x17 | 0x04 | 0x05 | 0x80030001 | 0x00030009 | 0x00000200 |
| 4 | `k3.x100.0` | 0x410 | 0x411 | 0x417 | 0x404 | 0x405 | 0x80000001 | 0x00000009 | 0x00010000 |
| 5 | `k3.x100.0` | 0x410 | 0x411 | 0x417 | 0x404 | 0x405 | 0x80010001 | 0x00010009 | 0x00080000 |
| 6 | `k3.x100.0` | 0x410 | 0x411 | 0x417 | 0x404 | 0x405 | 0x80020001 | 0x00020009 | 0x00400000 |
| 7 | `k3.x100.0` | 0x410 | 0x411 | 0x417 | 0x404 | 0x405 | 0x80030001 | 0x00030009 | 0x02000000 |

What the columns are:

- **N**: the number of the locked hart. The kernel names it, and so does the OpenOCD error shown above.
- **PREFIX**: the word every command starts with. OpenOCD calls hart N `k3.x100.N` (hart 0 is `k3.x100.0`, hart 1 is
  `k3.x100.1`, ...), and every command must start with the name of a hart that is NOT locked, because a locked hart is one
  OpenOCD could not attach to. The table suggests `k3.x100.0`, or `k3.x100.4` when hart 0 is the locked one. Several harts
  can lock in the same run: if the suggested hart is locked too (OpenOCD printed the "failed to halt" error for it as well,
  and commands through it fail with "Memory not ready"), use the name of any other hart instead. The prefix does not select
  which hart is acted on; the addresses and values in the other columns do.
- **DMCONTROL, DMSTATUS, COMMAND, DATA0, DATA1**: addresses of debug-module registers. Harts 0-3 and harts 4-7 have separate
  debug modules; the second one's registers are 0x400 higher.
- **HALTREQ, ARM**: values to write to DMCONTROL. Both contain the hart's index inside its debug module (bits 17:16) and
  `dmactive` (bit 0); HALTREQ adds `haltreq` (bit 31), ARM adds `setresethaltreq` (bit 3).
- **POP**: the hart's power-on-reset bit in the SoC register `APMU_PMU_CC2_AP` at physical address 0xd4282900 (bit numbers
  from Linux, `drivers/reset/spacemit/reset-spacemit-k3.c`, `RESET_APMU_CPUn_POP`).

### Steps

Type these in the telnet session, replacing each word in capitals by the value in the table row of the locked hart.

1. Stop OpenOCD's background polling:
   ```
   poll off
   ```
2. Confirm the hart is locked: request a halt, wait half a second, read the status.
   ```
   PREFIX riscv dmi_write DMCONTROL HALTREQ
   sleep 500
   PREFIX riscv dmi_read DMSTATUS
   ```
   A locked hart answers `0x400ca3` (bits 9 and 8, "halted", are 0). If the answer has 0x300 set, e.g. `0x4303a3`, the hart
   halted normally: it is not locked, stop here.
3. Withdraw the halt request and set "halt on reset" for this hart:
   ```
   PREFIX riscv dmi_write DMCONTROL ARM
   ```
4. Pulse the hart's power-on reset through the debug module's system-bus access:
   ```
   PREFIX riscv set_mem_access sysbus
   PREFIX mww phys 0xd4282900 POP
   sleep 50
   PREFIX mww phys 0xd4282900 0
   sleep 300
   ```
5. Read the status again:
   ```
   PREFIX riscv dmi_read DMSTATUS
   ```
   Expected: `0x4c03a3` (bits 19:18 "have reset" and bits 9:8 "halted"). The hart is now in debug mode.
6. Read a register: write an abstract command to COMMAND, then read DATA1 (upper 32 bits) and DATA0 (lower 32 bits).
   The command is 0x00320000 + register number; register numbers are 0x1000 + n for x0..x31 and the CSR number for a CSR
   (dpc is 0x7b1).
   ```
   PREFIX riscv dmi_write COMMAND 0x00321001
   PREFIX riscv dmi_read DATA1
   PREFIX riscv dmi_read DATA0
   ```
   This reads x1. On a hart locked by this reproducer it gave `0x3f` and `0xeb846500`, i.e. 0x3feb846500, every time.
   With 0x003207b1 (dpc) it gives `0x1` and `0x2ec`: 0x1000002ec, the address where the core starts after a reset.

`debug/reset-into-debug.tcl` contains the same steps as OpenOCD procedures: after `source debug/reset-into-debug.tcl`,
`hrescue N` does steps 1 to 5 and `hregs N` reads all integer registers and the trap registers.

Afterwards power-cycle the board. The system has been frozen since the lock-up, and the reset hart has lost its place.

Example: hart 0 is the locked hart, so the row N = 0 is used and every command starts with `k3.x100.4` (the output of each
command is after the arrow):
```
k3.x100.4 riscv dmi_write 0x10 0x80000001
k3.x100.4 riscv dmi_read 0x11                 -> 0x400ca3
k3.x100.4 riscv dmi_write 0x10 0x00000009
k3.x100.4 riscv set_mem_access sysbus
k3.x100.4 mww phys 0xd4282900 0x00000001
k3.x100.4 mww phys 0xd4282900 0
k3.x100.4 riscv dmi_read 0x11                 -> 0x4c03a3
k3.x100.4 riscv dmi_write 0x17 0x00321001
k3.x100.4 riscv dmi_read 0x5                  -> 0x3f
k3.x100.4 riscv dmi_read 0x4                  -> 0xeb846500
```

What does NOT work (tried on locked harts): a halt request alone; the debug module's own resets (`hartreset`, `ndmreset`);
the "software reset" bit next to POP (the POP value times 2), one try; the firmware's core power-down and wake-up registers;
and, from another hart of the same program, mapping the missing page, overwriting the half branch, or rewriting the data and
stack the locked hart was using.

Do not probe the second TAP in the K3's JTAG chain (IDCODE 0x08502c0d, 9-bit instruction register) with unknown
instructions: doing so froze the board.

### What the registers of a locked hart hold

How to read this. A core of this kind has more places to hold register values than the 31 architectural registers
(x1..x31, the names a program uses). These places are the physical registers. While the core runs, each architectural
register is mapped to one physical register, and the mapping changes constantly (register renaming): an instruction that
writes a register is given a fresh physical register from a free list. A reset puts the mapping back to its initial state but
does not clear the physical registers. So after the reset, reading an architectural register shows whatever the program
happened to leave in the physical register that name is mapped to at start-up: the VALUES are real leftovers of the locked
worker, but the NAME a value appears under means nothing. Which physical register a value was left in depends on the core's
state when it locked and differs from one lock-up to the next. (That this core is built this way is an assumption; it is the
usual design and it fits what is seen.)

Result on four locked harts: the same collection of values every time, under different names. Each row below is one value;
the hart columns show under which architectural register(s) it was read on that hart ("-" = not among the 31 that can be
read).

| value | what it is | hart 1 | hart 3 | hart 5 | hart 6 |
|---|---|---|---|---|---|
| 0x3ff5a9415e | return address of F1's second call (the instruction after the call in piece C) | a3 | a0 | s1 | s1 |
| 0x3feb8465b0 | stack pointer after F1 has returned (`STACK_TOP`) | s8 | a7 | s3 | s3 |
| 0x3feb846500 | stack pointer inside F1 | ra | ra | ra | ra |
| 0x3feb8499c0 | `OBJECT`, the pointer handed to F1 | t0, s1, a7 | t2, s1 | t1, s0 | t1, s0 |
| 0x3feb849a58 | `OBJECT` + 0x98 | t1 | a2 | a0 | a0 |
| 0x3feb849a70 | `OBJECT` + 0xb0 | - | t4, t5 | t5 | - |
| 0x3feb849a80 | `OBJECT` + 0xc0 | s2 | a6 | s2 | a7 |
| 0x16 | F1's cursor: 22, the value of the second call | a0 | a3 | a1 | a1 |
| 0xdf1 | a field of array element 22 | s6, t3 | s7, t3 | s7, t3 | s10, t5 |
| 0x38 | a field of array element 22 | s10 | s5 | s5 | s8 |
| 0x4 | a field of array element 22 | - | s9 | s10 | t3 |
| 0x400026da50000 | intermediate result of F1's shifts on those fields | - | s10 | s11 | t4 |
| 0x8000000000000 | intermediate result of F1's shifts on those fields | a2 | s11 | a2 | s4 |
| 0xffff | a mask F1 builds | t2 | t0 | t0 | t0 |
| 0x3ff30199bc | link value written by the `jalr t1, t3` in `stub_F1` | s3 | a4 | a7 | a6 |
| 0x3ff301ffec | link value written by the `jalr t1, t3` in `stub_helper2` | - | - | t4 | - |
| 0x3ff2ee276e | address of the `ret` the driver enters the pass through | t5 | - | - | - |
| 0x1 | constant | sp, gp | gp, tp | gp, tp | gp, tp |
| 0xffffffffffffffff | constant | s0 | s0 | t2 | t2 |
| 0x21e1b8 | a pointer into the driver's own data | tp | sp | sp | sp |
| 0 | (physical register holds 0) | 11 names | 9 names | 9 names | 11 names |

(Hart 1 was reset twice before it was read; harts 5 and 6 locked in the same run; a fifth locked hart, hart 0, was only
checked for x1 = 0x3feb846500 and the start address.)

What is never there: the values the final far call of piece C would produce (its `auipc` result 0x3ff5a89166 and its link
value 0x3ff5a9416e), and the stack pointer its target would set. That agrees with the instruction trace (F1 returns, four
no-ops complete, the far call does not), with two cautions: every pass computes the same values, so a value may be left over
from an earlier pass, and only 31 of the core's physical registers can be read this way.

After the reset the trap registers are cleared: `mepc`, `mcause`, `mtval`, `sepc`, `scause`, `stval` and `satp` read 0,
`mstatus` reads 0xa00001900, and `dpc` is 0x1000002ec, the address where the core starts after a reset.

### Markers: what the core executed in the pass it locked in

To learn more from those registers, variants of the program were made in which the values differ from pass to pass. The driver
puts the pass number (times 256) into a register the pass never uses, and some of the pass's no-ops are replaced by
instructions that add that number to a small tag and leave the sum in a register. A marker found after a lock-up therefore
shows which instruction wrote it and in which pass. Markers were put in the four instructions between F1's return and the
final far call, in six instructions of the far call's target, and at the start of piece C. All seven variants still
lock. (These variants are not in this repository.)

How to read the result: a marker that is found proves that its instruction executed in that pass. A marker that is not
found proves nothing by itself, because only 31 of the core's physical registers can be read (see above): the instruction may not
have executed, or its result may sit in a physical register that cannot be read.

40 register images from 26 lock-ups:

| marker | found |
|---|---|
| any marker found carries the number of the pass in which that hart locked | 40 of 40 |
| first instruction of piece C | 40 of 40 |
| 1st instruction after F1's return | 40 of 40, whichever register it writes |
| 2nd instruction after F1's return | 0 of 40 |
| 3rd instruction after F1's return | 9 of 31 |
| 4th instruction after F1's return (the last before the far call) | 0 of 40 |
| any of the six in the far call's target | 0 of 40 |

Established by this: the values in the registers of a locked hart are from the pass it locked in, not leftovers of earlier
passes; and in that pass the core executed past F1's return, at least as far as the third of the four instructions before the
far call.

Not established: whether the fourth instruction and the far call's target are executed. The registers never show a result
of the 2nd or 4th instruction, and they always show the OLD values of the two registers those instructions overwrite. The
instruction trace of the same variant, however, reports all four instructions as completed and only the far call as not.
These two do not agree, and telling which is right needs knowledge of how the core stores register values.

**A regularity that may matter: the state of register renaming (suggestive, not proven).** What is readable after a lock-up is almost the same every time:
the same results, the same old values, over 40 images, 26 lock-ups and 7 variants. If the core allocates physical registers
from a free list in rotation and the 31 readable ones are fixed, that should not happen when a hart stops at an arbitrary
moment: which results end up in readable physical registers would differ from one lock-up to the next. It would happen if
the hart always stops with its free list and rename map in the same state, i.e. if the lock-up needs one particular state of
register renaming.

One test was made. Three, and in another variant seven, extra register-writing instructions were inserted earlier in the
pass. If lock-ups could happen at any position of the rotation, the readable results should have moved by three or seven
places. They did not move: the result of the first instruction after F1's return was still the readable one, and of the
inserted instructions only the last was readable. That leaves two readings:

- the lock-up only happens at one position of the rotation, so the inserted instructions merely changed in which pass that
  position comes round, and what is readable stays fixed relative to the point where the hart stops; or
- the readable registers are not selected by rotation at all (for instance they show the last value given to each
  architectural register), and then the regularity says nothing about register renaming.

Neither reading fits every detail of the images, so this is offered as an observation for someone who knows the core's
register renaming, not as a finding.

## The earlier reproducer

The first reproducer in this repository, a replay of all 37,709 recorded instructions (15 MB), is on the branch
`v1-37k-instruction-replay`.
