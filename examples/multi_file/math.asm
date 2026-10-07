; math.asm: arithmetic helpers, compiled separately from main.asm.

.section .text

.global sum_to

; sum_to(n): the sum of 1..n.
;   input:  R1 = n
;   output: R14 = the sum
; R1 is caller-saved, so this is free to consume it.
sum_to:
    MOVI R14, 0
    MOVI R3, 0

.Lloop:                     ; a local label: invisible outside this file
    CMP  R1, R3
    JLE  .Ldone
    ADD  R14, R1
    DEC  R1
    JMP  .Lloop

.Ldone:
    RET
