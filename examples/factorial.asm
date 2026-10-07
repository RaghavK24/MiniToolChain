; factorial.asm: iterative factorial computation
; Demonstrates: loops, comparison, conditional branching

.section .text

.global _start

_start:
    MOVI R1, 10         ; compute factorial(10)
    CALL factorial
    MOV  R14, R1        ; result in R14
    HALT

; factorial(n): compute n! iteratively
; Input: R1 = n
; Output: R1 = n!
factorial:
    PUSH R13            ; save frame pointer
    MOV  R13, R15       ; not used here but good practice
    MOVI R2, 1          ; accumulator = 1

factorial_loop:
    MOVI R3, 1
    CMP  R1, R3         ; if n <= 1, done
    JLE  factorial_done
    MUL  R2, R1         ; accumulator *= n
    DEC  R1             ; n--
    JMP  factorial_loop

factorial_done:
    MOV  R1, R2         ; result = accumulator
    POP  R13            ; restore frame pointer
    RET
