# k3lock: a small program that hard-locks a SpacemiT K3 (X100) core

## What this is and why it exists

Fedora's RISC-V build system uses SpacemiT K3 Pico-ITX boards as package builders among other systems. When the Rust compiler
package was built for the RVA23 profile on these builders, the builder repeatedly locked up hard: one core (hart) stopped
executing, the kernel reported a hard lock-up on that CPU, and the machine had to be power-cycled. The same package built for
the older rv64gc baseline completed on the same boards and kernels. The problem is tracked in
https://forge.fedoraproject.org/riscv/planning/issues/40.

This repository contains a small standalone program that causes the same lock-up within seconds, without a compiler, a build
root or any Fedora files. It was made by recording the last 37,709 instructions the Rust compiler executed on a hart before
one such lock-up, replaying them in a loop, and then removing everything that could be removed while the lock-up still
occurred. What is left is a path of about 150 instructions, still at their original addresses, and a few words of data.

What it is for: giving someone who can look inside the core (or its firmware) a short, readable, fast test case. It shows
WHAT has to be present for the lock-up; it does not explain WHY the core stops.

**RVA23 is not required.** The lock-up was found with RVA23 code, and earlier versions of this program contained five vector
instructions and one `c.lbu`. This version contains only rv64gc instructions plus one cache-flush instruction (`cbo.flush`,
Zicbom extension). It can therefore be run unchanged on other RISC-V boards that have Zicbom, where it should simply run to
the end.

