/* irqtest.c - drive Doom with the DSP I2S (sample) interrupt firing, to develop
 * and validate the interrupt timebase. Throwaway dev harness.
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
static m68k_t cpu; static onca_mem_t mem; static onca_gpu_t gpu, dsp; static uint16_t fb[FB_W*FB_H];

int main(int c, char **v) {
    FILE*fr=fopen(v[1],"rb"); static uint8_t rom[131072]; size_t rn=fread(rom,1,131072,fr);fclose(fr);
    FILE*fc=fopen(v[2],"rb"); fseek(fc,0,SEEK_END);long cs=ftell(fc);fseek(fc,0,SEEK_SET);
    uint8_t*cart=malloc(cs); if(fread(cart,1,cs,fc)!=(size_t)cs)return 1; fclose(fc);
    int per   = c>3?atoi(v[3]):480;
    int frames= c>4?atoi(v[4]):2200;
    int fa    = c>5?atoi(v[5]):900, fb_=c>6?atoi(v[6]):960;
    onca_mem_init(&mem); memcpy(mem.rom,rom,rn); mem.rom_loaded=rn;
    onca_mem_set_cart(&mem,cart,cs); mem.security_bypass=1;
    memset(&cpu,0,sizeof(cpu)); onca_mem_bind(&mem,&cpu.bus); mem.cycles=&cpu.cycles;
    onca_gpu_init(&gpu,&mem); mem.gpu=&gpu; onca_gpu_init(&dsp,&mem); dsp.is_dsp=1; mem.dsp=&dsp;
    m68k_reset(&cpu); cpu.int_vector=64; mem.cpu_pc=&cpu.pc;
    uint64_t budget=13295000u/60; long ds=0; uint32_t lastcnt=0; int stuck_at=-1;
    for(int f=0;f<frames;f++){ mem.joypad1=(f>=fa&&f<fb_)?(1u<<TJ_A):0;
        mem.video_irq=1; uint64_t t=cpu.cycles+budget;
        while(cpu.cycles<t&&!cpu.halted){ m68k_set_irq(&cpu,mem.video_irq?2:0); m68k_step(&cpu);
            if(gpu.running)for(int k=0;k<16&&gpu.running;k++)onca_gpu_step(&gpu);
            if(dsp.running)for(int k=0;k<16&&dsp.running;k++){ onca_gpu_step(&dsp);
                if(f>=500&&++ds>=per){ds=0; if((onca_gpu_read_ctrl(&dsp,0xF1A100)&DF_I2SENA)&&!(dsp.flags_hi&GF_IMASK)) onca_gpu_interrupt(&dsp,1);} } }
        uint32_t cnt=onca_peek32(&mem,0xF1B02C);
        if(stuck_at<0 && f>1000 && cnt==lastcnt && cnt!=0) stuck_at=f;
        lastcnt=cnt;
    }
    printf("FINAL pc=%06X $F1B02C=%08X $279D4=%08X counter_first_stuck_at_frame=%d\n",
        cpu.pc&0xFFFFFF, onca_peek32(&mem,0xF1B02C), onca_peek32(&mem,0x279D4), stuck_at);
    printf("DSP: running=%d pc=%06X D_FLAGS=%08X I2Sena=%d IMASK=%d D_CTRL=%08X\n",
        dsp.running, dsp.pc&0xFFFFFF, onca_gpu_read_ctrl(&dsp,0xF1A100),
        (onca_gpu_read_ctrl(&dsp,0xF1A100)&DF_I2SENA)?1:0, (dsp.flags_hi&GF_IMASK)?1:0,
        onca_gpu_read_ctrl(&dsp,0xF1A114));
    int drawn=onca_op_render(&mem,fb,FB_W,FB_H); int nz=0; for(int i=0;i<FB_W*FB_H;i++) if(fb[i])nz++;
    printf("op_drawn=%d fb_nonzero=%d\n", drawn, nz);
    FILE*o=fopen("./irq.ppm","wb");
    fprintf(o,"P6\n%d %d\n255\n",FB_W,FB_H);
    for(int i=0;i<FB_W*FB_H;i++){uint16_t p=fb[i];fputc(((p>>11)&0x1F)<<3,o);fputc(((p>>5)&0x3F)<<2,o);fputc((p&0x1F)<<3,o);}
    fclose(o);
    return 0;
}
