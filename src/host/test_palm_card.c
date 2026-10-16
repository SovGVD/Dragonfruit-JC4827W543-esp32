/* No ROM is required for filesystem/ABI checks. Optional arguments:
 * test_palm_card <temporary-card-dir> <local-OS4-ROM> [local-Snake_123.prc]
 * ROMs and apps are never part of the repository or CTest fixtures. */
#include "palm_card.h"
#include "bus.h"
#include "dragonball.h"
#include "installer.h"
#include "host_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static m68k_cpu_t cpu;
static uint32_t scratch = 0x8000;
static unsigned tests;
#define P(n) (scratch + (n))
#define W(v) { (v), M68K_ARG_W }
#define L(v) { (v), M68K_ARG_L }
#define CHECK(x) do { tests++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)

/* A real RAM callback executes TRAP #15/AFFF then RTS. The test trap observes
 * its C arguments above the callback return PC, and can cancel at any offset. */
static struct {
    uint32_t total, user, previous, calls, cancel_at;
    uint16_t error;
} progress;
static unsigned custom_imports, custom_exports;
static uint16_t last_custom_error;

static bool test_trap(m68k_cpu_t *c, uint16_t trap)
{
    if (trap == 0xafff) {
        uint32_t sp = c->regs.a[7];
        uint32_t total = bus_read32(sp + 4), offset = bus_read32(sp + 8);
        CHECK(bus_read32(sp + 12) == progress.user);
        CHECK(total == progress.total && offset <= total);
        CHECK(progress.calls ? offset > progress.previous : offset == 0);
        progress.previous = offset;
        progress.calls++;
        c->regs.d[0] = offset >= progress.cancel_at ? progress.error : 0;
        /* Deliberately disturb caller-saved registers. The subcall must restore
         * the serializer/launcher registers around every callback. */
        c->regs.d[1] = 0x12345678;
        c->regs.a[0] = 0x87654321;
        return true;
    }
    unsigned sel = c->regs.d[2] & 0xffff;
    bool handled = palm_card_trap(c, trap);
    if (handled && trap == 0xa348 && (sel == 39 || sel == 40)) {
        if (sel == 39) custom_imports++; else custom_exports++;
        last_custom_error = c->regs.d[0] & 0xffff;
    }
    return handled;
}

static void reset_progress(uint32_t total, uint32_t cancel_at)
{
    memset(&progress, 0, sizeof progress);
    progress.total = total;
    progress.user = P(900);
    progress.cancel_at = cancel_at;
    progress.error = 0x0507; /* arbitrary callback Err, returned unchanged */
    bus_write16(P(700), 0x4e4f); /* TRAP #15 */
    bus_write16(P(702), 0xafff);
    bus_write16(P(704), 0x4e75); /* RTS */
}

static uint32_t invoke(uint16_t trap, unsigned selector, const m68k_arg_t *args, int n)
{
    /* Keep the synthetic return PC in A3, not below SP where nested ROM calls
     * overwrite it. The trap sees args at SP, exactly as a real Palm caller. */
    uint16_t code[] = {0x343c, (uint16_t)selector, 0x265f, 0x4e4f, trap, 0x2f0b, 0x4e75};
    for (unsigned i = 0; i < sizeof code / sizeof *code; i++) bus_write16(P(i*2), code[i]);
    bool timeout;
    uint32_t result = m68k_call_trap(&cpu, P(0), args, n, NULL, &timeout);
    CHECK(!timeout);
    return result & 0xffff;
}

static uint32_t api(unsigned selector, const m68k_arg_t *args, int n)
{ return invoke(0xa348, selector, args, n); }

static uint32_t rom_call(uint16_t trap, const m68k_arg_t *args, int n, uint32_t *a0)
{
    uint32_t table = bus_read32(0x122);
    bool timeout;
    uint32_t result = m68k_call_trap(&cpu, bus_read32(table+(trap&0x7ff)*4), args,n,a0,&timeout);
    CHECK(!timeout);
    return result;
}

