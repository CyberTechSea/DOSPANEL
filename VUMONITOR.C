/*
 * VUMONITOR.C  v3.0 — TSR per DOS
 * Invia dati estesi di sistema via COM1 al pannello JC3248W535
 *
 * Compilare con Open Watcom C:
 *   wcl -ms -os VUMONITOR.C
 *
 * Utilizzo:
 *   VUMONITOR        installa il TSR
 *   VUMONITOR /U     disinstalla
 *   VUMONITOR /Q     stato
 *
 * Formato pacchetto v3:
 *   $RAM:512;14:32:07;09/04/2026;DOS:7.1;DRV:0;
 *    CPU:486;BIOS:AMI-19920101;
 *    DSK:C:204800,D:102400;VOL:MSDOS_6;
 *    KBD:010;MOUSE:1,160,120;COM:9600;FILES:15\n
 */

#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <conio.h>

/* -------------------------------------------------------
   COSTANTI
   ------------------------------------------------------- */
#define COM_PORT     0x3F8
#define BAUD_RATE    9600
#define TICK_RATE    18
#define TSR_ID       0xABCD
#define TSR_INT      0x60

/* -------------------------------------------------------
   VARIABILI RESIDENTI
   ------------------------------------------------------- */
static unsigned int  tick_count = 0;
static unsigned int  tsr_id     = TSR_ID;
static void (__interrupt __far *old_int8)(void) = 0;

/* -------------------------------------------------------
   SERIALE
   ------------------------------------------------------- */
static void serial_putc(unsigned char c) {
    while (!(inp(COM_PORT + 5) & 0x20));
    outp(COM_PORT, c);
}
static void serial_puts(const char far *s) {
    while (*s) serial_putc(*s++);
}
static void uint_to_str(unsigned int n, char *buf) {
    char tmp[6]; int i=0,j=0;
    if(n==0){buf[j++]='0';buf[j]=0;return;}
    while(n>0){tmp[i++]='0'+(n%10);n/=10;}
    while(i>0) buf[j++]=tmp[--i];
    buf[j]=0;
}
static void uint_to_str2(unsigned int n, char *buf) {
    buf[0]='0'+(n/10); buf[1]='0'+(n%10); buf[2]=0;
}
static void uint_to_str4(unsigned int n, char *buf) {
    buf[0]='0'+(n/1000); buf[1]='0'+((n/100)%10);
    buf[2]='0'+((n/10)%10); buf[3]='0'+(n%10); buf[4]=0;
}
static void ulong_to_str(unsigned long n, char *buf) {
    char tmp[12]; int i=0,j=0;
    if(n==0){buf[j++]='0';buf[j]=0;return;}
    while(n>0){tmp[i++]='0'+(n%10);n/=10;}
    while(i>0) buf[j++]=tmp[--i];
    buf[j]=0;
}

/* -------------------------------------------------------
   DATI BASE
   ------------------------------------------------------- */
static unsigned int get_free_ram(void) {
    union REGS r; r.h.ah=0x48; r.x.bx=0xFFFF;
    int86(0x21,&r,&r);
    return (r.x.bx*16)/1024;
}
static void get_time(unsigned char *h,unsigned char *m,unsigned char *s){
    union REGS r; r.h.ah=0x2C; int86(0x21,&r,&r);
    *h=r.h.ch; *m=r.h.cl; *s=r.h.dh;
}
static void get_date(unsigned int *y,unsigned char *mo,unsigned char *d){
    union REGS r; r.h.ah=0x2A; int86(0x21,&r,&r);
    *y=r.x.cx; *mo=r.h.dh; *d=r.h.dl;
}
static void get_dos_version(unsigned char *maj,unsigned char *min){
    union REGS r; r.h.ah=0x30; int86(0x21,&r,&r);
    *maj=r.h.al; *min=r.h.ah;
}
static unsigned char get_drive_activity(void){
    return inp(0x3F4)&0xC0;
}

/* -------------------------------------------------------
   TIPO CPU
   Restituisce: 86, 186, 286, 386, 486, 586
   ------------------------------------------------------- */
