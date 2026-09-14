/*  Copyright (C) 2006 yopyop
	Copyright (C) 2009-2011 DeSmuME team
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <algorithm>
#include "types.h"
#include "arm_instructions.h"
#include "thumb_instructions.h"
#include "cp15.h"
#include "bios.h"
#include "debug.h"
#include "Disassembler.h"
#include "NDSSystem.h"
#include "MMU_timing.h"
#ifdef HAVE_LUA
#include "lua-engine.h"
#endif

template<u32> static u32 armcpu_prefetch();

FORCEINLINE u32 armcpu_prefetch(armcpu_t *armcpu) { 
	if(armcpu->proc_ID==0) return armcpu_prefetch<0>();
	else return armcpu_prefetch<1>();
}

const unsigned char arm_cond_table[16*16] = {
    /* N=0, Z=0, C=0, V=0 */
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,
    0x00,0xFF,0xFF,0x00,0xFF,0x00,0xFF,0x20,
    /* N=0, Z=0, C=0, V=1 */
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x00,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=0, Z=0, C=1, V=0 */
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0x00,0xFF,
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x20,
    /* N=0, Z=0, C=1, V=1 */
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x00,
    0xFF,0x00,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=0, Z=1, C=0, V=0 */
    0xFF,0x00,0x00,0xFF,0x00,0xFF,0x00,0xFF,
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x20,
    /* N=0, Z=1, C=0, V=1 */
    0xFF,0x00,0x00,0xFF,0x00,0xFF,0xFF,0x00,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=0, Z=1, C=1, V=0 */
    0xFF,0x00,0xFF,0x00,0x00,0xFF,0x00,0xFF,
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x20,
    /* N=0, Z=1, C=1, V=1 */
    0xFF,0x00,0xFF,0x00,0x00,0xFF,0xFF,0x00,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=0, C=0, V=0 */
    0x00,0xFF,0x00,0xFF,0xFF,0x00,0x00,0xFF,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=0, C=0, V=1 */
    0x00,0xFF,0x00,0xFF,0xFF,0x00,0xFF,0x00,
    0x00,0xFF,0xFF,0x00,0xFF,0x00,0xFF,0x20,
    /* N=1, Z=0, C=1, V=0 */
    0x00,0xFF,0xFF,0x00,0xFF,0x00,0x00,0xFF,
    0xFF,0x00,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=0, C=1, V=1 */
    0x00,0xFF,0xFF,0x00,0xFF,0x00,0xFF,0x00,
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x20,
    /* N=1, Z=1, C=0, V=0 */
    0xFF,0x00,0x00,0xFF,0xFF,0x00,0x00,0xFF,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=1, C=0, V=1 */
    0xFF,0x00,0x00,0xFF,0xFF,0x00,0xFF,0x00,
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=1, C=1, V=0 */
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0x00,0xFF,
    0x00,0xFF,0x00,0xFF,0x00,0xFF,0xFF,0x20,
    /* N=1, Z=1, C=1, V=1 */
    0xFF,0x00,0xFF,0x00,0xFF,0x00,0xFF,0x00,
    0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x20,
};

armcpu_t NDS_ARM7;
armcpu_t NDS_ARM9;

#define SWAP(a, b, c) do      \
	              {       \
                         c=a; \
                         a=b; \
                         b=c; \
		      }       \
                      while(0)

int armcpu_new( armcpu_t *armcpu, u32 id){
	armcpu->proc_ID = id;

	armcpu->stalled = 0;

	armcpu_init(armcpu, 0);

	return 0;
} 

//call this whenever CPSR is changed (other than CNVZQ or T flags); interrupts may need to be unleashed
void armcpu_t::changeCPSR()
{
	//but all it does is give them a chance to unleash by forcing an immediate reschedule
	//TODO - we could actually set CPSR through here and look for a change in the I bit
	//that would be a little optimization as well as a safety measure if we prevented setting CPSR directly
	NDS_Reschedule();
}