static void text(unsigned off, const char *s) { memcpy(bus_ram_ptr() + P(off), s, strlen(s)+1); }
static void path(const char *s) { text(128, s); }
static uint32_t open_file(const char *s, unsigned mode)
{
    path(s);
    m68k_arg_t args[] = {W(1), L(P(128)), W(mode), L(P(64))};
    CHECK(api(3, args, 4) == 0);
    CHECK(bus_read32(P(64)) != 0);
    return bus_read32(P(64));
}
static void close_file(uint32_t ref)
{ m68k_arg_t args[] = {L(ref)}; CHECK(api(4,args,1)==0); }

static void run_cycles(unsigned count)
{
    for (unsigned i = 0; i < count/4000; i++) {
        m68k_execute(&cpu,4000);
        if (!cpu.regs.stopped) db_timer_advance(4000);
        m68k_set_irq(&cpu,db_interrupt_pending_level());
        palm_card_pump(&cpu);
    }
}

static void screen(const char *root, const char *stage)
{
    char file[512];snprintf(file,sizeof file,"%s/../card-%s.ppm",root,stage);
    FILE *f=fopen(file,"wb");CHECK(f);fprintf(f,"P6\n160 160\n255\n");
    uint32_t fb=db_lcd_framebuffer_addr();
    int bits=db_lcd_bpp(), stride=db_lcd_is_color()?160*bits/8:db.lvpw*2;
    for(int y=0;y<160;y++) {
      uint8_t row[160*3];
      for(int x=0;x<160;x++) {
        unsigned value=bus_read8(fb+y*stride+x*bits/8);
        unsigned index=(value>>(8-bits-(x*bits)%8))&((1u<<bits)-1);
        for(int c=0;c<3;c++)row[x*3+c]=db_lcd_is_color()?db.sed_lut_rgb[index][c]:255-index*255/((1u<<bits)-1);
      }
      CHECK(fwrite(row,1,sizeof row,f)==sizeof row);
    }
    fclose(f);
}

static void tap(int x,int y)
{
    CHECK(host_pen_inject(&cpu,x,y,false));run_cycles(1500000);
    CHECK(host_pen_inject(&cpu,-1,-1,true));run_cycles(4500000);
}

