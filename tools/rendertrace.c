/* rendertrace.c - get Doom into a live level, then trace where the Blitter draws
 * and whether anything targets the 3D view buffers ($1D0000 / $1F4000). Finds
 * whether the renderer draws to the view buffer at all, or elsewhere.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CPU_HZ 13295000u
#define CYC_PER_SAMPLE (CPU_HZ/44100u)
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu, dsp;

static int g_on=0;
static unsigned long g_gpc[0x800], g_dpc[0x800];   /* GPU/DSP pc histograms (RAM) */
static void gtr(void*c,uint32_t pc,uint16_t op){(void)c;(void)op; if(!g_on)return; uint32_t p=pc&0xFFFFFF;
    if(p>=0xF03000&&p<0xF04000)g_gpc[(p-0xF03000)>>2]++;}
static void dtr(void*c,uint32_t pc,uint16_t op){(void)c;(void)op; if(!g_on)return; uint32_t p=pc&0xFFFFFF;
    if(p>=0xF1B000&&p<0xF1D000)g_dpc[(p-0xF1B000)>>2]++;}
static unsigned long g_cpc[0x1000];   /* 68000 pc histogram, bucket 0x100, $0-$100000 */
static void ctr(void*c,uint32_t pc,uint16_t op){(void)c;(void)op; if(!g_on)return; uint32_t p=pc&0xFFFFFF;
    if(p<0x100000)g_cpc[p>>8]++;}
static unsigned long g_nblit;
static struct { uint32_t base; unsigned long n; uint32_t last_cmd, last_cnt; } g_b[80]; static int g_nb;
static void blit_cb(void *ctx,uint32_t cmd,uint32_t a1,uint32_t a2,uint32_t count){ (void)ctx;(void)a2;
    if(!g_on) return; g_nblit++;
    uint32_t base=a1&0xFFF0000;   /* 64KB bucket */
    for(int i=0;i<g_nb;i++)if(g_b[i].base==base){g_b[i].n++;g_b[i].last_cmd=cmd;g_b[i].last_cnt=count;return;}
    if(g_nb<80){g_b[g_nb].base=base;g_b[g_nb].n=1;g_b[g_nb].last_cmd=cmd;g_b[g_nb].last_cnt=count;g_nb++;}
}
int main(int c,char**v){
    FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
    FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
    uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
    int frames=c>3?atoi(v[3]):1600;
    onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
    onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1; mem.blit_trace=blit_cb;
    memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
    onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
    gpu.trace=gtr; dsp.trace=dtr; cpu.trace=ctr;
    m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
    uint64_t budget=CPU_HZ/60; int launched=0; uint64_t acc=0; int owed=0;
    for(int f=0;f<frames;f++){ mem.joypad1=(f>=900&&f<1100)?(1u<<TJ_A):0;
        if(f==1400) g_on=1;   /* trace blits once well into the level */
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
    { int wide=0; for(int i=0;i<0x800;i++) if(g_gpc[i]) wide++;
      printf("GPU distinct PCs in-game=%d; hot: ",wide);
      for(int i=0;i<0x800;i++) if(g_gpc[i]>500000) printf("F%05X(%lu) ",0x03000+(i<<2),g_gpc[i]); printf("\n");
      wide=0; for(int i=0;i<0x800;i++) if(g_dpc[i]) wide++;
      printf("DSP distinct PCs in-game=%d; hot: ",wide);
      for(int i=0;i<0x800;i++) if(g_dpc[i]>500000) printf("F%05X(%lu) ",0x1B000+(i<<2),g_dpc[i]); printf("\n"); }
    { printf("68000 hot PC pages in-game:\n"); for(int i=0;i<0x1000;i++) if(g_cpc[i]>100000) printf("  %06X: %lu\n",i<<8,g_cpc[i]); }
    printf("blits: %lu total -> only bucket(s): ", g_nblit);
    for(int i=0;i<g_nb;i++) printf("%07X(x%lu) ", g_b[i].base, g_b[i].n); printf("\n");
    /* Frame-diff: hash every 8KB DRAM window, run ONE more frame, re-hash, report
     * which windows changed = where this frame's rendering wrote. */
    static unsigned h0[256];
    for(int w=0;w<256;w++){unsigned h=2166136261u;uint32_t a=0x10000+(uint32_t)w*0x2000;
        for(int i=0;i<0x2000;i+=4)h=(h^onca_peek32(&mem,a+i))*16777619u;h0[w]=h;}
    { mem.video_irq=1; uint64_t t=cpu.cycles+budget; uint64_t prev=cpu.cycles;
      while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
        acc+=cpu.cycles-prev; prev=cpu.cycles;
        while(acc>=CYC_PER_SAMPLE){acc-=CYC_PER_SAMPLE; if(owed<64)owed++;}
        if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
        if(dsp.running)for(int k=0;k<16&&dsp.running;k++){ onca_gpu_step(&dsp);
          if(owed>0){uint32_t df=onca_gpu_read_ctrl(&dsp,0xF1A100);
            if((df&DF_I2SENA)&&!(df&GF_IMASK)){onca_gpu_interrupt(&dsp,1);owed--;}}}} }
    printf("--- DRAM 8KB windows that CHANGED during one in-game frame ---\n");
    for(int w=0;w<256;w++){unsigned h=2166136261u;uint32_t a=0x10000+(uint32_t)w*0x2000;
        for(int i=0;i<0x2000;i+=4)h=(h^onca_peek32(&mem,a+i))*16777619u;
        if(h!=h0[w]){int nz=0;for(int i=0;i<0x2000;i+=4)if(onca_peek32(&mem,a+i))nz++;
            printf("  %06X changed (nz=%d/2048)\n",a,nz);}}
    printf("view $1D0000 nz=%d  $1F4000 nz=%d  (per 4KB/1024)\n",
        ({int n=0;for(int i=0;i<4096;i+=4)if(onca_peek32(&mem,0x1D0000+i))n++;n;}),
        ({int n=0;for(int i=0;i<4096;i+=4)if(onca_peek32(&mem,0x1F4000+i))n++;n;}));
    return 0;
}