void armcpu_init(armcpu_t *armcpu, u32 adr)
{
	armcpu->LDTBit = (armcpu->proc_ID==0); //arm9 is ARMv5 style. this should be renamed, or more likely, all references to this should poll a function to return an architecture level enum
	armcpu->intVector = 0xFFFF0000 * (armcpu->proc_ID==0);
	armcpu->waitIRQ = FALSE;
	armcpu->wirq = FALSE;

	for(int i = 0; i < 16; ++i)
	{
		armcpu->R[i] = 0;
		if(armcpu->coproc[i]) free(armcpu->coproc[i]);
		armcpu->coproc[i] = NULL;
	}
	
	armcpu->CPSR.val = armcpu->SPSR.val = SYS;
	
	armcpu->R13_usr = armcpu->R14_usr = 0;
	armcpu->R13_svc = armcpu->R14_svc = 0;
	armcpu->R13_abt = armcpu->R14_abt = 0;
	armcpu->R13_und = armcpu->R14_und = 0;
	armcpu->R13_irq = armcpu->R14_irq = 0;
	armcpu->R8_fiq = armcpu->R9_fiq = armcpu->R10_fiq = armcpu->R11_fiq = armcpu->R12_fiq = armcpu->R13_fiq = armcpu->R14_fiq = 0;
	
	armcpu->SPSR_svc.val = armcpu->SPSR_abt.val = armcpu->SPSR_und.val = armcpu->SPSR_irq.val = armcpu->SPSR_fiq.val = 0;

	armcpu->next_instruction = adr;
	
	// only ARM9 have co-processor
	if (armcpu->proc_ID==0)
		armcpu->coproc[15] = (armcp_t*)armcp15_new(armcpu);

	armcpu_prefetch(armcpu);
}

u32 armcpu_switchMode(armcpu_t *armcpu, u8 mode)
{
	u32 oldmode = armcpu->CPSR.bits.mode;
	
	switch(oldmode)
	{
		case USR :
		case SYS :
			armcpu->R13_usr = armcpu->R[13];
			armcpu->R14_usr = armcpu->R[14];
			break;
			
		case FIQ :
			{
				u32 tmp;
				SWAP(armcpu->R[8], armcpu->R8_fiq, tmp);
				SWAP(armcpu->R[9], armcpu->R9_fiq, tmp);
				SWAP(armcpu->R[10], armcpu->R10_fiq, tmp);
				SWAP(armcpu->R[11], armcpu->R11_fiq, tmp);
				SWAP(armcpu->R[12], armcpu->R12_fiq, tmp);
				armcpu->R13_fiq = armcpu->R[13];
				armcpu->R14_fiq = armcpu->R[14];
				armcpu->SPSR_fiq = armcpu->SPSR;
				break;
			}
		case IRQ :
			armcpu->R13_irq = armcpu->R[13];
			armcpu->R14_irq = armcpu->R[14];
			armcpu->SPSR_irq = armcpu->SPSR;
			break;
			
		case SVC :
			armcpu->R13_svc = armcpu->R[13];
			armcpu->R14_svc = armcpu->R[14];
			armcpu->SPSR_svc = armcpu->SPSR;
			break;
		
		case ABT :
			armcpu->R13_abt = armcpu->R[13];
			armcpu->R14_abt = armcpu->R[14];
			armcpu->SPSR_abt = armcpu->SPSR;
			break;
			
		case UND :
			armcpu->R13_und = armcpu->R[13];
			armcpu->R14_und = armcpu->R[14];
			armcpu->SPSR_und = armcpu->SPSR;
			break;
		default :
			break;
		}
		
		switch(mode)
		{
			case USR :
			case SYS :
				armcpu->R[13] = armcpu->R13_usr;
				armcpu->R[14] = armcpu->R14_usr;
				//SPSR = CPSR;
				break;
				
			case FIQ :
				{
					u32 tmp;
					SWAP(armcpu->R[8], armcpu->R8_fiq, tmp);
					SWAP(armcpu->R[9], armcpu->R9_fiq, tmp);
					SWAP(armcpu->R[10], armcpu->R10_fiq, tmp);
					SWAP(armcpu->R[11], armcpu->R11_fiq, tmp);
					SWAP(armcpu->R[12], armcpu->R12_fiq, tmp);
					armcpu->R[13] = armcpu->R13_fiq;
					armcpu->R[14] = armcpu->R14_fiq;
					armcpu->SPSR = armcpu->SPSR_fiq;
					break;
				}
				
			case IRQ :
				armcpu->R[13] = armcpu->R13_irq;
				armcpu->R[14] = armcpu->R14_irq;
				armcpu->SPSR = armcpu->SPSR_irq;
				break;
				
			case SVC :
				armcpu->R[13] = armcpu->R13_svc;
				armcpu->R[14] = armcpu->R14_svc;
				armcpu->SPSR = armcpu->SPSR_svc;
				break;
				
			case ABT :
				armcpu->R[13] = armcpu->R13_abt;
				armcpu->R[14] = armcpu->R14_abt;
				armcpu->SPSR = armcpu->SPSR_abt;
				break;
				
          case UND :
				armcpu->R[13] = armcpu->R13_und;
				armcpu->R[14] = armcpu->R14_und;
				armcpu->SPSR = armcpu->SPSR_und;
				break;
				
				default :
					break;
	}
	
	armcpu->CPSR.bits.mode = mode & 0x1F;
	armcpu->changeCPSR();
	return oldmode;
}