static void filesystem_tests(const char *root)
{
    m68k_arg_t volume[] = {L(P(64)),L(P(68))};
    bus_write32(P(68),0);
    CHECK(api(27,volume,2)==0 && bus_read16(P(64))==1);
    CHECK(bus_read32(P(68))==UINT32_MAX);
    CHECK(api(27,volume,2)==0x290d);
    m68k_arg_t info[] = {W(1),L(P(300))};
    CHECK(api(28,info,2)==0 && bus_read32(P(312))==0x706f7365);
    m68k_arg_t label[] = {W(1),L(P(300)),W(32)};
    CHECK(api(29,label,3)==0 && !strcmp((char*)bus_ram_ptr()+P(300),"SD Card"));
    uint32_t ref = open_file("/roundtrip.txt", 0x1f);
    text(300,"Palm SD round trip\n");
    m68k_arg_t transfer[] = {L(ref),L(19),L(P(300)),L(P(68))};
    CHECK(api(7,transfer,4)==0 && bus_read32(P(68))==19);
    m68k_arg_t seek[] = {L(ref),W(0),L(0)};
    CHECK(api(10,seek,3)==0);
    memset(bus_ram_ptr()+P(300),0,64);
    transfer[1].value=30;
    CHECK(api(6,transfer,4)==0x2a07 && bus_read32(P(68))==19);
    CHECK(!memcmp(bus_ram_ptr()+P(300),"Palm SD round trip\n",19));
    CHECK(palm_card_sync());
    m68k_arg_t one[] = {L(ref),L(P(68))};
    CHECK(api(18,one,2)==0 && bus_read32(P(68))==19);
    transfer[1].value=4; transfer[2].value=BUS_RAM_SIZE-2;
    CHECK(api(6,transfer,4)==0x0502);
    transfer[1].value=0; transfer[2].value=0;
    CHECK(api(6,transfer,4)==0);
    close_file(ref);
    CHECK(api(4,one,1)==0x2a03);
    path("/../outside.txt");
    m68k_arg_t unsafe[] = {W(1),L(P(128)),W(2),L(P(64))};
    CHECK(api(3,unsafe,4)==0x2a0e);
    path("/PALM/../../outside.txt"); CHECK(api(3,unsafe,4)==0x2a0e);
    path("/.. /outside.txt"); CHECK(api(3,unsafe,4)==0x2a0e);
    path("/C:\\outside.txt"); CHECK(api(3,unsafe,4)==0x2a0e);
#ifndef _WIN32
    char link[512]; snprintf(link,sizeof link,"%s/link",root); remove(link);
    CHECK(symlink("/tmp",link)==0);
    path("/link/file"); CHECK(api(3,unsafe,4)==0x2a05); remove(link);
#endif
    ref = open_file("/",2);
    bus_write32(P(68),0); bus_write32(P(304),P(400)); bus_write16(P(308),128);
    m68k_arg_t list[] = {L(ref),L(P(68)),L(P(300))};
    bool found = false;
    unsigned entries = 0;
    for (;;) {
        uint32_t err=api(20,list,3);
        if (err == 0x290d) break;
        CHECK(!err && ++entries < 100);
        if (!strcmp((char*)bus_ram_ptr()+P(400),"roundtrip.txt")) found=true;
    }
    CHECK(found && bus_read32(P(68))==UINT32_MAX);
    close_file(ref);
    /* File/directory mutations, open-handle protection, and truncation. */
    path("/temporary-dir");m68k_arg_t named[]={W(1),L(P(128))};
    CHECK(api(19,named,2)==0);
    path("/temporary-dir/first.txt");CHECK(api(2,named,2)==0);
    CHECK(api(2,named,2)==0x2a06);
    ref=open_file("/temporary-dir/first.txt",7);
    CHECK(api(8,named,2)==0x2a04);
    m68k_arg_t resize[]={L(ref),L(7)};CHECK(!api(13,resize,2));
    one[0].value=ref;CHECK(!api(18,one,2) && bus_read32(P(68))==7);
    path("/temporary-dir");text(300,"moved");
    m68k_arg_t rename[]={W(1),L(P(128)),L(P(300))};
    CHECK(api(9,rename,3)==0x2a04);
    close_file(ref);
    path("/temporary-dir/first.txt");text(300,"second.txt");
    CHECK(!api(9,rename,3));
    ref=open_file("/temporary-dir/second.txt",2);close_file(ref);
    path("/temporary-dir/second.txt");CHECK(!api(8,named,2));
    path("/temporary-dir");CHECK(!api(8,named,2));
    m68k_arg_t format[]={W(0),W(0),L(0)};CHECK(api(24,format,3)==0x2a10);
    /* Exhaustion returns an error without losing existing file handles. */
    uint32_t refs[8];
    for(int i=0;i<8;i++)refs[i]=open_file("/roundtrip.txt",2);
    path("/roundtrip.txt");CHECK(api(3,unsafe,4)==0x2a0f);
    for(int i=0;i<8;i++)close_file(refs[i]);
    ref=open_file("/roundtrip.txt",2);
    palm_card_reset();
    one[0].value=ref; CHECK(api(4,one,1)==0x2a03);
    CHECK(palm_card_init(root,true));
    ref=open_file("/roundtrip.txt",2);
    transfer[0].value=ref;transfer[1].value=1;transfer[2].value=P(300);
    CHECK(api(7,transfer,4)==0x2a05);
    close_file(ref);
    path("/forbidden.txt"); m68k_arg_t create[]={W(1),L(P(128))};
    CHECK(api(2,create,2)==0x2a05);
    CHECK(palm_card_init(root,false));
}