static unsigned int detect_cpu(void) {
    /* Metodo classico: test dei flag del processore */
    unsigned int cpu = 86;

    /* Test 286+: il 286 azzera i bit 12-15 del flags */
    #pragma aux test286 = \
        "pushf"         \
        "pop ax"        \
        "or ax,0xF000"  \
        "push ax"       \
        "popf"          \
        "pushf"         \
        "pop ax"        \
        "and ax,0xF000" \
        value [ax];

    /* Usiamo un approccio più semplice compatibile Watcom */
    /* Controlliamo se CPUID è disponibile (486+) tentando
       di modificare il flag ID (bit 21) */

    /* Per semplicità: leggiamo dal BIOS equipment word
       e distinguiamo per comportamento dei flag */

    /* Approccio pratico: usa int 11h per info base
       poi distingue 86/286/386/486 con test flag */

    /* Su Watcom C usiamo inline assembly tramite _asm */
    unsigned int flags_result = 0;

    /* Test 1: su 8086/8088 i bit 12-15 sono sempre 1 */
    _asm {
        pushf
        pop  ax
        and  ax, 0x0FFF
        push ax
        popf
        pushf
        pop  ax
        and  ax, 0xF000
        mov  flags_result, ax
    }
    if(flags_result == 0xF000) { return 86; }

    /* Test 2: su 286 i bit 12-15 sono sempre 0 in modo reale */
    _asm {
        pushf
        pop  ax
        or   ax, 0xF000
        push ax
        popf
        pushf
        pop  ax
        and  ax, 0xF000
        mov  flags_result, ax
    }
    if(flags_result == 0x0000) { return 286; }

    /* Test 3: distingue 386 da 486+ con bit AC (bit 18 di EFLAGS) */
    /* Su 386 il bit AC non esiste, su 486+ sì */
    /* Non possiamo usare EFLAGS in modo reale facilmente,
       quindi usiamo una euristica BIOS */
    cpu = 386;

    /* Tentiamo CPUID: se genera #UD siamo su 386,
       altrimenti 486+ — troppo complesso in TSR,
       usiamo il flag AC via int 15h */

    /* Soluzione pratica: leggi la stringa BIOS per identificare */
    /* Per ora restituiamo 386 come default conservativo
       se siamo arrivati fin qui */

    /* Miglioramento: prova a eseguire CPUID con trap */
    /* Su 486 SX/DX il bit 18 (AC) è settabile */
    _asm {
        mov  ax, 0
        mov  flags_result, ax
    }
    /* Semplificazione finale per TSR: se siamo su 386+
       controlliamo il modello dalla tabella BIOS */
    {
        unsigned char far *bios_model = (unsigned char far *)0xFFFF000EL;
        unsigned char model = *bios_model;
        /* FC=AT/386, F8=386, F0=486... valori tipici */
        if(model == 0xF0 || model == 0xF1) cpu = 486;
        else if(model == 0xF8 || model == 0xFC) cpu = 386;
    }

    return cpu;
}

/* -------------------------------------------------------
   BIOS — vendor e data
   La data BIOS è sempre a 0xFFFF5 in formato MM/DD/YY
   Il vendor è più difficile, usiamo una stringa fissa
   dall'area BIOS F000:E000-FFFF
   ------------------------------------------------------- */
static void get_bios_date(char *buf) {
    /* Data BIOS: sempre a F000:FFF5, formato MM/DD/YY */
    unsigned char far *bdate = (unsigned char far *)0xFFFF5L;
    /* Copia 8 caratteri MM/DD/YY */
    int i;
    for(i=0;i<8;i++) buf[i]=bdate[i];
    buf[8]=0;
    /* Converti in YYYYMMDD per compattezza nel pacchetto */
    /* MM/DD/YY → YY+MM+DD */
    /* Lasciamo il formato originale MM/DD/YY */
}

static void get_bios_vendor(char *buf) {
    /* Cerca stringa vendor nell'area F000:E000 */
    unsigned char far *bios = (unsigned char far *)0xFE000L;
    int i,j=0,found=0;
    /* Cerca "BIOS" o "AMI" o "Award" o "Phoenix" */
    const char *tags[]={"AMI","AWARD","PHOENIX","IBM","COMPAQ"};
    int t,tlen;
    for(t=0;t<5;t++){
        tlen=strlen(tags[t]);
        for(i=0;i<0x1F00-tlen;i++){
            int match=1;
            for(j=0;j<tlen;j++){
                unsigned char c=bios[i+j];
                /* Case insensitive approssimato */
                if(c>='a'&&c<='z') c-=32;
                if(c!=tags[t][j]){match=0;break;}
            }
            if(match){
                /* Copia max 8 caratteri del tag trovato */
                for(j=0;j<tlen&&j<8;j++) buf[j]=tags[t][j];
                buf[j]=0;
                return;
            }
        }
    }
    /* Non trovato */
    buf[0]='?'; buf[1]=0;
}

