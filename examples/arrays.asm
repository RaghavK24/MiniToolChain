; arrays.asm: array operations using LOAD/STORE
; Demonstrates: memory access, data section, address calculation

.section .data

array:
    .qword 5
    .qword 3
    .qword 8
    .qword 1
    .qword 9
    .qword 2
    .qword 7
    .qword 4
array_len:
    .qword 8

.section .text

.global _start

_start:
    ; Find the maximum value in the array
    LEA  R1, array          ; R1 = base address of array
    MOVI R2, 8              ; R2 = array length
    CALL find_max
    ; R14 holds the maximum value (9)
    HALT

; find_max(base, length) -> max value
; R1 = array base address, R2 = length
; R14 = result
find_max:
    PUSH R13
    MOV  R13, R15

    LOAD R14, [R1 + 0]      ; max = array[0]
    MOVI R3, 1              ; i = 1

find_max_loop:
    CMP  R3, R2             ; if i >= length, done
    JGE  find_max_done

    ; compute offset = i * 8
    MOV  R4, R3
    MOVI R5, 8
    MUL  R4, R5             ; R4 = i * 8

    ; load array[i]
    MOV  R6, R1
    ADD  R6, R4             ; R6 = base + i*8
    LOAD R7, [R6 + 0]       ; R7 = array[i]

    ; if array[i] > max, update max
    CMP  R7, R14
    JLE  find_max_next
    MOV  R14, R7            ; max = array[i]

find_max_next:
    INC  R3                 ; i++
    JMP  find_max_loop

find_max_done:
    POP  R13
    RET
