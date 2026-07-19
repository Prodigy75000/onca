/* kickprobe.c - in a live Doom level, watch whether the game kicks the GPU/DSP to
 * render (writes to G_CTRL/G_PC/D_CTRL/D_PC/DSP mailbox) and what interrupts it
 * enables (INT1, DSP D_FLAGS), plus the live OLP view-object data pointer. Answers
 * "is the 3D render even invoked, and is it gated on an interrupt we don't fire?"
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/op.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CPU_HZ 13295000u
#define CYC (CPU_HZ/44100u)
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu,dsp;

static int g_on=0, nlog=0;
/* watch writes to render-kick + interrupt registers */
static void tr(void*c,uint32_t pc,uint16_t op){(void)c;(void)op;}
static uint32_t last[8];
static const uint32_t WA[8]={0xF03048,0xF0304C,0xF02224,0xF02228,0xF0222C,0xF02114,0xF02110,0xF000E0};
static const char*WN[8]={"gpucodestart$F03048","$F0304C","A2_BASE$F02224","$F02228","$F0222C","G_CTRL","G_PC","INT1"};

int main(int c,char**v){
  FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
  FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
  uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
  onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
  onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1;
  memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
  onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
  m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
  uint64_t budget=CPU_HZ/60; int launched=0; uint64_t acc=0; int owed=0;
  for(int f=0;f<1460;f++){ mem.joypad1=(f>=900&&f<1100)?(1u<<TJ_A):0;
    if(f==1440) g_on=1;
    mem.video_irq=1; uint64_t t=cpu.cycles+budget; uint64_t prev=cpu.cycles;
    while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
      if(!launched&&cpu.pc>=0x800000&&cpu.pc<0xE00000)launched=1;
      acc+=cpu.cycles-prev; prev=cpu.cycles; while(acc>=CYC){acc-=CYC; if(owed<64)owed++;}
      if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
      if(dsp.running)for(int k=0;k<16&&dsp.running;k++){onca_gpu_step(&dsp);
        if(launched&&owed>0){uint32_t df=onca_gpu_read_ctrl(&dsp,0xF1A100);if((df&DF_I2SENA)&&!(df&GF_IMASK)){onca_gpu_interrupt(&dsp,1);owed--;}}}
      /* poll the watched registers for changes during the traced frame */
      if(g_on){ for(int i=0;i<8;i++){uint32_t val=(i==0)?onca_peek16(&mem,WA[i]):onca_peek32(&mem,WA[i]);
        if(val!=last[i]){ if(nlog++<50) printf("  %-14s = %08X  (cpuPC=%06X)\n",WN[i],val,cpu.pc&0xFFFFFF); last[i]=val; } } }
    }
  }
  uint32_t olp=((uint32_t)onca_peek16(&mem,0xF00022)<<16)|onca_peek16(&mem,0xF00020);
  printf("\nsummary: INT1=%04X D_FLAGS=%08X D_CTRL=%08X G_CTRL_run=%d DSP_run(pc)=%06X\n",
    onca_peek16(&mem,0xF000E0), onca_gpu_read_ctrl(&dsp,0xF1A100), onca_peek32(&mem,0xF1A114),
    gpu.running, dsp.pc&0xFFFFFF);
  printf("OLP=%06X view-obj@021DF0 data=%06X @021E00 data=%06X\n", olp&0xFFFFFF,
    (uint32_t)(((((uint64_t)onca_peek32(&mem,0x21DF0)<<32)|onca_peek32(&mem,0x21DF4))>>43&0x1FFFFF)<<3),
    (uint32_t)(((((uint64_t)onca_peek32(&mem,0x21E00)<<32)|onca_peek32(&mem,0x21E04))>>43&0x1FFFFF)<<3));
  (void)tr;
  return 0;
}