static void database_tests(const char *root, const char *app)
{
    /* A tiny resource DB with code and icon-name resources, generated locally. */
    uint8_t prc[112]={0};
    memcpy(prc,"CardTest",8); wb16(prc+32,1); wb16(prc+34,7);
    memcpy(prc+60,"applDFct",8); wb16(prc+76,2);
    memcpy(prc+78,"code",4); wb16(prc+82,1); wb32(prc+84,100);
    memcpy(prc+88,"tAIN",4); wb16(prc+92,1000); wb32(prc+94,102);
    wb16(prc+100,0x4e75); memcpy(prc+102,"Card Test",10);
    char file[512];snprintf(file,sizeof file,"%s/PALM/Launcher/test.prc",root);
    FILE *f=fopen(file,"wb");CHECK(f);CHECK(fwrite(prc,1,sizeof prc,f)==sizeof prc);CHECK(!fclose(f));
    uint32_t ref=open_file("/PALM/Launcher/test.prc",2);
    m68k_arg_t info[]={L(ref),L(P(300)),L(P(332)),L(P(334)),L(0),L(0),L(0),L(0),L(0),L(0),L(P(340)),L(P(344)),L(P(348))};
    CHECK(api(37,info,13)==0);
    CHECK(!strcmp((char*)bus_ram_ptr()+P(300),"CardTest") && bus_read16(P(334))==7);
    CHECK(bus_read32(P(340))==0x6170706c && bus_read16(P(348))==2);
    m68k_arg_t resource[]={L(ref),L(0x7441494e),W(1000),L(P(64))};
    CHECK(api(36,resource,4)==0);
    uint32_t handle=bus_read32(P(64)), ptr=0;
    m68k_arg_t lock[]={L(handle)};rom_call(0xa021,lock,1,&ptr);
    CHECK(rom_call(0xa030,lock,1,NULL)&0xff); /* allocated in storage, not dynamic heap */
    CHECK(ptr && !strcmp((char*)bus_ram_ptr()+ptr,"Card Test"));
    rom_call(0xa022,lock,1,NULL);rom_call(0xa02b,lock,1,NULL);
    close_file(ref);
    path("/PALM/Launcher/test.prc");
    m68k_arg_t import[]={W(1),L(P(128)),L(P(64)),L(P(68))};
    CHECK(api(34,import,4)==0 && bus_read32(P(68))!=0);
    uint32_t id=bus_read32(P(68));
    CHECK(api(34,import,4)==0x0219 && bus_read32(P(68))==id);
    path("/exported.prc");
    snprintf(file,sizeof file,"%s/exported.prc",root);remove(file);
    m68k_arg_t export[]={W(1),L(P(128)),W(0),L(id)};
    CHECK(api(35,export,4)==0);
    f=fopen(file,"rb");CHECK(f);uint8_t head[78];CHECK(fread(head,1,78,f)==78);fclose(f);
    CHECK(!memcmp(head,"CardTest",8));
    /* Remove fixture from RAM again; the launcher must discover the card copy. */
    m68k_arg_t del[]={W(0),L(id)};CHECK(!rom_call(0xa042,del,2,NULL));
    /* Record DB: a zero-length record followed by a real payload, plus optional
     * app/sort info. Equal offsets must not duplicate the following record. */
    uint8_t pdb[104]={0};memcpy(pdb,"RecordTest",10);wb16(pdb+76,2);
    wb32(pdb+52,96);wb32(pdb+56,98);memcpy(pdb+60,"DATADFcr",8);
    wb32(pdb+78,100);pdb[82]=0x10;pdb[85]=1;
    wb32(pdb+86,100);pdb[90]=0x20;pdb[93]=2;
    memcpy(pdb+96,"ABCDdata",8);
    snprintf(file,sizeof file,"%s/records.pdb",root);f=fopen(file,"wb");CHECK(f);
    CHECK(fwrite(pdb,1,sizeof pdb,f)==sizeof pdb);CHECK(!fclose(f));
    ref=open_file("/records.pdb",2);
    m68k_arg_t record[]={L(ref),W(0),L(P(64)),L(P(68)),L(P(72))};
    CHECK(!api(38,record,5));CHECK(!bus_read32(P(64)));
    CHECK(bus_read8(P(68))==0x10 && bus_read32(P(72))==1);
    record[2].value=0;CHECK(!api(38,record,5));record[2].value=P(64);
    record[1].value=1;CHECK(!api(38,record,5));handle=bus_read32(P(64));
    CHECK(bus_read8(P(68))==0x20 && bus_read32(P(72))==2);
    lock[0].value=handle;CHECK(rom_call(0xa02d,lock,1,NULL)==4);
    rom_call(0xa021,lock,1,&ptr);CHECK(!memcmp(bus_ram_ptr()+ptr,"data",4));
    rom_call(0xa022,lock,1,NULL);rom_call(0xa02b,lock,1,NULL);
    record[1].value=2;CHECK(api(38,record,5)==0x0202);
    info[0].value=ref;info[8].value=P(80);info[9].value=P(84);
    CHECK(!api(37,info,13));
    for(int i=0;i<2;i++) {
        lock[0].value=bus_read32(P(80+i*4));CHECK(lock[0].value);
        CHECK(rom_call(0xa02d,lock,1,NULL)==2);
        rom_call(0xa021,lock,1,&ptr);CHECK(!memcmp(bus_ram_ptr()+ptr,i?"CD":"AB",2));
        rom_call(0xa022,lock,1,NULL);rom_call(0xa02b,lock,1,NULL);
    }
    close_file(ref);
    if (app) {
        f=fopen(app,"rb");CHECK(f);
        snprintf(file,sizeof file,"%s/PALM/Launcher/app.prc",root);
        FILE *out=fopen(file,"wb");CHECK(out);
        uint8_t buf[4096];size_t n;
        while ((n=fread(buf,1,sizeof buf,f)))CHECK(fwrite(buf,1,n,out)==n);
        CHECK(!ferror(f));fclose(f);CHECK(!fclose(out));
    }
}

