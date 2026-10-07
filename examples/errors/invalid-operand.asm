; Operands that do not match the instruction's shape.
;
; Expected: three separate errors, one per line. Sema does not stop at the
; first problem it finds.

.section .text
.global _start

_start:
    MOV  R1, 5          ; MOV is register-to-register; use MOVI for a constant
    ADD  R1            ; ADD takes two operands
    LOAD R1, R2         ; LOAD needs a [base + displacement] memory operand
    HALT