/* -------------------------------------------------------
   DRIVE — lista drive installati e spazio libero
   Restituisce una stringa tipo "C:204800,D:102400"
   (spazio in KB)
   ------------------------------------------------------- */
static void get_drives_info(char *buf) {
    union REGS r;
    unsigned char drive;
    int first=1;
    char tmp[12];
    unsigned long free_kb;
    buf[0]=0;

    for(drive=3; drive<=26; drive++) { /* C=3, D=4... Z=26 */
        /* int 21h AH=36h: get disk free space */
        r.h.ah=0x36;
        r.h.dl=drive;
        int86(0x21,&r,&r);
        if(r.x.ax==0xFFFF) continue; /* drive non valido */

        /* AX=settori/cluster, BX=cluster liberi,
           CX=byte/settore, DX=cluster totali */
        free_kb = (unsigned long)r.x.bx *
                  (unsigned long)r.x.ax *
                  (unsigned long)r.x.cx / 1024UL;

        if(!first) {
            int l=strlen(buf);
            buf[l]=','; buf[l+1]=0;
        }
        /* Aggiungi "C:123456" */
        int l=strlen(buf);
        buf[l]='A'+drive-1; buf[l+1]=':'; buf[l+2]=0;
        ulong_to_str(free_kb, tmp);
        strcat(buf, tmp);
        first=0;

        if(strlen(buf)>40) break; /* protezione overflow */
    }
    if(first) { buf[0]='0'; buf[1]=0; } /* nessun drive */
}

/* -------------------------------------------------------
   VOLUME — nome del drive corrente
   ------------------------------------------------------- */
static void get_volume_label(char *buf) {
    union REGS r;
    struct SREGS sr;
    /* Usa int 21h AH=4Eh (find first) con attributo volume */
    /* Cerca "*.*" con attributo 0x08 (volume label) */
    static char pattern[]="C:\\*.*";
    r.h.ah=0x4E;
    r.x.cx=0x08; /* attributo volume label */
    r.x.dx=FP_OFF(pattern);
    sr.ds=FP_SEG(pattern);
    int86x(0x21,&r,&r,&sr);
    if(r.x.cflag) {
        buf[0]='-'; buf[1]=0; return;
    }
    /* Il nome è nel DTA a offset 30 */
    /* Usiamo int 21h AH=2Fh per ottenere il DTA */
    char far *dta;
    r.h.ah=0x2F; int86x(0x21,&r,&r,&sr);
    dta=MK_FP(sr.es,r.x.bx);
    /* offset 30 nel DTA = nome file (volume label) */
    int i;
    for(i=0;i<11&&dta[30+i]&&dta[30+i]!=' ';i++)
        buf[i]=dta[30+i];
    buf[i]=0;
    if(i==0){buf[0]='-';buf[1]=0;}
}

/* -------------------------------------------------------
   TASTIERA — stato CAPS/NUM/SCROLL lock
   Byte di stato tastiera a 0040:0017
   bit 6=CAPS, bit 5=NUM, bit 4=SCROLL
   ------------------------------------------------------- */
static void get_keyboard_state(unsigned char *caps,
                                unsigned char *num,
                                unsigned char *scroll) {
    unsigned char far *kbstate=(unsigned char far *)0x417L;
    unsigned char s=*kbstate;
    *caps  =(s>>6)&1;
    *num   =(s>>5)&1;
    *scroll=(s>>4)&1;
}

/* -------------------------------------------------------
   MOUSE — rilevazione e posizione
   int 33h: AX=0 init, AX=3 get pos+buttons
   ------------------------------------------------------- */
static void get_mouse_info(unsigned char *present,
                            unsigned int  *mx,
                            unsigned int  *my,
                            unsigned char *buttons) {
    union REGS r;
    /* Controlla presenza mouse */
    r.x.ax=0x0000;
    int86(0x33,&r,&r);
    if(r.x.ax==0x0000) { *present=0; *mx=0; *my=0; *buttons=0; return; }
    *present=1;
    /* Leggi posizione e pulsanti */
    r.x.ax=0x0003;
    int86(0x33,&r,&r);
    *buttons=(unsigned char)(r.x.bx&0x03);
    *mx=r.x.cx/8; /* coordinate in pixel → caratteri approx */
    *my=r.x.dx/8;
}

/* -------------------------------------------------------
   COM — velocità attuale della porta
   ------------------------------------------------------- */
