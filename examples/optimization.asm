; optimization.asm: material for the optimizer to work on.
;
; Build it both ways and compare:
;
;   minitool build examples/optimization.asm -o o0.mexe -O0 --stats
;   minitool build examples/optimization.asm -o o1.mexe -O1 --stats
;   minitool run o0.mexe --stats
;   minitool run o1.mexe --stats
;
; Both must print the same thing and leave R14 = 42. Only the instruction count
; differs. That is the whole standard the optimizer is held to.

.section .text

.global _start

_start:
    ; Constant folding: three instructions that compute one known value.
    MOVI R1, 40
    MOVI R2, 2
    ADD  R1, R2                 ; -> MOVI R1, 42, because the flags die below

    ; Dead store: R3 is overwritten before anyone reads it.
    MOVI R3, 111
    MOVI R3, 222

    ; Identities: neither of these does anything at all.
    MOV  R4, R4
    NOP

    ; A round trip through the stack that cancels out.
    PUSH R1
    POP  R1

    ; A jump to the very next instruction.
    JMP  carry_on

carry_on:
    MOV  R14, R1
    HALT

    ; Unreachable: nothing can branch here, so nothing needs to be emitted.
    MOVI R5, 1
    MOVI R6, 2
    ADD  R5, R6
