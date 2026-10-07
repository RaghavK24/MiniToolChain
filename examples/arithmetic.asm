; arithmetic.asm: basic arithmetic operations
; Demonstrates: MOVI, ADD, SUB, MUL, DIV, MOD, INC, DEC, NEG

.section .text

.global _start

_start:
    ; Addition: 10 + 20 = 30
    MOVI R1, 10
    MOVI R2, 20
    ADD  R1, R2         ; R1 = 30

    ; Subtraction: 30 - 5 = 25
    MOVI R3, 5
    SUB  R1, R3         ; R1 = 25

    ; Multiplication: 25 * 4 = 100
    MOVI R4, 4
    MUL  R1, R4         ; R1 = 100

    ; Division: 100 / 7 = 14
    MOVI R5, 7
    MOV  R6, R1         ; save 100
    DIV  R1, R5         ; R1 = 14 (quotient)

    ; Modulo: 100 % 7 = 2
    MOV  R1, R6         ; restore 100
    MOD  R1, R5         ; R1 = 2 (remainder)

    ; Increment and Decrement
    MOVI R7, 42
    INC  R7             ; R7 = 43
    DEC  R7             ; R7 = 42

    ; Negate
    NEG  R7             ; R7 = -42

    ; Store result and exit
    MOV  R14, R7        ; return value = -42
    HALT