**One fault, two programs.** [k3skip](https://github.com/jasonmontleon/k3skip) is the same program with one number
changed (6 instead of 4 no-ops in front of the last far call). There the hart does not lock; an instruction is skipped
instead and the program computes a wrong result. See "One fault, several forms".

## Build and run

Build on any machine (cross build; needs `make`, `clang`, `lld`, `python3`; on Fedora: `dnf install make clang lld python3`):
```
make
```
Copy the one file `k3lock` to the K3 system and run it there:
```
./k3lock                    # six workers, pinned to harts 0,1,3,4,5,6; expect a stop within seconds
./k3lock -c 3               # one worker on hart 3
./k3lock mapped             # control: must run to the end
                            # -c LIST worker harts   -t SECONDS (default 60)
```
Every worker runs the same instructions over and over (one run-through is called a "pass" in this README) and prints once
a second how many passes it has done. A locked worker's count stops; on Fedora the kernel then reports a hard lock-up on
that CPU on the serial console (see below), and the rest of the system usually freezes within seconds because it waits for
the dead hart, so run it from the serial console or expect the ssh session to hang.

A worker that reaches the end of its time prints
```
k3lock: cpu 3 finished, 251373248 passes in 180 s, no hang
k3lock: cpu 3 SUMMARY: 251373248 passes, 0 traps, 0 wrong results
```
If the system does not allow user programs to execute `cbo.flush`, the program says so and exits with status 3.

**A locked hart cannot be recovered: the board needs a power cycle. Do not run this on a machine you cannot power-cycle.**

## Results

This version (rv64gc plus Zicbom). "7.3" is a mainline 7.3.5 kernel with board patches; "vendor" is SpacemiT's 6.18.

| Board | System | Run | Result |
|---|---|---|---|
| K3 CoM260 | Fedora, 7.3 | `./k3lock`, 3 runs | locked within 8 s every time; all six worker harts did not answer a debugger halt request |
| K3 CoM260 | Fedora, 7.3 | `./k3lock -c 3`, 2 runs | locked within 8 s both times; hart 3 only |
| K3 CoM260 | Fedora, 7.3 | `./k3lock -t 180 mapped` | ran to the end, 1.36 G passes |
| K3 CoM260 | Fedora, vendor 6.18 | `./k3lock`, 3 runs | locked every time |
| K3 CoM260 | Fedora, vendor 6.18 | `./k3lock mapped` | ran to the end, 488 M passes, 0 wrong results |
| K3 Pico-ITX | Fedora, 7.3 | `./k3lock` | locked after about 1 s |
| K3 Pico-ITX | Bianbu | `./k3lock` | locked after 21 s (harts 0 and 6 reported stalled) |
| Milk-V Titan (not a K3) | 7.3 | `./k3lock` | ran to the end: 86.6 M passes, 0 traps, 0 wrong results |
| SpacemiT K1 (X60 cores), Milk-V Jupiter | | `./k3lock` | ran to the end: 620 M passes, 0 traps, 0 wrong results |
| SpacemiT K1 (X60 cores), Banana Pi BPI-F3 | | `./k3lock` | ran to the end: 585 M passes, 0 traps, 0 wrong results |

## What happens when it runs

`./k3lock` starts six worker processes pinned to harts 0, 1, 3, 4, 5, 6. Nothing else is needed; the board can be idle
otherwise. Every worker repeats the pass and prints its pass count once a second. On a K3 one or more workers stop within
seconds: such a hart no longer executes instructions, does not take interrupts, and cannot be halted through the debug
module (dmstatus 0x00400ca3, "running", not "unavailable"; halt requests are not acknowledged). The kernel reports a hard
lock-up on that CPU a few seconds later, and the rest of the system usually freezes soon after because it waits for the dead
hart.

`./k3lock mapped` is the control: identical, except that the one page the reproducer leaves unmapped is mapped.

About one worker process in three does not lock quickly even when the run as a whole locks every time, so a single worker
(`-c 3`) can run for a long time; use the six-worker form for a yes/no answer.

### What the kernel prints
On Bianbu the kernel names the stopped hart like this (here hart 3):
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
| `window.S` | The code that triggers the lock-up, in assembly. A hart runs through it once, start to end, and then starts over; this README calls one such run-through a "pass". Each piece of the code is placed at the same memory address it had inside the compiler, because the lock-up depends on those addresses. |
| `data.S` | The memory contents that code needs: the few data words it reads, and one page of memory that is never executed and ends in the first half of a branch instruction. |
| `k3lock.c` | The driver, ordinary C. It puts the contents of `window.S` and `data.S` at their addresses, starts the workers, and makes each worker repeat the pass and print how many passes it has done. |
| `k3lock_asm.S` | A few lines of assembly the driver needs: the program's entry point, the system-call helper, and the jump from the driver into the pass and back. |
| `build-assets/` | Used only while building; nothing in here runs on the target. `layout.ld` lists the memory address of every piece of `window.S` and `data.S`; `mkpages.py` assembles the two files for those addresses (with `-march=rv64gc`) and packs the result into a table that is compiled into the driver. |
| `debug/reset-into-debug.tcl` | OpenOCD procedures for examining a hart after it has locked (see "Unlocking a locked hart with a debugger"). Not needed to reproduce the lock-up. |

`make` produces one static binary, `k3lock` (no libc, no dynamic loader). Generated files go to `build/`.

Labels of the form `L_2aae6d6`, and `lib+X` in comments, are offsets in the original library (librustc_driver, mapped at
0x3ff3000000 in the recorded process).

## One pass

```
driver:   load five registers, jump to a `ret` whose return address is `start`
start:    beq (taken)
part 1:   far call helper1 (`ret`);  c.bnez (never taken)
          a0 = object;  far call stub_F1 -> F1                first call
          far call stub_helper2 -> helper2 (`ret`)            its wrong target guess is the page that ends in half a branch
part 2:   jump;  beq (taken);  bne (taken);  bne (never taken)
part 3:   jump;  c.beqz (taken)
          cbo.flush of one 64-byte line of F1's stack frame
          far call helper1;  c.bnez (never taken)
          a0 = object;  far call stub_F1 -> F1                second call
          4 two-byte no-ops
          far call far_target                                 on a pass that locks, this call never completes
far_target: jump to exit_stub, which goes back to the driver
```
"far call" = `auipc ra, ...` + `jalr ra, ...(ra)`, the two-instruction call compilers emit when the target may be out of
reach of `jal`. The stubs are PLT-style: `auipc t3; ld t3, ...(t3); jalr t1, t3`.

`F1` is a real function from the compiler (about 95 instructions on the path taken: it saves 13 registers, reads a cursor
and a 32-byte array element, copies fields around, restores the registers and returns). In the original it copies 16 and 8
bytes with five vector instructions; here each of those is one floating-point load or store of the same bytes.

`exit_stub` also carries a check that has nothing to do with the lock-up: eight `ori` instructions each set one bit of a
register, and the driver verifies that the result is 0xff (the "wrong results" count in the summary line). It is what
[k3skip](https://github.com/jasonmontleon/k3skip) uses.

Where it stops was recorded with the K3's on-chip instruction trace (vendor 6.18 kernel) on the earlier, larger versions of
this program and on a real rustc build that locked up: the last branch message is F1's return to lib+0x2a9415e in part 3;
after that the trace is silent until the read-out 3.2 s later, whose closing message reports 4 more instructions completed.
Those are the four no-ops between the return and the last far call, so the `auipc`/`jalr` pair at lib+0x2a94166 never
completes (3 of 3 captures). This version has not been traced again; its code at that place is the same.

## What the pass needs

Measured with the RVA23 form of this pass (F1 with its vector instructions; otherwise the same), on a K3 CoM260 under
Fedora, kernel 7.3.5. Each line was removed or neutralised alone (same-size no-ops, or a plain jump in place of a taken
conditional branch, so nothing else moves) and run as six workers for 60 s, about 80 M passes per worker:

| change | result |
|---|---|
| the page behind the half branch mapped | no lock (2 runs of 240 s, 1.9 G and 2.0 G passes) |
| the taken `beq` at the start -> plain jump | no lock |
| first helper1 call removed | no lock |
| never-taken `c.bnez` behind it removed | no lock |
| first call of F1 removed | no lock |
| helper2 call removed | no lock |
| never-taken `bne` of part 2 removed | no lock |
| taken `c.beqz` of part 3 -> plain jump | no lock |
| the flush removed | no lock (308 M passes per worker) |
| second helper1 call removed | no lock |
| never-taken `c.bnez` behind it removed | no lock |
| 3 instead of 4 no-ops before the final far call | no lock (another failure instead, see below) |
| the taken `beq` of part 2 -> plain jump | locks (but with that transfer removed altogether: no lock, 3 runs) |
| the taken `bne` of part 2 -> plain jump | locks |
| F1 without its odd-size stack stores and reloads | locks |
| F1 with its four conditional branches neutralised | no lock |
| F1 with its data loads and stores as no-ops | no lock |
| the parts packed together at other addresses (3 layouts) | no lock |

Further conditions:

- **The page.** It is never executed, ends in the first half of a 4-byte conditional branch, and is followed by an unmapped
  page (`data.S`, section `.front`, 0x3ff5a1f000; the unmapped page is 0x3ff5a20000). With the last two bytes changed to a
  no-op, or to the first half of `addi`, `jal`, `jalr` or `auipc`: no lock (2 runs each, on an earlier version); three
  different conditional-branch encodings: lock. The program never jumps there. The far call in part 1 goes to
  `stub_helper2` at lib+0x1ffe0, 42 MB away; the page covers lib+0x2a1f000..0x2a1ffff, i.e. the same offset (0x1ffe0)
  inside the call site's own 2 MB region. Our reading (not proven): the core predicts the call's target with the upper
  address bits of the call site and the lower 21 bits of the real target, fetches from lib+0x2a1ffe0 on that wrong guess,
  runs through the no-ops to the end of the page and starts on the half branch.
- **Both calls of F1 must go to the same function through the same stub.** With either call sent to an exact copy of F1 at
  another address (own stub): no lock in 460-700 M passes. With both calls sent to the copy: it locks, but took 227 M passes.
- **The helper2 call must lie on the path that follows F1's FIRST return address.** With the helper2 call moved in front of
  F1's first call, so that the program makes the same calls in another order: no lock in 1.4 G passes on six harts. How far
  behind the return address it lies does not matter (0, 13, 15, 29 or 77 no-ops in between: all lock within 8 s; this
  version has 11).
- **The flush must come before F1's second call**, or before the first with another stack line; flushing the same line
  before both calls: no lock in 1.2 G passes. Flushing lines of the object or of the table instead of the stack: no lock.
  (The flush stands in for what earlier versions did with page copies between passes and memory traffic on neighbouring
  harts: it makes F1's stack slow to reach.)
- **The final far call** must be the adjacent `auipc`+`jalr` pair (`auipc ra; c.nop; jalr ra`: no lock in 315 M passes;
  `nop; jal` instead: no lock), and the number of instructions between F1's return address and the `auipc` decides what
  happens (next section). It is the number of instructions, not of bytes: 2 four-byte no-ops behave like 2 two-byte ones.
- **F1's copies.** Deleting the five copy instructions (vector or floating-point) without replacement nearly switches
  the failure off; whether they are vector or floating-point instructions makes no difference to the lock-up.

Our reading (inferred from which variants lock; the wrong path itself was never observed): F1's `ret` is one instruction
that returns to two places in every pass. At the second return the core guesses the first return address. Because F1's
stack line was flushed, the true return address arrives late and the wrong guess lives long enough to run on to the helper2
call, whose own wrong target guess leads into the page that ends in half a branch with an unmapped page behind it. The
conditional branches that do nothing for the program (taken at the head of each part, never taken behind each helper call,
and those inside F1) would then matter through the core's record of recent branch outcomes: the failure needs that record
in one particular state. That the single edits are not independent (two of them can each be made alone, but not both) fits.

## One fault, several forms

The number of two-byte no-ops in front of the last far call selects what is seen (RVA23 form of the pass, six workers):

| No-ops | Seen |
|---|---|
| 2 or 4 | the hart locks |
| 3 or 6 | wrong results: an instruction is skipped, silently |
| 0, 1, 5 or 8 | traps at addresses just behind the far call, and wrong results |

The conditions in the table above are the same for the lock-up and for the wrong results: every change that removes the
one removes the other, including mapping the page.

The wrong results and traps were looked at with the on-chip instruction trace (five captures, vendor 6.18 kernel). In all
five a timer interrupt was taken within a few instructions after the last far call, and the return address the core gave
the kernel (`sepc`) was wrong: in four cases the address of the interrupted instruction plus 4, in one 8 bytes behind a
jump that had already been taken. The kernel returned there, so one instruction was skipped. Depending on which instruction
that is, the program computes a wrong value, runs into bytes that are not code (illegal-instruction trap), or jumps to
address 0.

With the rv64gc form in this repository: 4 no-ops lock (this program), 6 give wrong results
([k3skip](https://github.com/jasonmontleon/k3skip)), 3 locked on the vendor kernel (4 of 4 runs) and not on the 7.3 kernel
(3 runs of 180 s), 2 locked in 1 of 3 runs on the 7.3 kernel.

Whether the lock-up also begins with an interrupt is not known.

## Cautions about the method

- A single worker process that survives proves little (see above); the tables use six workers of the same variant.
- "No lock" always means "not in that many passes". Counts are given so the reader can judge.
- Results were obtained on one K3 CoM260 unless another board is named.

## Unlocking a locked hart with a debugger

Everything in this section was measured on the earlier, larger version of this program (commit `585c8d1`). In that
version the three parts of the pass were called piece A, B and C (piece C = part 3 here), and F1's second call read array
element 22. The procedure itself does not depend on the version.

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

Established by this alone: the values in the registers of a locked hart are from the pass it locked in, not leftovers of
earlier passes, and in that pass the core executed past F1's return.

**Calibrating the read-out.** To learn what "not found" means, small programs whose behaviour is known were run in machine
mode on a healthy hart, which was then reset and read in the same way:

- a program in which every instruction writes its own sequence number: readable afterwards were the last write and the nine
  most recent EVEN-numbered writes before it, without a gap, and no odd-numbered write. So every second register write lands
  where it cannot be read, and of the others the most recent ones can all be read;
- a program shaped like the end of the pass (a return, four marker instructions, a far call, six markers in its target), in
  which everything certainly executes: readable were the 1st and 3rd markers after the return and the 2nd, 4th and 6th in the
  target; not readable were the 2nd and 4th after the return (2 runs, same result; a third run was unusable).
- (Values written to a halted hart through the debugger behave differently, nearly all stay readable, so the calibration
  has to use code running at full speed.)

Read with that, the table says:

- the 2nd and 4th instructions after the return are of the kind that can never be seen; their absence means nothing;
- the far call's target was not executed in the pass that locked: three of its markers are readable whenever it runs, and
  none was found in any of the 27 images of variants that had them;
- the 3rd instruction after the return, readable whenever it runs, was found in only 9 of 31 images: in most lock-ups the
  core had executed the 1st instruction after the return and not the 3rd.

So by the registers the core stops within one to three instructions after F1's return. The instruction trace reports four
instructions completed after the return in every capture (3 of 3). The two still disagree, by a couple of instructions; which
is right cannot be told from outside the core.

**A regularity that may matter: the state of register renaming (suggestive, not proven; written before the calibration
above, which weakens it: the calibration programs, which stop nowhere in particular, also leave a regular pattern, so a
regular pattern is at least partly a property of the read-out).** What is readable after a lock-up is almost the same every time:
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

## Vendor control registers that change the outcome

The X100 has control registers that the vendor firmware sets at start-up (numbers from SpacemiT's OpenSBI, which names only
one of the bits involved). Bits were changed on all eight harts with a debugger before the test program was started. What
the bits mean is not documented anywhere we could find.

| Register | Firmware value | Change | Effect |
|---|---|---|---|
| `CSR_MVMD1` (0xBF9) | 0x2000000000 | set bit 3 (0x2000000008) | no events in about 950 M passes in three layouts that give wrong results or traps; in the two layouts that otherwise lock: no lock-up (2 runs of 120 s), but 14 wrong results in 1.9 G passes. About 4 % slower in the test loop |
| `CSR_MVMD1` (0xBF9) | | set bit 0 | ordinary programs are killed by illegal-instruction faults on legal instructions |
| `CSR_MVMD1` (0xBF9) | | set bit 1, 4 or 5, or bits 6-10; set bits 32-36; clear bit 37 | no effect seen |
| `CSR_MSETUP` (0x7C0) | 0x10073 | clear bit 4 (0x10063) | no lock-up and no events seen; about 20 % slower in the test loop |
| `CSR_MSETUP` (0x7C0) | | clear bit 5, 6 or 16 | no effect seen |
| `CSR_MHCR` (0x7C1) | | clear bit 0; set bit 32; clear bit 33 | no effect seen |

Bit 37 of `CSR_MVMD1` is the documented one ("disable ifu fusion type5"); the firmware sets it, and it makes no difference
here either way. So setting bit 3 of `CSR_MVMD1` is a strong mitigation but not a cure, and only the vendor can say what it
switches.

## Earlier versions

- The first reproducer, a replay of all 37,709 recorded instructions (15 MB), is on the branch `v1-37k-instruction-replay`.
- The second, 298 instructions per pass with RVA23 instructions, page copies between passes and reader processes on
  neighbouring harts, is commit `585c8d1` of this branch. Its README documents what that version needed (the page copies
  and the readers turned out to be one way of making F1's stack slow to reach; the flush replaces both). That version was
  confirmed on a K3 CoM260 and a K3 Pico-ITX, under Fedora and Bianbu.
