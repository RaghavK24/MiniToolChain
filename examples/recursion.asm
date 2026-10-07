; recursion.asm: recursive power function
; Demonstrates: deep recursion, stack usage

.section .text

.global _start

_start:
    ; Compute 2^16 = 65536
    MOVI R1, 2          ; base
    MOVI R2, 16         ; exponent
    CALL power
    MOV  R14, R1        ; result = 65536
    HALT

; power(base, exp) -> base^exp
; R1 = base, R2 = exponent
; Returns result in R1
power:
    PUSH R13
    MOV  R13, R15

    ; base case: exp == 0 -> return 1
    MOVI R3, 0
    CMP  R2, R3
    JNE  power_recurse
    MOVI R1, 1
    POP  R13
    RET

power_recurse:
    ; recursive case: base * power(base, exp-1)
    PUSH R1             ; save base
    DEC  R2             ; exp - 1
    CALL power          ; R1 = power(base, exp-1)
    POP  R9             ; restore base into R9
    MUL  R1, R9         ; R1 = base * power(base, exp-1)
    POP  R13
    RET
