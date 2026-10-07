; functions.asm: function calls and calling convention
; Demonstrates: CALL/RET, argument passing, return values

.section .text

.global _start

_start:
    ; Call add(15, 27) -> result in R14
    MOVI R1, 15         ; arg1
    MOVI R2, 27         ; arg2
    CALL add
    ; R14 now holds 42

    ; Call multiply(R14, 3) -> result in R14
    MOV  R1, R14        ; arg1 = 42
    MOVI R2, 3          ; arg2 = 3
    CALL multiply
    ; R14 now holds 126

    HALT

; add(a, b) -> a + b
; R1 = a, R2 = b, R14 = result
add:
    MOV  R14, R1
    ADD  R14, R2
    RET

; multiply(a, b) -> a * b
; R1 = a, R2 = b, R14 = result
multiply:
    MOV  R14, R1
    MUL  R14, R2
    RET
