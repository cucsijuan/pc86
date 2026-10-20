; typewriter.asm - reads keys through the BIOS (INT 16h) and echoes them.
; The top line shows the scan code and ASCII code of the last key. Esc exits.
org 100h

    mov ax, 0003h
    int 10h
    mov dx, 0200h           ; start typing at row 2
    call set_cursor

next_key:
    mov ah, 10h             ; wait for a key (enhanced keyboard function)
    int 16h
    mov [last], ax
    call show_codes
    cmp al, 27              ; Esc
    je done
    cmp al, 0E0h            ; gray keys: ASCII E0h, nothing to print
    je next_key
    test al, al             ; function keys, arrows: ASCII 0
    jz next_key
    cmp al, 13              ; Enter: new line
    jne .print
    mov ax, 0E0Dh
    int 10h
    mov al, 10
.print:
    mov ah, 0Eh
    int 10h
    jmp next_key

done:
    mov dx, 1700h
    call set_cursor
    ret

; show_codes: writes "scan XXh  ascii XXh" on row 0, keeping the cursor
show_codes:
    push ax
    mov ah, 03h             ; save cursor
    xor bh, bh
    int 10h
    push dx
    mov dx, 0000h
    call set_cursor
    mov si, label_scan
    call print
    mov al, [last+1]
    call print_hex
    mov si, label_ascii
    call print
    mov al, [last]
    call print_hex
    mov si, label_h
    call print
    pop dx
    call set_cursor
    pop ax
    ret

set_cursor:
    mov ah, 02h
    xor bh, bh
    int 10h
    ret

print:                      ; prints the 0-terminated string at SI
    lodsb
    test al, al
    jz .end
    mov ah, 0Eh
    int 10h
    jmp print
.end:
    ret

print_hex:                  ; prints AL as two hex digits
    push ax
    shr al, 1
    shr al, 1
    shr al, 1
    shr al, 1
    call .digit
    pop ax
    and al, 0Fh
.digit:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    mov ah, 0Eh
    int 10h
    ret

last        dw 0
label_scan  db 'scan ', 0
label_ascii db 'h  ascii ', 0
label_h     db 'h  (Esc exits)', 0
