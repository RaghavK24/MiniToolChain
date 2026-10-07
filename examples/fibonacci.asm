; fibonacci.asm: Fibonacci sequence
; Demonstrates: recursion, stack management, CALL/RET

.section .text

.global _start

_start:
    MOVI R1, 10         ; compute fib(10) = 55
    CALL fibonacci
    MOV  R14, R1        ; result in R14
    HALT

; fibonacci(n): recursive Fibonacci
; Input: R1 = n
; Output: R1 = fib(n)
fibonacci:
    PUSH R13            ; save frame pointer
    MOV  R13, R15

    ; base cases: fib(0) = 0, fib(1) = 1
    MOVI R2, 1
    CMP  R1, R2
    JLE  fib_base

    ; recursive case: fib(n) = fib(n-1) + fib(n-2)
    PUSH R1             ; save n
    DEC  R1             ; n-1
    CALL fibonacci      ; R1 = fib(n-1)
    MOV  R9, R1         ; R9 = fib(n-1) (caller-saved temp)
    POP  R1             ; restore n
    MOVI R2, 2
    SUB  R1, R2         ; n-2
    CALL fibonacci      ; R1 = fib(n-2)
    ADD  R1, R9         ; R1 = fib(n-1) + fib(n-2)

    POP  R13            ; restore frame pointer
    RET

fib_base:
    ; R1 already holds 0 or 1
    POP  R13            ; restore frame pointer
    RET
