/* gpudis.c - Tom/Jerry GPU/DSP RISC disassembler. Boots a cart to a chosen frame,
 * then disassembles the GPU program in GPU RAM. Handles MOVEI (2-word immediate),
 * JR/JUMP targets, and register/immediate operands, so the decode is reliable.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/m68k.h"
#include "../src/memory.h"
#include "../src/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu,dsp;

/* Opcodes that take an immediate/quick in the source field (not a register). */
static const char *MN[64] = {
 "add","addc","addq","addqt","sub","subc","subq","subqt","neg","and","or","xor","not","btst","bset","bclr",
 "mult","imult","imultn","resmac","imacn","div","abs","sh","shlq","shrq","sha","sharq","ror","rorq","cmp","cmpq",
 "sat8","sat16","move","moveq","moveta","movefa","movei","loadb","loadw","load","loadp","load_r14n","load_r15n","storeb","storew",
 "store","storep","store_r14n","store_r15n","move_pc","jump","jr","mmult","mtoi","normi","nop","load_r14rn","load_r15rn","store_r14rn","store_r15rn","sat24","pack"
};
/* 1 = source field is an immediate/quick, not a register */
static int imm_src(int op){switch(op){case 2:case 3:case 6:case 7:case 13:case 14:case 15:case 24:case 25:case 27:case 29:case 31:case 35:case 43:case 44:case 49:case 50:return 1;}return 0;}

int main(int c,char**v){
  FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
  FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
  uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
  int frame=c>3?atoi(v[3]):680;
  uint32_t lo=c>4?strtoul(v[4],0,16):0xF03000, hi=c>5?strtoul(v[5],0,16):0xF03400;
  onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
  onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1;
  memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
  onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
  m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
  uint64_t budget=13295000u/60;
  for(int f=0;f<frame;f++){ mem.joypad1=(f>=560&&f<620)?(1u<<TJ_A):0;
    mem.video_irq=1; uint64_t t=cpu.cycles+budget;
    while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
      if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
      if(dsp.running)for(int k=0;k<16&&dsp.running;k++)onca_gpu_step(&dsp);} }
  uint32_t a=lo;
  while(a<hi){
    uint16_t w=onca_peek16(&mem,a);
    int op=(w>>10)&0x3F, s=(w>>5)&0x1F, d=w&0x1F;
    printf("%06X: %04X  %-11s ", a, w, MN[op]);
    if(op==38){ /* movei */
      uint32_t imm=onca_peek16(&mem,a+2)|(onca_peek16(&mem,a+4)<<16);
      printf("#$%08X,r%d", imm, d); a+=6;
      if(imm>=0x279C0&&imm<0x27A00) printf("   ; <- results block $279CC-E8");
      if((imm&0xFFFFFF)>=0xF03000&&(imm&0xFFFFFF)<0xF04000) printf("   ; GPU RAM");
      printf("\n"); continue;
    }
    if(op==53){ /* jr cc,n */
      int off=(s&0x10)?(s|~0x1F):s; uint32_t tgt=(a+2+off*2)&0xFFFFFF;
      printf("cc=%d,$%06X", d, tgt);
    } else if(op==52){ printf("cc=%d,(r%d)", d, s);
    } else if(imm_src(op)){ printf("#%d,r%d", s?s:32, d);
    } else printf("r%d,r%d", s, d);
    printf("\n"); a+=2;
  }
  return 0;
}