static void callback_tests(const char *root)
{
    enum { PAYLOAD = 12300, TOTAL = 88 + PAYLOAD };
    uint8_t *image = calloc(1, TOTAL); CHECK(image);
    memcpy(image, "CallbackTest", 12); memcpy(image + 60, "DATADFcb", 8);
    wb16(image + 76, 1); wb32(image + 78, 88); image[85] = 1;
    for (unsigned i = 0; i < PAYLOAD; i++) image[88 + i] = (uint8_t)i;
    char file[512]; snprintf(file, sizeof file, "%s/callback.pdb", root);
    FILE *f = fopen(file, "wb"); CHECK(f);
    CHECK(fwrite(image, 1, TOTAL, f) == TOTAL); CHECK(!fclose(f));
    free(image);

    path("/callback.pdb");
    m68k_arg_t in[] = {W(1),L(P(128)),L(P(64)),L(P(68)),L(P(700)),L(P(900))};
    text(300,"CallbackTest"); m68k_arg_t find[] = {W(0),L(P(300))};
    /* Abort before reading, midway through reading, and after installation.
     * No failing import may leave a database or publish success outputs. */
    const uint32_t stops[] = {0, 4096, TOTAL};
    for (unsigned i = 0; i < sizeof stops / sizeof *stops; i++) {
        reset_progress(TOTAL, stops[i]);
        bus_write16(P(64), 0xbeef); bus_write32(P(68), 0xdeadbeef);
        CHECK(api(39,in,6) == progress.error && progress.calls);
        CHECK(bus_read16(P(64)) == 0xbeef && bus_read32(P(68)) == 0xdeadbeef);
        CHECK(!rom_call(0xa045,find,2,NULL));
    }
    reset_progress(TOTAL,UINT32_MAX);
    in[4].value = P(701); CHECK(api(39,in,6) == 0x0502 && !progress.calls);
    in[4].value = BUS_RAM_SIZE; CHECK(api(39,in,6) == 0x0502 && !progress.calls);
    bus_write16(P(720),0x60fe); /* valid code that never returns */
    in[4].value = P(720); CHECK(api(39,in,6) == 0x2a02 && !progress.calls);
    CHECK(!rom_call(0xa045,find,2,NULL));
    in[4].value = P(700);
    CHECK(!api(39,in,6));
    CHECK(progress.calls >= 4 && progress.previous == TOTAL);
    uint32_t id = bus_read32(P(68)); CHECK(id && !bus_read16(P(64)));
    reset_progress(TOTAL,UINT32_MAX);
    CHECK(api(39,in,6) == 0x0219 && bus_read32(P(68)) == id);
    CHECK(rom_call(0xa045,find,2,NULL) == id); /* duplicate preserved */

    m68k_arg_t out[] = {W(1),L(P(128)),W(0),L(id),L(P(700)),L(P(900))};
    path("/callback-export.pdb");
    snprintf(file,sizeof file,"%s/callback-export.pdb",root); remove(file);
    for (unsigned i = 0; i < sizeof stops / sizeof *stops; i++) {
        reset_progress(TOTAL, stops[i]);
        CHECK(api(40,out,6) == progress.error && progress.calls);
        f = fopen(file,"rb"); CHECK(!f); /* partial/final canceled file removed */
        CHECK(rom_call(0xa045,find,2,NULL) == id); /* source preserved */
    }
    reset_progress(TOTAL,UINT32_MAX);
    out[4].value = P(701); CHECK(api(40,out,6) == 0x0502 && !progress.calls);
    out[4].value = P(700);
    CHECK(!api(40,out,6));
    CHECK(progress.calls >= 4 && progress.previous == TOTAL);
    f=fopen(file,"rb"); CHECK(f); CHECK(!fseek(f,0,SEEK_END));
    CHECK(ftell(f) == TOTAL); CHECK(!fseek(f,88,SEEK_SET));
    for (unsigned i=0;i<PAYLOAD;i++) CHECK(fgetc(f) == (uint8_t)i);
    CHECK(!fclose(f));
    reset_progress(TOTAL,0);
    CHECK(api(40,out,6) == 0x2a06 && !progress.calls); /* existing file intact */
    CHECK(palm_card_init(root,true));
    CHECK(api(40,out,6) == 0x2a05 && !progress.calls);
    CHECK(palm_card_init(root,false));

    /* Custom selectors also accept NULL callbacks, with opaque user data. */
    m68k_arg_t del[] = {W(0),L(id)}; CHECK(!rom_call(0xa042,del,2,NULL));
    path("/callback.pdb"); in[4].value=0; in[5].value=0xffffffff;
    CHECK(!api(39,in,6)); id=bus_read32(P(68)); CHECK(id);
    out[3].value=id; out[4].value=0; out[5].value=0xffffffff;
    path("/callback-null.pdb"); snprintf(file,sizeof file,"%s/callback-null.pdb",root);remove(file);
    CHECK(!api(40,out,6));
    del[1].value=id; CHECK(!rom_call(0xa042,del,2,NULL));
}