static unsigned int get_com_baud(void) {
    /* Legge il divisore attuale e calcola il baud rate */
    unsigned int divisor;
    outp(COM_PORT+3, inp(COM_PORT+3)|0x80); /* DLAB on */
    divisor = inp(COM_PORT) | (inp(COM_PORT+1)<<8);
    outp(COM_PORT+3, inp(COM_PORT+3)&0x7F); /* DLAB off */
    if(divisor==0) return 0;
    return (unsigned int)(115200UL/divisor);
}

/* -------------------------------------------------------
   FILE APERTI — contatore handle in uso
   ------------------------------------------------------- */
static unsigned int get_open_files(void) {
    /* Legge la SFT (System File Table) tramite int 21h AH=52h
       che restituisce il puntatore al primo MCB/lista interna */
    union REGS r; struct SREGS sr;
    unsigned int count=0;
    r.h.ah=0x52;
    int86x(0x21,&r,&r,&sr);
    /* ES:BX punta alla lista interna DOS (INVARS) */
    /* offset -2 da ES:BX = segmento primo MCB */
    /* Approccio più semplice: conta handle 0-19 aperti */
    /* Un handle è aperto se GetFileSize non restituisce errore */
    unsigned int h;
    for(h=5;h<20;h++) { /* 0-4 sono stdin/out/err/aux/prn */
        r.h.ah=0x45; /* dup handle — fallisce se non aperto */
        r.x.bx=h;
        int86(0x21,&r,&r);
        if(!r.x.cflag) {
            /* Era aperto, chiudi il dup */
            unsigned int newh=r.x.ax;
            r.h.ah=0x3E; r.x.bx=newh;
            int86(0x21,&r,&r);
            count++;
        }
    }
    return count;
}

/* -------------------------------------------------------
   COSTRUISCI E INVIA PACCHETTO COMPLETO v3
   ------------------------------------------------------- */
static void send_packet(void) {
    char tmp[16];

    /* --- Dati base --- */
    unsigned int  free_ram;
    unsigned char hh,mm,ss;
    unsigned int  yy; unsigned char mo,dd;
    unsigned char dos_maj,dos_min;
    unsigned char drv;

    /* --- Dati estesi --- */
    unsigned int  cpu_type;
    char          bios_date[10];
    char          bios_vendor[10];
    char          drives_buf[64];
    char          vol_buf[16];
    unsigned char caps,num,scroll;
    unsigned char mouse_present;
    unsigned int  mx,my; unsigned char mbuttons;
    unsigned int  com_baud;
    unsigned int  open_files;

    /* Raccolta dati */
    free_ram = get_free_ram();
    get_time(&hh,&mm,&ss);
    get_date(&yy,&mo,&dd);
    get_dos_version(&dos_maj,&dos_min);
    drv = get_drive_activity();

    cpu_type = detect_cpu();
    get_bios_date(bios_date);
    get_bios_vendor(bios_vendor);
    get_drives_info(drives_buf);
    get_volume_label(vol_buf);
    get_keyboard_state(&caps,&num,&scroll);
    get_mouse_info(&mouse_present,&mx,&my,&mbuttons);
    com_baud   = get_com_baud();
    open_files = get_open_files();

    /* --- Costruzione pacchetto --- */
    /* $RAM:nnn;HH:MM:SS;DD/MM/YYYY;DOS:x.y;DRV:n; */
    serial_puts("$RAM:");
    uint_to_str(free_ram,tmp); serial_puts(tmp);

    serial_puts(";");
    uint_to_str2(hh,tmp); serial_puts(tmp); serial_putc(':');
    uint_to_str2(mm,tmp); serial_puts(tmp); serial_putc(':');
    uint_to_str2(ss,tmp); serial_puts(tmp);

    serial_puts(";");
    uint_to_str2(dd,tmp); serial_puts(tmp); serial_putc('/');
    uint_to_str2(mo,tmp); serial_puts(tmp); serial_putc('/');
    uint_to_str4(yy,tmp); serial_puts(tmp);

    serial_puts(";DOS:");
    uint_to_str(dos_maj,tmp); serial_puts(tmp); serial_putc('.');
    uint_to_str(dos_min,tmp); serial_puts(tmp);

    serial_puts(";DRV:");
    serial_putc(drv?'1':'0');

    /* CPU:nnn */
    serial_puts(";CPU:");
    uint_to_str(cpu_type,tmp); serial_puts(tmp);

    /* BIOS:vendor-date */
    serial_puts(";BIOS:");
    serial_puts(bios_vendor); serial_putc('-');
    /* Data: MM/DD/YY → compatta come MMDDYY */
    serial_putc(bios_date[0]); serial_putc(bios_date[1]); /* MM */
    serial_putc(bios_date[3]); serial_putc(bios_date[4]); /* DD */
    serial_putc(bios_date[6]); serial_putc(bios_date[7]); /* YY */

    /* DSK:C:nnn,D:nnn */
    serial_puts(";DSK:");
    serial_puts(drives_buf);

    /* VOL:nome */
    serial_puts(";VOL:");
    serial_puts(vol_buf);

    /* KBD:CNS (CAPS NUM SCROLL, 0/1) */
    serial_puts(";KBD:");
    serial_putc('0'+caps);
    serial_putc('0'+num);
    serial_putc('0'+scroll);

    /* MOUSE:presente,x,y,btn */
    serial_puts(";MOUSE:");
    serial_putc('0'+mouse_present);
    if(mouse_present){
        serial_putc(',');
        uint_to_str(mx,tmp); serial_puts(tmp);
        serial_putc(',');
        uint_to_str(my,tmp); serial_puts(tmp);
        serial_putc(',');
        serial_putc('0'+mbuttons);
    }

    /* COM:baud */
    serial_puts(";COM:");
    uint_to_str(com_baud,tmp); serial_puts(tmp);

    /* FILES:n */
    serial_puts(";FILES:");
    uint_to_str(open_files,tmp); serial_puts(tmp);

    serial_putc('\n');
}

