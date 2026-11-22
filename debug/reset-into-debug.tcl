# OpenOCD procedures to get a locked X100 hart into debug mode by resetting it, and to read what survives.
# Not part of the reproducer; nothing here runs on the target.  See "Unlocking a locked hart with a debugger" in README.md.
#
# Load into a running OpenOCD that is attached to the K3's X100 debug port:   source debug/reset-into-debug.tcl
# T (set below) is the name every command is sent through.  OpenOCD calls hart N k3.x100.N and commands can only be sent
# through a hart it could attach to, so T must be a hart that is NOT locked (several can lock in one run): k3.x100.0, or any
# other healthy one, e.g.  set T k3.x100.4
# T does not select the hart that is acted on; the argument of each procedure does.
#
#   hst 5          show hart 5's debug status
#   hhalt 5 500    ask hart 5 to halt for 500 ms; a locked hart answers "DID NOT HALT"
#   hrescue 5      arm halt-on-reset for hart 5, pulse its power-on reset, show the status (expect havereset + halted)
#   hregs 5        read its integer registers and a few CSRs through abstract commands
#
# The K3 has two debug modules: harts 0-3 at DMI base 0, harts 4-7 at DMI base 0x400; inside each the hart index is 0-3.
# OpenOCD's `riscv dmi_read/dmi_write` take the raw DMI address, so the base has to be added by hand (dmb).

set T k3.x100.0        ;# any hart that OpenOCD examined successfully; use k3.x100.4 if hart 0 is the locked one
poll off

proc dmb {n} { return [expr {$n < 4 ? 0 : 0x400}] }
proc dmr {n reg} { global T; return [$T riscv dmi_read [expr {[dmb $n] + $reg}]] }
proc dmw {n reg val} { global T; $T riscv dmi_write [expr {[dmb $n] + $reg}] $val }
# dmcontrol for hart n: hartsel | extra bits | dmactive.   0x80000000 = haltreq, 0x40000000 = resumereq, 0x8 = setresethaltreq
proc hctl {n bits} { dmw $n 0x10 [expr {(($n & 3) << 16) | $bits | 1}] }
proc hbits {v} {
    set out {}
    foreach {bit name} {19 allhavereset 17 allresumeack 13 allunavail 11 allrunning 9 allhalted} { if {($v >> $bit) & 1} { lappend out $name } }
    return [join $out ,]
}
proc hst {n} { hctl $n 0; set v [dmr $n 0x11]; return [format "hart %d dmstatus 0x%08x %s" $n $v [hbits $v]] }
proc hhalt {n ms} { hctl $n 0x80000000; after $ms; set v [dmr $n 0x11]; hctl $n 0; return [expr {($v & 0x300) ? "halted" : "DID NOT HALT"}] }

# Per-core power-on reset: register APMU_PMU_CC2_AP at physical 0xd4282900, one bit per X100 core, 1 = reset asserted
# (bit numbers from Linux, drivers/reset/spacemit/reset-spacemit-k3.c: RESET_APMU_CPUn_POP).
# Written through the debug module's system-bus access, so no working hart is needed.
set POPBIT {0 3 6 9 16 19 22 25}
proc hpop {n ms} {
    global T POPBIT
    $T riscv set_mem_access sysbus
    $T mww phys 0xd4282900 [expr {1 << [lindex $POPBIT $n]}]
    after $ms
    $T mww phys 0xd4282900 0
    after 300
}
proc hrescue {n} {
    echo "before:               [hst $n]"
    echo "halt request, 500 ms: [hhalt $n 500]"
    hctl $n 0x8
    hpop $n 50
    hctl $n 0x8
    set v [dmr $n 0x11]
    echo [format "after the reset pulse: hart %d dmstatus 0x%08x %s" $n $v [hbits $v]]
}

# one register through an abstract command (regno: 0x1000 + n for x0-x31, the CSR number for a CSR)
proc hreg {n regno} {
    hctl $n 0; dmw $n 0x16 0x700; dmw $n 0x17 [expr {0x00320000 | $regno}]; after 5
    set e [expr {([dmr $n 0x16] >> 8) & 7}]
    if {$e} { dmw $n 0x16 0x700; return "cmderr=$e" }
    return [format "0x%08x%08x" [dmr $n 0x05] [dmr $n 0x04]]
}
proc hregs {n} {
    set names {zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6}; set line ""
    for {set i 0} {$i < 32} {incr i} {
        append line [format "%s=%s " [lindex $names $i] [hreg $n [expr {0x1000 + $i}]]]
        if {$i % 4 == 3} { echo "  $line"; set line "" }
    }
    foreach {nm no} {dpc 0x7b1 mstatus 0x300 mepc 0x341 mcause 0x342 mtval 0x343 sepc 0x141 scause 0x142 stval 0x143 satp 0x180 mcycle 0xb00 minstret 0xb02} {
        echo [format "  %-9s %s" $nm [hreg $n $no]]
    }
}