static void launcher_copy_tests(const char *root)
{
    unsigned imports = custom_imports, exports = custom_exports;
    /* Starting on the SD Card launcher page, open App > Copy, choose From SD
     * Card and copy Snake. Blank-area taps drain queued form/menu events. */
    const int import_steps[][2] = {{8,6},{35,64},{150,140},{79,39},{100,50},
                                  {150,140},{35,72},{65,147},{150,140}};
    for (unsigned i=0;i<sizeof import_steps/sizeof *import_steps;i++)
        tap(import_steps[i][0],import_steps[i][1]);
    CHECK(custom_imports == imports+1 && last_custom_error == 0);
    text(300,"Snake 123"); m68k_arg_t find[] = {W(0),L(P(300))};
    uint32_t id = rom_call(0xa045,find,2,NULL); CHECK(id);
    screen(root,"copied-to-handheld");

    /* Copy again, decline the replacement prompt, then reverse the direction.
     * Accept replacing the original card app, exercising launcher delete/export. */
    const int export_steps[][2] = {{65,147},{150,140},{65,147},{79,39},{100,39},
                                  {150,140},{35,72},{65,147},{150,140},
                                  {25,147},{150,140}};
    for (unsigned i=0;i<sizeof export_steps/sizeof *export_steps;i++)
        tap(export_steps[i][0],export_steps[i][1]);
    CHECK(custom_imports == imports+1); /* declined import didn't replace RAM */
    CHECK(rom_call(0xa045,find,2,NULL) == id);
    CHECK(custom_exports == exports+1 && last_custom_error == 0);
    char file[512]; snprintf(file,sizeof file,"%s/PALM/Launcher/app.prc",root);
    FILE *f=fopen(file,"rb"); CHECK(f); uint8_t header[78];
    CHECK(fread(header,1,sizeof header,f) == sizeof header); CHECK(!fclose(f));
    CHECK(!strcmp((char *)header,"Snake 123"));
    screen(root,"copied-to-card");
    tap(25,147); tap(150,140); /* Done */
}