/* -------------------------------------------------------
   INT 8h HANDLER
   ------------------------------------------------------- */
static void __interrupt __far tsr_int8_handler(void) {
    tick_count++;
    if(tick_count>=TICK_RATE){ tick_count=0; send_packet(); }
    _chain_intr(old_int8);
}

/* -------------------------------------------------------
   INIT SERIALE
   ------------------------------------------------------- */
static void serial_init(void) {
    unsigned int divisor=115200/BAUD_RATE;
    outp(COM_PORT+3,0x80);
    outp(COM_PORT+0,divisor&0xFF);
    outp(COM_PORT+1,divisor>>8);
    outp(COM_PORT+3,0x03);
    outp(COM_PORT+2,0xC7);
    outp(COM_PORT+4,0x03);
}

/* -------------------------------------------------------
   VERIFICA INSTALLAZIONE
   ------------------------------------------------------- */
static int tsr_is_installed(void) {
    void __far *vec=_dos_getvect(TSR_INT);
    unsigned int __far *sig;
    if(vec==0) return 0;
    sig=(unsigned int __far *)vec;
    return (*sig==TSR_ID)?1:0;
}

/* -------------------------------------------------------
   DISINSTALLA
   ------------------------------------------------------- */
static void tsr_uninstall(void) {
    _dos_setvect(8,old_int8);
    _dos_setvect(TSR_INT,0);
    printf("VUMONITOR: disinstallato.\n");
}

/* -------------------------------------------------------
   MAIN
   ------------------------------------------------------- */
int main(int argc,char *argv[]) {
    unsigned char dos_maj,dos_min;

    if(argc>1){
        if(argv[1][0]=='/'&&(argv[1][1]=='Q'||argv[1][1]=='q')){
            printf("VUMONITOR: %s\n",tsr_is_installed()?"installato":"non installato");
            return 0;
        }
        if(argv[1][0]=='/'&&(argv[1][1]=='U'||argv[1][1]=='u')){
            if(!tsr_is_installed()){printf("VUMONITOR: non installato.\n");return 1;}
            tsr_uninstall(); return 0;
        }
        printf("VUMONITOR v3.0\nUso: VUMONITOR [/U] [/Q]\n");
        return 0;
    }

    if(tsr_is_installed()){printf("VUMONITOR: gia' installato.\n");return 1;}

    serial_init();
    get_dos_version(&dos_maj,&dos_min);

    printf("VUMONITOR v3.0\n");
    printf("DOS %u.%u  COM1 @ %d baud\n",dos_maj,dos_min,BAUD_RATE);
    printf("CPU: %u\n",detect_cpu());
    printf("RAM libera: %u KB\n",get_free_ram());
    printf("Installazione TSR...\n");

    old_int8=_dos_getvect(8);
    _dos_setvect(8,tsr_int8_handler);
    _dos_setvect(TSR_INT,tsr_int8_handler);

    send_packet(); /* primo pacchetto immediato */

    printf("Installato. Premi VUMONITOR /U per rimuovere.\n\n");

    _dos_keep(0,256); /* ~4KB — più grande per dati estesi */
    return 0;
}