u32 armcpu_Wait4IRQ(armcpu_t *cpu)
{
	cpu->waitIRQ = TRUE;
	//cpu->halt_IE_and_IF = TRUE;
	return 1;
}

template<u32 PROCNUM>
FORCEINLINE static u32 armcpu_prefetch()
{
	armcpu_t* const armcpu = &ARMPROC;


	if(armcpu->CPSR.bits.T == 0)
	{
		u32 curInstruction = armcpu->next_instruction;

		armcpu->instruction = _MMU_read32<PROCNUM,MMU_AT_CODE>(curInstruction&0xFFFFFFFC);
		armcpu->instruct_adr = curInstruction;
		armcpu->next_instruction = curInstruction + 4;
		armcpu->R[15] = curInstruction + 8;


		return MMU_codeFetchCycles<PROCNUM,32>(curInstruction);
	}

	u32 curInstruction = armcpu->next_instruction;

	armcpu->instruction = _MMU_read16<PROCNUM, MMU_AT_CODE>(curInstruction&0xFFFFFFFE);
	armcpu->instruct_adr = curInstruction;
	armcpu->next_instruction = curInstruction + 2;
	armcpu->R[15] = curInstruction + 4;


	if(PROCNUM==0)
	{
		// arm9 fetches 2 instructions at a time in thumb mode
		if(!(curInstruction == armcpu->instruct_adr + 2 && (curInstruction & 2)))
			return MMU_codeFetchCycles<PROCNUM,32>(curInstruction);
		else
			return 0;
	}

	return MMU_codeFetchCycles<PROCNUM,16>(curInstruction);
}

#if 0 /* not used */
static BOOL FASTCALL test_EQ(Status_Reg CPSR) { return ( CPSR.bits.Z); }
static BOOL FASTCALL test_NE(Status_Reg CPSR) { return (!CPSR.bits.Z); }
static BOOL FASTCALL test_CS(Status_Reg CPSR) { return ( CPSR.bits.C); }
static BOOL FASTCALL test_CC(Status_Reg CPSR) { return (!CPSR.bits.C); }
static BOOL FASTCALL test_MI(Status_Reg CPSR) { return ( CPSR.bits.N); }
static BOOL FASTCALL test_PL(Status_Reg CPSR) { return (!CPSR.bits.N); }
static BOOL FASTCALL test_VS(Status_Reg CPSR) { return ( CPSR.bits.V); }
static BOOL FASTCALL test_VC(Status_Reg CPSR) { return (!CPSR.bits.V); }
static BOOL FASTCALL test_HI(Status_Reg CPSR) { return (CPSR.bits.C) && (!CPSR.bits.Z); }
static BOOL FASTCALL test_LS(Status_Reg CPSR) { return (CPSR.bits.Z) || (!CPSR.bits.C); }
static BOOL FASTCALL test_GE(Status_Reg CPSR) { return (CPSR.bits.N==CPSR.bits.V); }
static BOOL FASTCALL test_LT(Status_Reg CPSR) { return (CPSR.bits.N!=CPSR.bits.V); }
static BOOL FASTCALL test_GT(Status_Reg CPSR) { return (!CPSR.bits.Z) && (CPSR.bits.N==CPSR.bits.V); }
static BOOL FASTCALL test_LE(Status_Reg CPSR) { return ( CPSR.bits.Z) || (CPSR.bits.N!=CPSR.bits.V); }
static BOOL FASTCALL test_AL(Status_Reg CPSR) { return 1; }

static BOOL (FASTCALL* test_conditions[])(Status_Reg CPSR)= {
	test_EQ , test_NE ,
	test_CS , test_CC ,
	test_MI , test_PL ,
	test_VS , test_VC ,
	test_HI , test_LS ,
	test_GE , test_LT ,
	test_GT , test_LE ,
	test_AL
};
#define TEST_COND2(cond, CPSR) \
	(cond<15&&test_conditions[cond](CPSR))
#endif