int main(int argc,char **argv)
{
    CHECK(argc>=2);
    uint8_t *rom=calloc(1,0x40000);size_t size=0x40000;
    if (argc>2) {
        FILE *f=fopen(argv[2],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));
        size=ftell(f);rewind(f);free(rom);rom=malloc(size);CHECK(rom);
        CHECK(fread(rom,1,size,f)==size);fclose(f);
    }
    bus_init(rom,size);dragonball_init();m68k_init(&cpu);
    cpu.irq_callback=db_interrupt_pending_level;cpu.tick_callback=db_timer_advance;
    cpu.wake_callback=db_iwr_wake_pending;cpu.vector_base_callback=db_int_vector_base;
    cpu.resume_callback=db_cpu_resume;cpu.palm_trap_callback=test_trap;
    CHECK(palm_card_init(argv[1],false));
    cpu.regs.sr=0x2700;cpu.regs.a[7]=0x1f0000;
    if (argc>2) {
        bus_write16(0x1f0100,0x4e73);
        for(unsigned i=2;i<256;i++)bus_write32(i*4,0x1f0100);
        m68k_warm_reset(&cpu);run_cycles(160000000);
        CHECK(m68k_ui_initialized() && palm_card_ready());
        uint32_t p=0;m68k_arg_t alloc[]={L(4096)};rom_call(0xa013,alloc,1,&p);CHECK(p);
        scratch=p;
        m68k_arg_t feature[]={L(0x7666736d),W(0),L(P(64))};
        CHECK(!rom_call(0xa27b,feature,3,NULL) && bus_read32(P(64))==200);
    }
    filesystem_tests(argv[1]);
    if (argc>2) {
        database_tests(argv[1],argc>3?argv[3]:NULL);
        callback_tests(argv[1]);
        /* Tests reset the card handles; restore manager advertisement. */
        for(unsigned i=0;i<10000 && !palm_card_ready();i++)run_cycles(4000);
        CHECK(palm_card_ready());
        const int setup[][2]={{80,80},{80,80},{10,10},{144,148},{79,60},{55,153},{106,152}};
        for(unsigned i=0;i<sizeof setup/sizeof *setup;i++) {
            tap(setup[i][0],setup[i][1]);
            char stage[20];snprintf(stage,sizeof stage,"setup-%u",i);screen(argv[1],stage);
        }
        installer_refresh_launcher(&cpu);run_cycles(30000000);
        screen(argv[1],"launcher");
        tap(139,6);screen(argv[1],"categories");
        tap(121,84);screen(argv[1],"card-apps");
        if(argc>3) {
            tap(73,30);run_cycles(10000000);screen(argv[1],"running-app");
            /* Verify the OS actually switched to the card app, then removed
             * its transient RAM copy when returning to the launcher. */
            m68k_arg_t current[]={L(P(64)),L(P(68))};
            CHECK(!rom_call(0xa0ac,current,2,NULL));
            uint32_t id=bus_read32(P(68));
            m68k_arg_t dbinfo[]={W(bus_read16(P(64))),L(id),L(P(300)),L(0),L(0),L(0),L(0),L(0),L(0),L(0),L(0),L(0),L(P(344))};
            CHECK(!rom_call(0xa046,dbinfo,13,NULL));
            CHECK(!strcmp((char*)bus_ram_ptr()+P(300),"Snake 123"));
            installer_refresh_launcher(&cpu);run_cycles(30000000);
            text(300,"Snake 123");m68k_arg_t find[]={W(0),L(P(300))};
            CHECK(!rom_call(0xa045,find,2,NULL));
            launcher_copy_tests(argv[1]);
        }
        palm_card_reset();dragonball_init();m68k_warm_reset(&cpu);
        run_cycles(160000000);CHECK(palm_card_ready());
        screen(argv[1],"restarted");
        /* Fresh handles after a normal Palm restart, unchanged card contents. */
        m68k_arg_t alloc[]={L(4096)};uint32_t p=0;rom_call(0xa013,alloc,1,&p);CHECK(p);scratch=p;
        uint32_t ref=open_file("/roundtrip.txt",2);close_file(ref);
    }
    palm_card_close();free(rom);
    printf("PASS Palm card: %u checks%s\n",tests,argc>2?" (real ROM, VFS feature, database callbacks/cancellation, import/export, restart)":"");
    return 0;
}
