; text_demo.asm - text mode 03h: CP437 box drawing, attributes, blink, cursor
org 100h

    mov ax, 0003h           ; set text mode 80x25 (clears the screen)
    int 10h

    ; --- double-line box drawn directly into video memory (B800:0000) ---
    mov ax, 0B800h
    mov es, ax
    mov di, (2*80 + 10)*2   ; row 2, column 10
    mov ah, 0Fh             ; bright white on black
    mov al, 0C9h            ; top-left corner
    stosw
    mov al, 0CDh            ; horizontal line
    mov cx, 58
    rep stosw
    mov al, 0BBh            ; top-right corner
    stosw

    mov bx, 3               ; rows 3..12: sides
sides:
    mov ax, bx
    mov dl, 80
    mul dl
    add ax, 10
    shl ax, 1
    mov di, ax
    mov ax, 0FBAh           ; left side
    stosw
    add di, 58*2
    stosw                   ; right side
    inc bx
    cmp bx, 13
    jb sides

    mov di, (13*80 + 10)*2
    mov ax, 0FC8h           ; bottom-left corner
    stosw
    mov al, 0CDh
    mov cx, 58
    rep stosw
    mov al, 0BCh            ; bottom-right corner
    stosw

    ; --- lines of text with different attributes (BIOS write string) ---
    mov si, lines
next_line:
    lodsb                   ; attribute
    test al, al
    jz lines_done
    mov bl, al
    lodsb                   ; row
    mov dh, al
    mov dl, 13              ; column
    lodsb                   ; length
    xor ch, ch
    mov cl, al
    mov bp, si              ; ES:BP = string
    push ds
    pop es
    mov ax, 1300h           ; write string, attribute in BL, cursor not moved
    xor bh, bh
    int 10h
    add si, cx
    jmp next_line
lines_done:

    ; --- shaded bar using the CP437 shade characters ---
    mov ax, 0B800h
    mov es, ax
    mov di, (11*80 + 13)*2
    mov ah, 07h
    mov al, 0B0h
    mov cx, 13
    rep stosw
    mov al, 0B1h
    mov cx, 13
    rep stosw
    mov al, 0B2h
    mov cx, 13
    rep stosw
    mov al, 0DBh
    mov cx, 13
    rep stosw

    ; --- prompt with the blinking hardware cursor after it ---
    mov dx, 1600h           ; row 22, column 0
    mov ah, 02h
    xor bh, bh
    int 10h
    mov si, prompt
print_prompt:
    lodsb
    test al, al
    jz forever
    mov ah, 0Eh
    int 10h
    jmp print_prompt

forever:
    jmp forever             ; keep running so the cursor and blink can be seen

; attribute, row, length, text
lines:
    db 0Fh, 3, line1_len
line1: db 'vga86 - monochrome VGA text mode 03h'
line1_len equ $ - line1
    db 07h, 5, line2_len
line2: db 'Normal (07h)   ', 0FAh, '  CP437: caf', 82h, ' na', 8Bh, 've fa', 87h, 'ade se', 0A4h, 'or'
line2_len equ $ - line2
    db 0Fh, 6, line3_len
line3: db 'Bright (0Fh)   ', 0FAh, '  ', 01h, ' ', 02h, ' ', 03h, ' ', 04h, ' ', 05h, ' ', 06h, ' ', 0Eh, ' ', 0Fh, ' ', 18h, ' ', 19h, ' ', 1Ah, ' ', 1Bh
line3_len equ $ - line3
    db 08h, 7, line4_len
line4: db 'Dim (08h)      ', 0FAh, '  dark gray foreground'
line4_len equ $ - line4
    db 70h, 8, line5_len
line5: db 'Inverse (70h)  ', 0FAh, '  black on light gray '
line5_len equ $ - line5
    db 8Fh, 9, line6_len
line6: db 'Blinking (8Fh) ', 0FAh, '  this line blinks'
line6_len equ $ - line6
    db 0                    ; end of table

prompt db 'C:\>', 0