BOOL armcpu_irqException(armcpu_t *armcpu)
{
    Status_Reg tmp;

	if(armcpu->CPSR.bits.I) return FALSE;

      
	tmp = armcpu->CPSR;
	armcpu_switchMode(armcpu, IRQ);


	armcpu->R[14] = armcpu->instruct_adr + 4;

	armcpu->SPSR = tmp;
	armcpu->CPSR.bits.T = 0;
	armcpu->CPSR.bits.I = 1;
	armcpu->next_instruction = armcpu->intVector + 0x18;
	armcpu->waitIRQ = 0;


	armcpu->R[15] = armcpu->next_instruction + 8;
	armcpu_prefetch(armcpu);

	return TRUE;
}

u32 TRAPUNDEF(armcpu_t* cpu){
	LOG("Undefined instruction: %#08X PC = %#08X \n", cpu->instruction, cpu->instruct_adr);

	if (((cpu->intVector != 0) ^ (cpu->proc_ID == ARMCPU_ARM9))){
		Status_Reg tmp = cpu->CPSR;
		armcpu_switchMode(cpu, UND);			// enter und mode
		cpu->R[14] = cpu->R[15] - 4;			// jump to und Vector
		cpu->SPSR = tmp;						// save old CPSR as new SPSR
		cpu->CPSR.bits.T = 0;					// handle as ARM32 code
		cpu->CPSR.bits.I = cpu->SPSR.bits.I;	// keep int disable flag
		cpu->changeCPSR();
		cpu->R[15] = cpu->intVector + 0x04;
		cpu->next_instruction = cpu->R[15];
	}
	else{
		emu_halt();
	}
	return 4;
}

template<int PROCNUM>
u32 armcpu_exec()
{
	// Usually, fetching and executing are processed parallelly.
	// So this function stores the cycles of each process to
	// the variables below, and returns appropriate cycle count.
	u32 cFetch = 0;
	u32 cExecute = 0;

	//this assert is annoying. but sometimes it is handy.
	//assert(ARMPROC.instruct_adr!=0x00000000);

	if(ARMPROC.CPSR.bits.T == 0)
	{
		if(
			CONDITION(ARMPROC.instruction) == 0x0E  //fast path for unconditional instructions
			|| (TEST_COND(CONDITION(ARMPROC.instruction), CODE(ARMPROC.instruction), ARMPROC.CPSR)) //handles any condition
			)
		{
#ifdef HAVE_LUA
			CallRegisteredLuaMemHook(ARMPROC.instruct_adr, 4, ARMPROC.instruction, LUAMEMHOOK_EXEC); // should report even if condition=false?
#endif
			if(PROCNUM==0) {
				#ifdef DEVELOPER
				DEBUG_statistics.instructionHits[0].arm[INSTRUCTION_INDEX(ARMPROC.instruction)]++;
				#endif
				cExecute = arm_instructions_set_0[INSTRUCTION_INDEX(ARMPROC.instruction)](ARMPROC.instruction);
			}
			else {
				#ifdef DEVELOPER
				DEBUG_statistics.instructionHits[1].arm[INSTRUCTION_INDEX(ARMPROC.instruction)]++;
				#endif
				cExecute = arm_instructions_set_1[INSTRUCTION_INDEX(ARMPROC.instruction)](ARMPROC.instruction);
			}
		}
		else
			cExecute = 1; // If condition=false: 1S cycle

		cFetch = armcpu_prefetch<PROCNUM>();
		return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
	}

#ifdef HAVE_LUA
	CallRegisteredLuaMemHook(ARMPROC.instruct_adr, 2, ARMPROC.instruction, LUAMEMHOOK_EXEC);
#endif
	if(PROCNUM==0)
	{
		#ifdef DEVELOPER
		DEBUG_statistics.instructionHits[0].thumb[ARMPROC.instruction>>6]++;
		#endif
		cExecute = thumb_instructions_set_0[ARMPROC.instruction>>6](ARMPROC.instruction);
	}
	else {
		#ifdef DEVELOPER
		DEBUG_statistics.instructionHits[1].thumb[ARMPROC.instruction>>6]++;
		#endif
		cExecute = thumb_instructions_set_1[ARMPROC.instruction>>6](ARMPROC.instruction);
	}

	cFetch = armcpu_prefetch<PROCNUM>();
	return MMU_fetchExecuteCycles<PROCNUM>(cExecute, cFetch);
}

//these templates needed to be instantiated manually
template u32 armcpu_exec<0>();
template u32 armcpu_exec<1>();
