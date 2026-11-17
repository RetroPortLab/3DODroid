#include "../../core/arm60.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char mem[0x100000];
uint32_t bus_read32(uint32_t a){ uint32_t v; memcpy(&v,mem+(a&0xFFFFC),4); return v; }
uint8_t bus_read8(uint32_t a){ return mem[a&0xFFFFF]; }
void bus_write32(uint32_t a,uint32_t v){ memcpy(mem+(a&0xFFFFC),&v,4); }
void bus_write8(uint32_t a,uint8_t v){ mem[a&0xFFFFF]=v; }
uint32_t bus_fetch(uint32_t a){ return bus_read32(a); }
int main(int c,char**v){ FILE*f=fopen(v[1],"rb"); if(!f)return 1; fread(mem+0x10000,1,0x80000,f);
 arm_reset(); arm.v4=1; /* vectors: reset -> 0x10000 */ 
 uint32_t b=0xEA000000 | (((0x000103a0-8)>>2)&0xFFFFFF); bus_write32(0,b);
 bus_write32(8,0xEAFFFFFE); /* swi: loop */
 arm.r[13]=0xF0000;
 for(int i=0;i<2000;i++){ arm_run(100000); if(arm.r[15]==8) break; }
 printf("%08x  (cycles %llu, mode %x)\n", bus_read32(0x8000), (unsigned long long)arm.cycles, arm.mode); }
