; hello.asm - the classic: print a string with DOS service 09h
org 100h

    mov dx, message     ; DS:DX points to the text
    mov ah, 09h         ; "print '$'-terminated string" service
    int 21h
    ret                 ; in a .COM file, RET returns to DOS

message db 'Hello from the 8086!', 13, 10, '$'
