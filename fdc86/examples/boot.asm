; boot.asm - a boot sector that lists the files on its diskette
;
; The BIOS reads sector 1 of drive A: to 0000:7C00 and jumps to it with DL =
; the drive. The first bytes jump over the BPB, the table that describes the
; diskette (bytes per sector, FATs, root directory size, geometry); when the
; sector is installed (workbench: Run; fdimage boot a.img boot.bin) the
; diskette's own BPB replaces the zeros below.
;
; The root directory comes right after the FATs. Its first sector number
; (LBA) becomes cylinder, head and sector for INT 13h with the BPB's geometry.
org 7C00h

    jmp short start
    nop
    times 59 db 0                   ; the BPB

RESERVED          equ 7C0Eh         ; BPB fields (word, except FATS)
FATS              equ 7C10h
SECTORS_PER_FAT   equ 7C16h
SECTORS_PER_TRACK equ 7C18h
HEADS             equ 7C1Ah
BUFFER            equ 0500h         ; free memory above the BIOS data area

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 7C00h
    sti
    mov [drive], dl
    mov si, title
    call print

    ; LBA of the root directory: reserved sectors + FATs * sectors per FAT
    mov al, [FATS]
    xor ah, ah
    mul word [SECTORS_PER_FAT]
    add ax, [RESERVED]
    ; LBA -> sector = LBA % spt + 1, head = (LBA / spt) % heads, cylinder = LBA / spt / heads
    xor dx, dx
    div word [SECTORS_PER_TRACK]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [HEADS]
    mov ch, al
    mov dh, dl
    mov dl, [drive]
    mov bx, BUFFER
    mov di, 3                       ; tries
read:
    mov ax, 0201h
    int 13h
    jnc listing
    xor ax, ax                      ; reset and try again
    int 13h
    dec di
    jnz read
    mov si, failed
    call print
    jmp halt

listing:
    mov si, BUFFER
entry:
    mov al, [si]
    or al, al
    jz done                         ; 00h: end of the directory
    cmp al, 0E5h
    je skip                         ; E5h: deleted
    test byte [si+11], 08h
    jnz skip                        ; the volume label
    push si
    mov al, ' '
    call putc
    mov al, ' '
    call putc
    mov cx, 8                       ; NAME
name_char:
    lodsb
    call putc
    loop name_char
    mov al, ' '
    call putc
    mov cx, 3                       ; EXT
ext_char:
    lodsb
    call putc
    loop ext_char
    mov al, 13
    call putc
    mov al, 10
    call putc
    pop si
skip:
    add si, 32
    cmp si, BUFFER + 512            ; the first 16 entries
    jb entry

done:
    mov si, bye
    call print
halt:
    hlt
    jmp halt

; prints the zero-terminated string at DS:SI
print:
    lodsb
    or al, al
    jz .end
    call putc
    jmp print
.end:
    ret

; prints AL with the BIOS teletype
putc:
    push ax
    push bx
    mov ah, 0Eh
    mov bx, 0007h
    int 10h
    pop bx
    pop ax
    ret

title  db "Booted from the diskette (0000:7C00).", 13, 10, "Files:", 13, 10, 0
failed db "Cannot read the root directory.", 13, 10, 0
bye    db "Done.", 13, 10, 0
drive  db 0
