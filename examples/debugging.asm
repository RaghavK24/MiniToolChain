; debugging.asm: a program worth stepping through.
;
;   minitool build examples/debugging.asm -o debug.mexe
;   minitool debug debug.mexe
;
; Then, at the (minidbg) prompt:
;
;   break accumulate     set a breakpoint on the function
;   run                  stop at it
;   registers            R1 holds the argument
;   backtrace            see who called it
;   watch total          stop when the total changes
;   continue             run until it does
;   next                 step over a call
;   list                 which source line are we on
;   quit

.section .data

total:
    .qword 0

.section .text

.global _start

_start:
    MOVI R1, 5
    CALL accumulate
    LEA  R2, total
    LOAD R3, [R2 + 0]       ; R3 = 15
    MOV  R14, R3
    HALT

; accumulate(n): adds 1..n into `total`.
;   input: R1 = n
accumulate:
    PUSH R13                ; save the frame pointer
    MOV  R13, R1            ; keep the counter in a callee-saved register
    LEA  R4, total
    MOVI R5, 0              ; the running sum

.Lloop:
    CMP  R13, R0
    JLE  .Ldone
    ADD  R5, R13
    DEC  R13
    JMP  .Lloop

.Ldone:
    STORE [R4 + 0], R5      ; publish the result
    POP  R13
    RET
