; piano.asm - plays notes on the PC speaker.
; Keys A S D F G H J K play C4 to C5: the timer's channel 2 generates the
; tone, port 61h connects it to the speaker, and an INT 1Ch handler (18.2 Hz)
; silences it a few ticks later. Esc quits.
;
;   asm86 piano.asm -o piano.com && pcrun piano.com

org 100h

    mov ax, 0003h               ; 80x25 text, clears the screen
    int 10h
    mov ah, 01h                 ; hide the cursor
    mov cx, 2000h
    int 10h

    mov dx, 0A17h               ; row 10, column 23
    mov si, title
    call print
    mov dx, 0C1Ch               ; row 12, column 28
    mov si, keys
    call print

    xor ax, ax                  ; install the tick handler, keeping the old one
    mov es, ax
    cli
    mov ax, [es:1Ch*4]
    mov [old_tick], ax
    mov ax, [es:1Ch*4+2]
    mov [old_tick+2], ax
    mov word [es:1Ch*4], tick
    mov [es:1Ch*4+2], cs
    sti

main_loop:
    hlt                         ; sleep until the next interrupt
    mov ah, 01h                 ; key waiting?
    int 16h
    jz main_loop
    xor ah, ah
    int 16h
    cmp al, 27
    je quit
    or al, 20h                  ; lower case
    mov si, letters
    xor bx, bx
find:
    cmp byte [si+bx], 0
    je main_loop                ; not a piano key
    cmp al, [si+bx]
    je play
    inc bx
    jmp find

play:
    mov al, 0B6h                ; channel 2, low then high byte, mode 3
    out 43h, al
    shl bx, 1
    mov ax, [divisors+bx]       ; 1193182 / frequency
    out 42h, al
    mov al, ah
    out 42h, al
    in al, 61h                  ; gate and speaker on
    or al, 03h
    out 61h, al
    mov word [left], 4          ; about 220 ms

    mov dx, 0E24h               ; row 14, column 36: the note's name
    mov si, names
    shl bx, 1                   ; 4 bytes per name
    add si, bx
    call print
    jmp main_loop

quit:
    call silence
    xor ax, ax                  ; give INT 1Ch back
    mov es, ax
    cli
    mov ax, [old_tick]
    mov [es:1Ch*4], ax
    mov ax, [old_tick+2]
    mov [es:1Ch*4+2], ax
    sti
    int 20h

tick:
    cmp word [cs:left], 0
    je .done
    dec word [cs:left]
    jnz .done
    call silence
.done:
    iret

silence:
    push ax
    in al, 61h
    and al, 0FCh
    out 61h, al
    pop ax
    ret

; prints the zero-terminated string at SI at row DH, column DL
print:
    mov ah, 02h
    xor bh, bh
    int 10h
.next:
    lodsb
    or al, al
    jz .done
    mov ah, 0Eh
    mov bl, 07h
    int 10h
    jmp .next
.done:
    ret

title    db "Piano - timer channel 2 and speaker", 0
keys     db "A S D F G H J K    Esc quits", 0
letters  db "asdfghjk", 0
divisors dw 4561, 4063, 3620, 3417, 3044, 2712, 2416, 2280
names    db "C4 ", 0, "D4 ", 0, "E4 ", 0, "F4 ", 0, "G4 ", 0, "A4 ", 0, "B4 ", 0, "C5 ", 0
left     dw 0
old_tick dw 0, 0
