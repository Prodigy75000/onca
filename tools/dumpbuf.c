/* dumpbuf.c - boot a cart, tap fire, run, then save raw framebuffers from DRAM
 * as decoded images so we can see what the game's renderer actually produced
 * (independent of the OP/display path). Dumps the view + status buffers Doom uses.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CPU_HZ 13295000u
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu, dsp;

static void dump16(const char *path, uint32_t base, int w, int h, int cry) {
    FILE *o = fopen(path, "wb"); if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint16_t px = onca_peek16(&mem, base + (uint32_t)(y*w + x) * 2);
        uint16_t rgb = onca_op_decode16(px, cry);
        fputc(((rgb>>11)&0x1F)<<3, o); fputc(((rgb>>5)&0x3F)<<2, o); fputc((rgb&0x1F)<<3, o);
    }
    fclose(o); printf("wrote %s\n", path);
}

int main(int c, char **v) {
    FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
    FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
    uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
    int frames = c>3?atoi(v[3]):1000;
    int fa = c>4?atoi(v[4]):490, fb = c>5?atoi(v[5]):505;
    uint32_t btn = c>6?(uint32_t)strtoul(v[6],0,0):(1u<<TJ_A);
    onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
    onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1;
    memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
    onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
    m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
    uint64_t budget=CPU_HZ/60;
    for(int f=0;f<frames;f++){
        mem.joypad1 = (f>=fa && f<fb) ? btn : 0;   /* scripted press window */
        mem.video_irq=1; uint64_t t=cpu.cycles+budget;
        while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
            if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
            if(dsp.running)for(int k=0;k<16&&dsp.running;k++)onca_gpu_step(&dsp);} }
    /* Doom view buffers are 16bpp (CRY), 160 wide x 180 tall. Dump both. */
    dump16("./buf_1D0000.ppm", 0x1D0000, 160, 180, 1);
    dump16("./buf_1E0000.ppm", 0x1E0000, 160, 180, 1);
    /* also try RGB decode in case CRY is wrong */
    dump16("./buf_1E0000_rgb.ppm", 0x1E0000, 160, 180, 0);
    return 0;
}
