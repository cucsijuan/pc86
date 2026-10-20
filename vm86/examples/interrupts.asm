; interrupts.asm - install our own handlers in the interrupt vector table
org 100h

    ; the vector table is at 0000:0000, 4 bytes per interrupt (IP, CS)
    xor ax, ax
    mov es, ax
    cli
    mov word [es:60h*4], my_service
    mov [es:60h*4+2], cs
    mov word [es:0*4], divide_error
    mov [es:0*4+2], cs
    sti

    ; call our INT 60h three times
    mov cx, 3
.call:
    int 60h
    loop .call

    ; divide by zero: jumps to divide_error
    mov dx, msg_before
    mov ah, 09h
    int 21h
    xor dx, dx
    mov ax, 1234
    xor bx, bx
    div bx
    mov dx, msg_unreached       ; never executed
    mov ah, 09h
    int 21h
    ret

my_service:                     ; INT 60h: print a message
    push ax
    push dx
    push ds
    push cs
    pop ds
    mov dx, msg_int60
    mov ah, 09h
    int 21h
    pop ds
    pop dx
    pop ax
    iret

divide_error:                   ; INT 0: division by zero
    push cs
    pop ds
    mov dx, msg_div0
    mov ah, 09h
    int 21h
    mov ax, 4C01h               ; terminate the program with exit code 1
    int 21h

msg_int60     db 'INT 60h handled by the program', 13, 10, '$'
msg_before    db 'Dividing by zero...', 13, 10, '$'
msg_div0      db 'Caught the divide error exception (INT 0)', 13, 10, '$'
msg_unreached db 'this should not be printed', 13, 10, '$'
