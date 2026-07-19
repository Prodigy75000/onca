/* viewprobe.c - get Doom into a live level (DSP interrupt timebase), press fire,
 * then inspect the OLP + which DRAM buffers hold rendered content. Goal: find why
 * the 3D view object shows black - is the renderer filling any buffer at all, and
 * does the OP object point at it? Throwaway dev harness.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FB_W 320
#define FB_H 240
#define CPU_HZ 13295000u
#define CYC_PER_SAMPLE (CPU_HZ/44100u)
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu, dsp; static uint16_t fb[FB_W*FB_H];

static int nzblk(uint32_t a, int bytes){int n=0;for(int i=0;i<bytes;i+=4)if(onca_peek32(&mem,a+i))n++;return n;}

int main(int c, char **v){
    FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
    FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
    uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
    int frames=c>3?atoi(v[3]):1600;
    onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
    onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1;
    memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
    onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
    m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
    uint64_t budget=CPU_HZ/60; int launched=0; uint64_t acc=0; int owed=0;
    for(int f=0;f<frames;f++){ mem.joypad1=(f>=900&&f<1100)?(1u<<TJ_A):0;   /* hold fire to start a game */
        mem.video_irq=1; uint64_t t=cpu.cycles+budget; uint64_t prev=cpu.cycles;
        while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
            if(!launched&&cpu.pc>=0x800000&&cpu.pc<0xE00000)launched=1;
            acc+=cpu.cycles-prev; prev=cpu.cycles;
            while(acc>=CYC_PER_SAMPLE){acc-=CYC_PER_SAMPLE; if(owed<64)owed++;}
            if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
            if(dsp.running)for(int k=0;k<16&&dsp.running;k++){ onca_gpu_step(&dsp);
                if(launched&&owed>0){uint32_t df=onca_gpu_read_ctrl(&dsp,0xF1A100);
                    if((df&DF_I2SENA)&&!(df&GF_IMASK)){onca_gpu_interrupt(&dsp,1);owed--;}}}}
    }
    uint32_t olp=((uint32_t)onca_peek16(&mem,0xF00022)<<16)|onca_peek16(&mem,0xF00020);
    printf("final pc=%06X OLP=%06X VMODE=%04X\n",cpu.pc&0xFFFFFF,olp&0xFFFFFF,onca_peek16(&mem,0xF00028));
    /* BFS the OLP, report bitmap objects + their buffer fill */
    printf("--- OLP objects ---\n");
    uint32_t q[128];int qh=0,qt=0,seen[128],ns=0; q[qt++]=olp&0xFFFFFF;
    while(qh<qt&&ns<80){uint32_t a=q[qh++];int dup=0;for(int i=0;i<ns;i++)if(seen[i]==(int)a)dup=1;if(dup)continue;seen[ns++]=a;
        uint64_t p0=((uint64_t)onca_peek32(&mem,a)<<32)|onca_peek32(&mem,a+4);int ty=p0&7;uint32_t lk=((p0>>24)&0x7FFFF)<<3;
        if(ty==4){continue;} if(ty==3){if(qt<126){q[qt++]=a+8;if(lk)q[qt++]=lk;}continue;} if(ty==2){if(lk&&qt<127)q[qt++]=lk;continue;}
        uint32_t data=((p0>>43)&0x1FFFFF)<<3,yp=(p0>>3)&0x7FF,h=(p0>>14)&0x3FF;
        uint64_t p1=((uint64_t)onca_peek32(&mem,a+8)<<32)|onca_peek32(&mem,a+12);int dep=(p1>>12)&7,iw=(p1>>28)&0x3FF;
        printf("  @%06X %s ypos=%u h=%u depth=%d iw=%d data=%06X buf_nz(4k)=%d/1024 link=%06X\n",
            a,ty==1?"SCALED":"BITMAP",yp,h,dep,iw,data,nzblk(data,4096),lk);
        if(lk&&qt<127)q[qt++]=lk;}
    /* sweep DRAM for the biggest filled 8KB region (candidate render target) */
    printf("--- filled DRAM regions (8KB windows, nz>1000) ---\n");
    for(uint32_t a=0x10000;a<0x200000;a+=0x2000){int nz=nzblk(a,0x2000);if(nz>1000)printf("  %06X: %d/2048\n",a,nz);}
    onca_op_render(&mem,fb,FB_W,FB_H);int nzfb=0;for(int i=0;i<FB_W*FB_H;i++)if(fb[i])nzfb++;
    printf("op fb_nonzero=%d\n",nzfb);
    return 0;
}
