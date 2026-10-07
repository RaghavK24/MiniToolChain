; hello.asm: MiniToolchain "Hello, World!" example
; Demonstrates: syscalls, string data, basic control flow

.section .data

message:
    .asciz "Hello, World!\n"
msg_len:
    .qword 14

.section .text

.global _start

_start:
    ; syscall write(fd=1, buf=message, len=14)
    MOVI R1, 1              ; fd = stdout
    LEA  R2, message        ; buffer address (relocated)
    MOVI R3, 14             ; length
    SYSCALL 1               ; write

    ; syscall exit(0)
    MOVI R1, 0              ; exit code
    SYSCALL 0               ; exit
