; loops.asm: loop constructs
; Demonstrates: CMP, conditional branches, loop patterns

.section .text

.global _start

_start:
    ; Sum numbers 1 to 100
    MOVI R1, 0          ; sum = 0
    MOVI R2, 1          ; i = 1
    MOVI R3, 100        ; limit = 100

sum_loop:
    ADD  R1, R2         ; sum += i
    INC  R2             ; i++
    CMP  R2, R3         ; compare i with limit
    JLE  sum_loop       ; if i <= 100, continue

    ; R1 now holds 5050
    MOV  R14, R1        ; return value
    HALT
