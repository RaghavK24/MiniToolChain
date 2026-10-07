; main.asm: the program entry point.
;
; This file knows nothing about how `sum_to` or `message` are implemented; it
; only declares that they exist. The linker is what connects them, by resolving
; the relocations the assembler leaves behind.
;
;   minitool assemble examples/multi_file/main.asm -o main.mobj
;   minitool assemble examples/multi_file/math.asm -o math.mobj
;   minitool assemble examples/multi_file/data.asm -o data.mobj
;   minitool link main.mobj math.mobj data.mobj -o program.mexe
;   minitool run program.mexe

.section .text

.global _start
.extern sum_to
.extern message
.extern message_len

_start:
    MOVI R1, 10
    CALL sum_to             ; defined in math.asm
    MOV  R5, R14            ; keep the result: 10+9+...+1 = 55

    ; write(fd = 1, buffer = message, length = message_len)
    LEA  R6, message_len    ; the address of the length, from data.asm
    LOAD R3, [R6 + 0]       ; ...and the length itself
    MOVI R1, 1
    LEA  R2, message
    SYSCALL 1

    ; exit(0)
    MOVI R1, 0
    SYSCALL 0
