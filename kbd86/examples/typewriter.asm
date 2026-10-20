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
    je .special
    test al, al             ; function keys, arrows: ASCII 0
    jz .special
    cmp al, 8               ; Backspace
    je .backspace
    cmp al, 13              ; Enter: new line
    jne .print
    mov ax, 0E0Dh
    int 10h
    mov al, 10
.print:
    mov ah, 0Eh
    int 10h
    jmp next_key

; The BIOS only moves the cursor for a backspace (as on a real PC), so the
; program erases the character itself, and at the start of a line goes back
; to the previous one and erases its last character.
.backspace:
    call get_cursor
    test dl, dl
    jz .line_up
    dec dl
    call set_cursor
    call erase
    jmp next_key
.line_up:
    cmp dh, 2               ; first typing row
    jbe next_key
    dec dh
    call erase_last
    jmp next_key

; Arrows move the cursor around the typing area (rows 2-24).
.special:
    call get_cursor
    cmp ah, 48h             ; up
    je .up
    cmp ah, 50h             ; down
    je .down
    cmp ah, 4Bh             ; left
    je .left
    cmp ah, 4Dh             ; right
    je .right
    jmp next_key
.up:
    cmp dh, 2
    jbe next_key
    dec dh
    jmp .move
.down:
    cmp dh, 24
    jae next_key
    inc dh
    jmp .move
.left:
    test dl, dl
    jz next_key
    dec dl
    jmp .move
.right:
    cmp dl, 79
    jae next_key
    inc dl
.move:
    call set_cursor
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

set_cursor:                 ; DH = row, DL = column
    mov ah, 02h
    xor bh, bh
    int 10h
    ret

get_cursor:                 ; DH = row, DL = column (keeps AX)
    push ax
    push cx
    mov ah, 03h
    xor bh, bh
    int 10h
    pop cx
    pop ax
    ret

erase:                      ; blank at the cursor, which stays there
    mov ax, 0A20h
    xor bh, bh
    mov cx, 1
    int 10h
    ret

; erase_last: erases the last character of row DH and leaves the cursor in
; its place (column 0 if the row is empty)
erase_last:
    mov dl, 79
.scan:
    call set_cursor
    mov ah, 08h             ; read the character at the cursor
    int 10h
    cmp al, ' '
    je .blank
    test al, al
    jne .found
.blank:
    test dl, dl
    jz .done
    dec dl
    jmp .scan
.found:
    call erase
.done:
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
