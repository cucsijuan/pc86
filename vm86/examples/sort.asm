; sort.asm - bubble sort a table of signed words
org 100h

COUNT equ 10

    mov cx, COUNT - 1   ; passes
outer:
    push cx
    mov si, table
.compare:
    mov ax, [si]
    cmp ax, [si+2]
    jle .next           ; already in order (signed)
    xchg ax, [si+2]     ; swap the two elements
    mov [si], ax
.next:
    add si, 2
    loop .compare
    pop cx
    loop outer

    ; print the sorted table
    mov si, table
    mov cx, COUNT
show:
    lodsw
    call print_signed
    mov dl, ' '
    mov ah, 02h
    int 21h
    loop show
    mov dx, crlf
    mov ah, 09h
    int 21h
    ret

; print_signed: print AX as a signed decimal number
print_signed:
    push ax
    push bx
    push cx
    push dx
    test ax, ax
    jns .positive
    push ax
    mov dl, '-'
    mov ah, 02h
    int 21h
    pop ax
    neg ax
.positive:
    mov bx, 10
    xor cx, cx
.divide:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .divide
.print:
    pop dx
    add dl, '0'
    mov ah, 02h
    int 21h
    loop .print
    pop dx
    pop cx
    pop bx
    pop ax
    ret

table dw 42, -7, 1000, 0, 13, -300, 8, 77, 5, -1
crlf  db 13, 10, '$'
