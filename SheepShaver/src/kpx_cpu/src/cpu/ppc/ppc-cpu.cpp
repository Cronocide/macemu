/*
 *  ppc-cpu.cpp - PowerPC CPU definition
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include <stdlib.h>
#include <assert.h>
#include "vm_alloc.h"
#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-cpu.hpp"
#ifndef SHEEPSHAVER
#include "basic-kernel.hpp"
#endif

#if PPC_ENABLE_JIT
#include "cpu/jit/dyngen-exec.h"
#endif

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#if PPC_PROFILE_GENERIC_CALLS
uint32 powerpc_cpu::generic_calls_count[PPC_I(MAX)];
static int generic_calls_ids[PPC_I(MAX)];
const int generic_calls_top_ten = 20;

int generic_calls_compare(const void *e1, const void *e2)
{
	const int id1 = *(const int *)e1;
	const int id2 = *(const int *)e2;
	return powerpc_cpu::generic_calls_count[id2] - powerpc_cpu::generic_calls_count[id1];
}
#endif

#if PPC_PROFILE_REGS_USE
int register_info_compare(const void *e1, const void *e2)
{
	const powerpc_cpu::register_info *ri1 = (powerpc_cpu::register_info *)e1;
	const powerpc_cpu::register_info *ri2 = (powerpc_cpu::register_info *)e2;
	return ri2->count - ri1->count;
}
#endif

static int ppc_refcount = 0;

#if PPC_AARCH64_JIT_DEBUG
struct jit_chain_record { uint32 src_pc; uint32 dst_pc; int slot; };
static const int JIT_CHAIN_RING_SIZE = 64;
static jit_chain_record jit_chain_ring[JIT_CHAIN_RING_SIZE];
static int jit_chain_ring_idx = 0;

struct jit_dispatch_record { uint32 entry_pc; uint32 return_pc; };
static const int JIT_DISPATCH_RING_SIZE = 64;
static jit_dispatch_record jit_dispatch_ring[JIT_DISPATCH_RING_SIZE];
static int jit_dispatch_ring_idx = 0;
#endif

#ifdef DO_CONVENTION_CALL_STATICS
template<> bool nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_init_done = false;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_code_len = 0;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_pf_offset = 0;
#endif

void powerpc_cpu::set_register(int id, any_register const & value)
{
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		gpr(id - powerpc_registers::GPR_BASE) = value.i;
		return;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		fpr(id - powerpc_registers::FPR_BASE) = value.d;
		return;
	}
	switch (id) {
	case powerpc_registers::CR:			cr().set(value.i);		break;
	case powerpc_registers::FPSCR:		fpscr() = value.i;		break;
	case powerpc_registers::XER:		xer().set(value.i);		break;
	case powerpc_registers::LR:			lr() = value.i;			break;
	case powerpc_registers::CTR:		ctr() = value.i;		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			pc() = value.i;			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			gpr(1)= value.i;		break;
	default:							abort();				break;
	}
}

any_register powerpc_cpu::get_register(int id)
{
	any_register value;
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		value.i = gpr(id - powerpc_registers::GPR_BASE);
		return value;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		value.d = fpr(id - powerpc_registers::FPR_BASE);
		return value;
	}
	switch (id) {
	case powerpc_registers::CR:			value.i = cr().get();	break;
	case powerpc_registers::FPSCR:		value.i = fpscr();		break;
	case powerpc_registers::XER:		value.i = xer().get();	break;
	case powerpc_registers::LR:			value.i = lr();			break;
	case powerpc_registers::CTR:		value.i = ctr();		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			value.i = pc();			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			value.i = gpr(1);		break;
	default:							abort();				break;
	}
	return value;
}

#if KPX_MAX_CPUS != 1
uint32 powerpc_registers::reserve_valid = 0;
uint32 powerpc_registers::reserve_addr = 0;
uint32 powerpc_registers::reserve_data = 0;
#endif

void powerpc_cpu::init_registers()
{
	assert((((uintptr)&vr(0)) % 16) == 0);
	for (int i = 0; i < 32; i++) {
		gpr(i) = 0;
		fpr(i) = 0;
	}
	cr().set(0);
	fpscr() = 0;
	xer().set(0);
	lr() = 0;
	ctr() = 0;
	pc() = 0;
}

void powerpc_cpu::init_flight_recorder()
{
#if PPC_FLIGHT_RECORDER
	log_ptr = 0;
	log_ptr_wrapped = false;
#endif
}

void powerpc_cpu::do_record_step(uint32 pc, uint32 opcode)
{
#if PPC_FLIGHT_RECORDER
	log[log_ptr].pc = pc;
	log[log_ptr].opcode = opcode;
#ifdef SHEEPSHAVER
	log[log_ptr].sp = gpr(1);
	log[log_ptr].r24 = gpr(24);
#endif
#if PPC_FLIGHT_RECORDER >= 2
	for (int i = 0; i < 32; i++) {
		log[log_ptr].r[i] = gpr(i);
		log[log_ptr].fr[i] = fpr(i);
	}
	log[log_ptr].lr = lr();
	log[log_ptr].ctr = ctr();
	log[log_ptr].cr = cr().get();
	log[log_ptr].xer = xer().get();
	log[log_ptr].fpscr = fpscr();
#endif
	log_ptr++;
	if (log_ptr == LOG_SIZE) {
		log_ptr = 0;
		log_ptr_wrapped = true;
	}
#endif
}

#if PPC_FLIGHT_RECORDER
void powerpc_cpu::start_log()
{
	logging = true;
	invalidate_cache();
}

void powerpc_cpu::stop_log()
{
	logging = false;
	invalidate_cache();
}

void powerpc_cpu::dump_log(const char *filename)
{
	if (filename == NULL)
		filename = "ppc.log";

	FILE *f = fopen(filename, "w");
	if (f == NULL)
		return;

	int start_ptr = 0;
	int log_size = log_ptr;
	if (log_ptr_wrapped) {
		start_ptr = log_ptr;
		log_size = LOG_SIZE;
	}

	for (int i = 0; i < log_size; i++) {
		int j = (i + start_ptr) % LOG_SIZE;
#if PPC_FLIGHT_RECORDER >= 2
		fprintf(f, " pc %08x  lr %08x ctr %08x  cr %08x xer %08x ", log[j].pc, log[j].lr, log[j].ctr, log[j].cr, log[j].xer);
		fprintf(f, " r0 %08x  r1 %08x  r2 %08x  r3 %08x ", log[j].r[0], log[j].r[1], log[j].r[2], log[j].r[3]);
		fprintf(f, " r4 %08x  r5 %08x  r6 %08x  r7 %08x ", log[j].r[4], log[j].r[5], log[j].r[6], log[j].r[7]);
		fprintf(f, " r8 %08x  r9 %08x r10 %08x r11 %08x ", log[j].r[8], log[j].r[9], log[j].r[10], log[j].r[11]);
		fprintf(f, "r12 %08x r13 %08x r14 %08x r15 %08x ", log[j].r[12], log[j].r[13], log[j].r[14], log[j].r[15]);
		fprintf(f, "r16 %08x r17 %08x r18 %08x r19 %08x ", log[j].r[16], log[j].r[17], log[j].r[18], log[j].r[19]);
		fprintf(f, "r20 %08x r21 %08x r22 %08x r23 %08x ", log[j].r[20], log[j].r[21], log[j].r[22], log[j].r[23]);
		fprintf(f, "r24 %08x r25 %08x r26 %08x r27 %08x ", log[j].r[24], log[j].r[25], log[j].r[26], log[j].r[27]);
		fprintf(f, "r28 %08x r29 %08x r30 %08x r31 %08x\n", log[j].r[28], log[j].r[29], log[j].r[30], log[j].r[31]);
		fprintf(f, "opcode %08x\n", log[j].opcode);
#else
		fprintf(f, " pc %08x opc %08x", log[j].pc, log[j].opcode);
#ifdef SHEEPSHAVER
		fprintf(f, " sp %08x r24 %08x", log[j].sp, log[j].r24);
#endif
		fprintf(f, "| ");
#if !ENABLE_MON
		fprintf(f, "\n");
#endif
#endif
#if ENABLE_MON
		disass_ppc(f, log[j].pc, log[j].opcode);
#endif
	}
	fclose(f);
}
#endif

#if ENABLE_MON
static uint32 mon_read_byte_ppc(uintptr addr)
{
	return *((uint8 *)addr);
}

static void mon_write_byte_ppc(uintptr addr, uint32 b)
{
	uint8 *m = (uint8 *)addr;
	*m = b;
}
#endif

void powerpc_cpu::initialize()
{
#ifdef SHEEPSHAVER
	printf("PowerPC CPU emulator by Gwenole Beauchesne\n");
#endif

#if PPC_PROFILE_REGS_USE
	reginfo = new register_info[32];
	for (int i = 0; i < 32; i++) {
		reginfo[i].id = i;
		reginfo[i].count = 0;
	}
#endif

	init_flight_recorder();
	init_decoder();
	init_registers();
	init_decode_cache();
	execute_depth = 0;

	// Initialize block lookup table
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.initialize();
#endif

	// Init cache range invalidate recorder
	cache_range.start = cache_range.end = 0;

	// Init syscalls handler
	execute_do_syscall = NULL;

	// Init field2mask
	for (int i = 0; i < 256; i++) {
		uint32 mask = 0;
		if (i & 0x01) mask |= 0x0000000f;
		if (i & 0x02) mask |= 0x000000f0;
		if (i & 0x04) mask |= 0x00000f00;
		if (i & 0x08) mask |= 0x0000f000;
		if (i & 0x10) mask |= 0x000f0000;
		if (i & 0x20) mask |= 0x00f00000;
		if (i & 0x40) mask |= 0x0f000000;
		if (i & 0x80) mask |= 0xf0000000;
		field2mask[i] = mask;
	}

#if ENABLE_MON
	mon_init();
	mon_read_byte = mon_read_byte_ppc;
	mon_write_byte = mon_write_byte_ppc;
#endif

#if PPC_PROFILE_COMPILE_TIME
	compile_count = 0;
	compile_time = 0;
	emul_start_time = clock();
#endif
}

#if PPC_ENABLE_JIT
void powerpc_cpu::enable_jit(uint32 cache_size)
{
	use_jit = true;
	if (cache_size)
		codegen.set_cache_size(cache_size);
	codegen.initialize();
}
#endif

// Memory allocator returning powerpc_cpu objects aligned on 16-byte boundaries
// FORMAT: [ alignment ] magic identifier, offset to malloc'ed data, powerpc_cpu data
void *powerpc_cpu::operator new(size_t size)
{
	const int ALIGN = 16;

	// Allocate enough space for powerpc_cpu data + signature + align pad
	uint8 *ptr = (uint8 *)malloc(size + ALIGN * 2);
	if (ptr == NULL)
		throw std::bad_alloc();

	// Align memory
	int ofs = 0;
	while ((((uintptr)ptr) % ALIGN) != 0)
		ofs++, ptr++;

	// Insert signature and offset
	struct aligned_block_t {
		uint32 pad[(ALIGN - 8) / 4];
		uint32 signature;
		uint32 offset;
		uint8  data[sizeof(powerpc_cpu)];
	};
	aligned_block_t *blk = (aligned_block_t *)ptr;
	blk->signature = 0x53435055;		/* 'SCPU' */
	blk->offset = ofs + (&blk->data[0] - (uint8 *)blk);
	assert((((uintptr)&blk->data) % ALIGN) == 0);
	return &blk->data[0];
}

void powerpc_cpu::operator delete(void *p)
{
	uint32 *blk = (uint32 *)p;
	assert(blk[-2] == 0x53435055);		/* 'SCPU' */
	void *ptr = (void *)(((uintptr)p) - blk[-1]);
	free(ptr);
}

#ifdef SHEEPSHAVER
powerpc_cpu::powerpc_cpu()
#if PPC_ENABLE_JIT
	: codegen(this)
#endif
#else
powerpc_cpu::powerpc_cpu(task_struct *parent_task)
	: basic_cpu(parent_task)
#if PPC_ENABLE_JIT
	, codegen(this)
#endif
#endif
{
#if PPC_ENABLE_JIT
	use_jit = false;
#endif
	spcflags().init();
	++ppc_refcount;
	initialize();
}

powerpc_cpu::~powerpc_cpu()
{
	--ppc_refcount;
#if PPC_PROFILE_COMPILE_TIME
	clock_t emul_end_time = clock();

	const char *type = NULL;
#if PPC_ENABLE_JIT
	if (use_jit)
		type = "compile";
#endif
#if PPC_DECODE_CACHE
	if (!type)
		type = "predecode";
#endif
	if (type) {
		printf("### Statistics for block %s\n", type);
		printf("Total block %s count : %d\n", type, compile_count);
		uint32 emul_time = emul_end_time - emul_start_time;
		printf("Total emulation time : %.1f sec\n",
			   double(emul_time) / double(CLOCKS_PER_SEC));
		printf("Total %s time : %.1f sec (%.1f%%)\n", type,
			   double(compile_time) / double(CLOCKS_PER_SEC),
			   100.0 * double(compile_time) / double(emul_time));
		printf("\n");
	}
#endif

#if PPC_PROFILE_GENERIC_CALLS
	if (use_jit && ppc_refcount == 0) {
		uint64 total_generic_calls_count = 0;
		for (int i = 0; i < PPC_I(MAX); i++) {
			generic_calls_ids[i] = i;
			total_generic_calls_count += generic_calls_count[i];
		}
		qsort(generic_calls_ids, PPC_I(MAX), sizeof(int), generic_calls_compare);
		printf("Rank      Count Ratio Name\n");
		for (int i = 0; i < generic_calls_top_ten; i++) {
			uint32 mnemo = generic_calls_ids[i];
			uint32 count = generic_calls_count[mnemo];
			const instr_info_t *ii = powerpc_ii_table;
			while (ii->mnemo != mnemo)
				ii++;
			printf("%03d: %10lu %2.1f%% %s\n", i, count, 100.0*double(count)/double(total_generic_calls_count), ii->name);
		}
	}
#endif

#if PPC_PROFILE_REGS_USE
	printf("\n### Statistics for register usage\n");
	uint64 tot_reg_count = 0;
	for (int i = 0; i < 32; i++)
		tot_reg_count += reginfo[i].count;
	qsort(reginfo, 32, sizeof(register_info), register_info_compare);
	uint64 cum_reg_count = 0;
	for (int i = 0; i < 32; i++) {
		cum_reg_count += reginfo[i].count;
	    printf("r%-2d : %16llu %2.1f%% [%3.1f%%]\n",
			   reginfo[i].id, reginfo[i].count,
			   100.0*double(reginfo[i].count)/double(tot_reg_count),
			   100.0*double(cum_reg_count)/double(tot_reg_count));
	}
	delete[] reginfo;
#endif

	kill_decode_cache();

#if ENABLE_MON
	mon_exit();
#endif
}

void powerpc_cpu::dump_registers()
{
	fprintf(stderr, " r0 %08x   r1 %08x   r2 %08x   r3 %08x\n", gpr(0), gpr(1), gpr(2), gpr(3));
	fprintf(stderr, " r4 %08x   r5 %08x   r6 %08x   r7 %08x\n", gpr(4), gpr(5), gpr(6), gpr(7));
	fprintf(stderr, " r8 %08x   r9 %08x  r10 %08x  r11 %08x\n", gpr(8), gpr(9), gpr(10), gpr(11));
	fprintf(stderr, "r12 %08x  r13 %08x  r14 %08x  r15 %08x\n", gpr(12), gpr(13), gpr(14), gpr(15));
	fprintf(stderr, "r16 %08x  r17 %08x  r18 %08x  r19 %08x\n", gpr(16), gpr(17), gpr(18), gpr(19));
	fprintf(stderr, "r20 %08x  r21 %08x  r22 %08x  r23 %08x\n", gpr(20), gpr(21), gpr(22), gpr(23));
	fprintf(stderr, "r24 %08x  r25 %08x  r26 %08x  r27 %08x\n", gpr(24), gpr(25), gpr(26), gpr(27));
	fprintf(stderr, "r28 %08x  r29 %08x  r30 %08x  r31 %08x\n", gpr(28), gpr(29), gpr(30), gpr(31));
	fprintf(stderr, " f0 %02.5f   f1 %02.5f   f2 %02.5f   f3 %02.5f\n", fpr(0), fpr(1), fpr(2), fpr(3));
	fprintf(stderr, " f4 %02.5f   f5 %02.5f   f6 %02.5f   f7 %02.5f\n", fpr(4), fpr(5), fpr(6), fpr(7));
	fprintf(stderr, " f8 %02.5f   f9 %02.5f  f10 %02.5f  f11 %02.5f\n", fpr(8), fpr(9), fpr(10), fpr(11));
	fprintf(stderr, "f12 %02.5f  f13 %02.5f  f14 %02.5f  f15 %02.5f\n", fpr(12), fpr(13), fpr(14), fpr(15));
	fprintf(stderr, "f16 %02.5f  f17 %02.5f  f18 %02.5f  f19 %02.5f\n", fpr(16), fpr(17), fpr(18), fpr(19));
	fprintf(stderr, "f20 %02.5f  f21 %02.5f  f22 %02.5f  f23 %02.5f\n", fpr(20), fpr(21), fpr(22), fpr(23));
	fprintf(stderr, "f24 %02.5f  f25 %02.5f  f26 %02.5f  f27 %02.5f\n", fpr(24), fpr(25), fpr(26), fpr(27));
	fprintf(stderr, "f28 %02.5f  f29 %02.5f  f30 %02.5f  f31 %02.5f\n", fpr(28), fpr(29), fpr(30), fpr(31));
	fprintf(stderr, " lr %08x  ctr %08x   cr %08x  xer %08x\n", lr(), ctr(), cr().get(), xer().get());
	fprintf(stderr, " pc %08x fpscr %08x\n", pc(), fpscr());
	fflush(stderr);
}

void powerpc_cpu::dump_instruction(uint32 opcode)
{
	fprintf(stderr, "[%08x]-> %08x\n", pc(), opcode);
}

void powerpc_cpu::fake_dump_registers(uint32)
{
	dump_registers();
}

void powerpc_registers::interrupt_copy(powerpc_registers &oregs, powerpc_registers const &iregs)
{
	for (int i = 0; i < 32; i++) {
		oregs.gpr[i] = iregs.gpr[i];
		oregs.fpr[i] = iregs.fpr[i];
	}
	oregs.cr	= iregs.cr;
	oregs.fpscr	= iregs.fpscr;
	oregs.xer	= iregs.xer;
	oregs.lr	= iregs.lr;
	oregs.ctr	= iregs.ctr;
	oregs.pc	= iregs.pc;

	uint32 vrsave = iregs.vrsave;
	oregs.vrsave  = vrsave;
	if (vrsave) {
		for (int i = 31; i >= 0; i--) {
			if (vrsave & 1)
				oregs.vr[i] = iregs.vr[i];
			vrsave >>= 1;
		}
	}
}

bool powerpc_cpu::check_spcflags()
{
#if PPC_AARCH64_JIT_DEBUG
	static unsigned long spc_check_count = 0;
	spc_check_count++;
	if (spc_check_count <= 30 || (PPC_AARCH64_JIT_DEBUG >= 2 && (spc_check_count & 0xffff) == 0)) {
		fprintf(stderr, "[JIT:spc] count=%lu flags=0x%08x", spc_check_count, spcflags().get());
		if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) fprintf(stderr, " EXEC_RETURN");
		if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) fprintf(stderr, " JIT_EXEC_RETURN");
#ifdef SHEEPSHAVER
		if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) fprintf(stderr, " HANDLE_IRQ");
		if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) fprintf(stderr, " TRIGGER_IRQ");
#endif
		if (spcflags().test(SPCFLAG_CPU_ENTER_MON)) fprintf(stderr, " ENTER_MON");
		fprintf(stderr, "\n");
	}
#endif
	if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) {
		spcflags().clear(SPCFLAG_CPU_EXEC_RETURN);
		return false;
	}
#ifdef SHEEPSHAVER
	if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_HANDLE_INTERRUPT);
		static bool processing_interrupt = false;
		if (!processing_interrupt) {
			processing_interrupt = true;
			powerpc_registers r;
			powerpc_registers::interrupt_copy(r, regs());
			HandleInterrupt(&r);
			powerpc_registers::interrupt_copy(regs(), r);
			processing_interrupt = false;
			// On slow CPUs (e.g. Cortex-A53), HandleInterrupt may take
			// longer than one timer period (~16.7ms at 60Hz). If the
			// timer fired during HandleInterrupt, TRIGGER is already set
			// again. Without this clear, TRIGGER→HANDLE conversion below
			// keeps spcflags permanently non-zero, preventing JIT blocks
			// from ever executing. The timer will re-trigger on the next
			// tick, giving the JIT time to run between interrupts.
			spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		}
	}
	if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
	}
#endif
	if (spcflags().test(SPCFLAG_CPU_ENTER_MON)) {
		spcflags().clear(SPCFLAG_CPU_ENTER_MON);
#if ENABLE_MON
		// Start up mon in real-mode
		const char *arg[] = {
			"mon",
#ifdef SHEEPSHAVER
			"-m",
#endif
			"-r",
			NULL
		};
		mon(sizeof(arg)/sizeof(arg[0]) - 1, arg);
#endif
	}
	return true;
}

#if DYNGEN_DIRECT_BLOCK_CHAINING
void * powerpc_cpu::call_compile_chain_block(powerpc_cpu * the_cpu, block_info *sbi)
{
	return the_cpu->compile_chain_block(sbi);
}

void * PF_CONVENTION powerpc_cpu::compile_chain_block(block_info *sbi)
{
	// Block index is stuffed into the source basic block pointer,
	// which is aligned at least on 4-byte boundaries
	const int n = ((uintptr)sbi) & 3;
	sbi = (block_info *)(((uintptr)sbi) & ~3L);

	const uint32 tpc = sbi->li[n].jmp_pc;
	block_info *tbi = my_block_cache.find(tpc);
	if (tbi == NULL) {
		tbi = compile_block(tpc);
#if PPC_AARCH64_JIT_DEBUG
		fprintf(stderr, "[JIT:chain-comp] src=0x%08lx dst=0x%08x (new block compiled)\n",
			(unsigned long)sbi->pc, tpc);
#endif
	}
	assert(tbi && tbi->pc == tpc);

#if PPC_AARCH64_JIT_DEBUG
	static unsigned long chain_count = 0;
	chain_count++;
	if (chain_count <= 50 || (chain_count & 0xffff) == 0) {
		fprintf(stderr, "[JIT:chain] #%lu src=0x%08lx dst=0x%08x slot=%d jmp_addr=%p entry=%p\n",
			chain_count, (unsigned long)sbi->pc, tpc, n, sbi->li[n].jmp_addr, tbi->entry_point);
	}
	jit_chain_ring[jit_chain_ring_idx] = { (uint32)sbi->pc, tpc, n };
	jit_chain_ring_idx = (jit_chain_ring_idx + 1) % JIT_CHAIN_RING_SIZE;
#endif
	dg_set_jmp_target(sbi->li[n].jmp_addr, tbi->entry_point);
	return tbi->entry_point;
}
#endif

#if defined(__GNUC__) && defined(__aarch64__)
__attribute__((hot))
#endif
void powerpc_cpu::execute(uint32 entry)
{
	bool invalidated_cache = false;
	pc() = entry;
#if PPC_EXECUTE_DUMP_STATE
	const bool dump_state = true;
#endif
	execute_depth++;
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	if (execute_depth == 1 || (PPC_ENABLE_JIT && PPC_REENTRANT_JIT)) {
#if PPC_ENABLE_JIT
		if (use_jit) {
#if PPC_AARCH64_JIT_DEBUG
			static unsigned long jit_dispatch_count = 0;
			static unsigned long jit_compile_count = 0;
			static unsigned long jit_cache_miss_count = 0;
			static unsigned long jit_spcflag_exit_count = 0;
			static unsigned long jit_cache_invalidate_count = 0;
#endif
			block_info *bi = my_block_cache.find(pc());
			if (bi == NULL)
				bi = compile_block(pc());
			for (;;) {
				// Execute all cached blocks
				for (;;) {
#if PPC_AARCH64_JIT_DEBUG
					jit_dispatch_count++;
#endif
					uint32 pre_exec_pc = bi->pc;
					uint32 pre_r31 = gpr(31);
					uint32 pre_ctr = ctr();
					uint32 pre_lr = lr();
					codegen.execute(bi->entry_point);
					uint32 post_exec_pc = pc();
#if PPC_AARCH64_JIT_DEBUG
					jit_dispatch_ring[jit_dispatch_ring_idx] = { pre_exec_pc, post_exec_pc };

					if (pre_exec_pc == 0x5046e1dc) {
						fprintf(stderr, "[JIT:BLR-DBG] block 0x%08x -> pc=0x%08x lr=0x%08x->0x%08x ctr=0x%08x->0x%08x\n",
							pre_exec_pc, post_exec_pc, pre_lr, lr(), pre_ctr, ctr());
						fprintf(stderr, "[JIT:BLR-DBG] CTR host addr=%p, CPU state starts at %p\n",
							&regs().ctr, (void*)this);
					} else if (pre_exec_pc >= 0x5046e180 && pre_exec_pc <= 0x5046e200) {
						fprintf(stderr, "[JIT:BLR-DBG] block 0x%08x -> pc=0x%08x lr=0x%08x ctr=0x%08x\n",
							pre_exec_pc, post_exec_pc, lr(), ctr());
					}

					{
						uint32 post_r31 = gpr(31);
						if (post_r31 >= 0x6A000000 && pre_r31 < 0x6A000000) {
							fprintf(stderr, "[JIT:GPR31] r31 corrupted! dispatch #%lu: "
								"pc=0x%08x->0x%08x r31: 0x%08x->0x%08x\n",
								jit_dispatch_count, pre_exec_pc, post_exec_pc,
								pre_r31, post_r31);
							fprintf(stderr, "[JIT:GPR31] block end_pc=0x%08x size=%u entry=%p\n",
								bi->end_pc, bi->size, bi->entry_point);
							fprintf(stderr, "[JIT:GPR31] All GPRs:\n");
							dump_registers();
							fprintf(stderr, "[JIT:GPR31] PPC at block (0x%08x to 0x%08x):\n",
								pre_exec_pc, bi->end_pc + 32);
							for (uint32 addr = pre_exec_pc; addr <= bi->end_pc + 32 && addr < 0x60000000; addr += 4)
								fprintf(stderr, "  0x%08x: 0x%08x%s\n", addr, vm_read_memory_4(addr),
									addr == pre_exec_pc ? " <-- entry" : addr == bi->end_pc ? " <-- end" : "");
							fflush(stderr);
						}
					}
					jit_dispatch_ring_idx = (jit_dispatch_ring_idx + 1) % JIT_DISPATCH_RING_SIZE;

					{
						static bool kd_entry_logged = false;
						if (!kd_entry_logged && post_exec_pc >= 0x68ffe000 && post_exec_pc < 0x6A000000) {
							kd_entry_logged = true;
							fprintf(stderr, "\n[JIT:KD-ENTRY] First jump to kernel data area!\n");
							fprintf(stderr, "[JIT:KD-ENTRY] dispatch #%lu: pc=0x%08x->0x%08x\n",
								jit_dispatch_count, pre_exec_pc, post_exec_pc);
							fprintf(stderr, "[JIT:KD-ENTRY] GPRs:\n");
							dump_registers();
						fprintf(stderr, "[JIT:KD-ENTRY] Key kernel data offsets:\n");
						fprintf(stderr, "  KD+0x1074 (opcode table)  = 0x%08x\n", vm_read_memory_4(0x68ffe000 + 0x1074));
						fprintf(stderr, "  KD+0x1078 (emulator addr) = 0x%08x\n", vm_read_memory_4(0x68ffe000 + 0x1078));
						fprintf(stderr, "  KD+0x0634 (emulator data) = 0x%08x\n", vm_read_memory_4(0x68ffe000 + 0x0634));
						fprintf(stderr, "  KD+0x119c (opcode table2) = 0x%08x\n", vm_read_memory_4(0x68ffe000 + 0x119c));
						fprintf(stderr, "  KD+0x1184 (emul init)     = 0x%08x\n", vm_read_memory_4(0x68ffe000 + 0x1184));
						fprintf(stderr, "[JIT:KD-ENTRY] EmulatorData dump (KD+0x1000 to KD+0x10FF) via KERNEL_DATA_BASE:\n");
						for (uint32 off = 0x1000; off < 0x1100; off += 16) {
							fprintf(stderr, "  +0x%04x:", off);
							for (int j = 0; j < 16; j += 4)
								fprintf(stderr, " %08x", vm_read_memory_4(0x68ffe000 + off + j));
							fprintf(stderr, "\n");
						}
						fprintf(stderr, "[JIT:KD-ENTRY] Same range via KERNEL_DATA2_BASE (0x5fffe000):\n");
						for (uint32 off = 0x1000; off < 0x1100; off += 16) {
							fprintf(stderr, "  +0x%04x:", off);
							for (int j = 0; j < 16; j += 4)
								fprintf(stderr, " %08x", vm_read_memory_4(0x5fffe000 + off + j));
							fprintf(stderr, "\n");
						}
						fprintf(stderr, "[JIT:KD-ENTRY] Host addr check: vm_get_real(0x68fff038)=%p vm_get_real(0x5ffff038)=%p\n",
							vm_do_get_real_address(0x68fff038), vm_do_get_real_address(0x5ffff038));
							fprintf(stderr, "[JIT:KD-ENTRY] PPC instructions at source block (0x%08x):\n", pre_exec_pc);
							for (uint32 a = (pre_exec_pc > 128 ? pre_exec_pc - 128 : 0); a < pre_exec_pc + 512 && a < 0x60000000; a += 4)
								fprintf(stderr, "  0x%08x: 0x%08x%s\n", a, vm_read_memory_4(a),
									a == pre_exec_pc ? " <-- entry" : "");
							fprintf(stderr, "[JIT:KD-ENTRY] Last 8 dispatches:\n");
							for (int i = JIT_DISPATCH_RING_SIZE - 8; i < JIT_DISPATCH_RING_SIZE; i++) {
								int idx = (jit_dispatch_ring_idx + i) % JIT_DISPATCH_RING_SIZE;
								fprintf(stderr, "  0x%08x -> 0x%08x\n",
									jit_dispatch_ring[idx].entry_pc, jit_dispatch_ring[idx].return_pc);
							}
							fflush(stderr);
						}
					}

					if (post_exec_pc >= 0x6A000000) {
						fprintf(stderr, "\n[JIT:CORRUPT] PC out of range after JIT return!\n");
						fprintf(stderr, "[JIT:CORRUPT] dispatch #%lu: entered at pc=0x%08x, returned pc=0x%08x\n",
							jit_dispatch_count, pre_exec_pc, post_exec_pc);
						fprintf(stderr, "[JIT:CORRUPT] block entry_point=%p bi->end_pc=0x%08x bi->size=%u\n",
							bi->entry_point, bi->end_pc, bi->size);
						fprintf(stderr, "[JIT:CORRUPT] Register dump:\n");
						dump_registers();
						fprintf(stderr, "\n[JIT:CORRUPT] Last %d dispatches (entry_pc -> return_pc):\n", JIT_DISPATCH_RING_SIZE);
						for (int i = 0; i < JIT_DISPATCH_RING_SIZE; i++) {
							int idx = (jit_dispatch_ring_idx + i) % JIT_DISPATCH_RING_SIZE;
							if (jit_dispatch_ring[idx].entry_pc == 0 && jit_dispatch_ring[idx].return_pc == 0)
								continue;
							fprintf(stderr, "  [%2d] 0x%08x -> 0x%08x\n",
								i, jit_dispatch_ring[idx].entry_pc, jit_dispatch_ring[idx].return_pc);
						}
						fprintf(stderr, "\n[JIT:CORRUPT] Last %d chain patches (src_pc -> dst_pc [slot]):\n", JIT_CHAIN_RING_SIZE);
						for (int i = 0; i < JIT_CHAIN_RING_SIZE; i++) {
							int idx = (jit_chain_ring_idx + i) % JIT_CHAIN_RING_SIZE;
							if (jit_chain_ring[idx].src_pc == 0 && jit_chain_ring[idx].dst_pc == 0)
								continue;
							fprintf(stderr, "  [%2d] 0x%08x -> 0x%08x [%d]\n",
								i, jit_chain_ring[idx].src_pc, jit_chain_ring[idx].dst_pc, jit_chain_ring[idx].slot);
						}
						fprintf(stderr, "\n[JIT:CORRUPT] PPC instructions at and around entry block (0x%08x, end_pc=0x%08x):\n",
							pre_exec_pc, bi->end_pc);
						uint32 dump_start = pre_exec_pc;
						uint32 dump_end = bi->end_pc + 128;
						if (dump_end < dump_start) dump_end = 0x5FFFFFFF;
						for (uint32 addr = dump_start; addr <= dump_end && addr < 0x60000000; addr += 4) {
							uint32 insn = vm_read_memory_4(addr);
							const char *marker = "";
							if (addr == pre_exec_pc) marker = " <-- entry";
							else if (addr == bi->end_pc) marker = " <-- end_pc";
							fprintf(stderr, "  0x%08x: 0x%08x%s\n", addr, insn, marker);
						}
						fprintf(stderr, "\n[JIT:CORRUPT] Previous dispatch return PCs (last 8):\n");
						for (int i = JIT_DISPATCH_RING_SIZE - 8; i < JIT_DISPATCH_RING_SIZE; i++) {
							int idx = (jit_dispatch_ring_idx + i) % JIT_DISPATCH_RING_SIZE;
							fprintf(stderr, "  0x%08x -> 0x%08x\n",
								jit_dispatch_ring[idx].entry_pc, jit_dispatch_ring[idx].return_pc);
						}
						fprintf(stderr, "\n[JIT:CORRUPT] stats: dispatches=%lu compiles=%lu misses=%lu spc_exits=%lu invalidates=%lu\n",
							jit_dispatch_count, jit_compile_count, jit_cache_miss_count,
							jit_spcflag_exit_count, jit_cache_invalidate_count);
						fflush(stderr);
						abort();
					}
#endif

					if (!spcflags().empty()) {
#if PPC_AARCH64_JIT_DEBUG
						jit_spcflag_exit_count++;
#endif
						if (!check_spcflags())
							goto return_site;

						// Force redecoding if cache was invalidated
						if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
							spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
							invalidated_cache = true;
#if PPC_AARCH64_JIT_DEBUG
							jit_cache_invalidate_count++;
							fprintf(stderr, "[JIT:inv]  cache invalidated, recompile at pc=0x%08x (total=%lu)\n",
								pc(), jit_cache_invalidate_count);
#endif
							break;
						}
					}

					// Don't check for backward branches here as this
					// is now done by generated code. Besides, we will
					// get here if the fast cache lookup failed too.
					if ((bi = my_block_cache.find(pc())) == NULL) {
#if PPC_AARCH64_JIT_DEBUG
						jit_cache_miss_count++;
						if (jit_cache_miss_count <= 50 || (jit_cache_miss_count & 0xffff) == 0) {
							fprintf(stderr, "[JIT:miss] #%lu pc=0x%08x\n",
								jit_cache_miss_count, pc());
						}
#endif
						break;
					}
				}

				// Compile new block
#if PPC_AARCH64_JIT_DEBUG
				jit_compile_count++;
#endif
				bi = compile_block(pc());
			}
		}
#endif
#if PPC_DECODE_CACHE
		block_info *bi = my_block_cache.find(pc());
		if (bi != NULL)
			goto pdi_execute;
		for (;;) {
#if PPC_PROFILE_COMPILE_TIME
			compile_count++;
			clock_t start_time;
			start_time = clock();
#endif
			bi = my_block_cache.new_blockinfo();
			bi->init(pc());

			// Predecode a new block
			block_info::decode_info *di;
			const instr_info_t *ii;
			uint32 dpc;
			di = bi->di = decode_cache_p;
			dpc = pc() - 4;
			do {
				uint32 opcode = vm_read_memory_4(dpc += 4);
				ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::dump_instruction);
					di++;
				}
#endif
#if PPC_FLIGHT_RECORDER
				if (is_logging()) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::record_step);
					di++;
				}
#endif
				di->opcode = opcode;
				di->execute = ii->execute;
				di++;
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = 0;
					di->execute = nv_mem_fun(&powerpc_cpu::fake_dump_registers);
					di++;
				}
#endif
				if (di >= decode_cache_end_p) {
					// Invalidate cache and move current code to start
					invalidate_cache();
					const int blocklen = di - bi->di;
					memmove(decode_cache_p, bi->di, blocklen * sizeof(*di));
					bi->di = decode_cache_p;
					di = bi->di + blocklen;
				}
			} while ((ii->cflow & CFLOW_END_BLOCK) == 0);
			bi->end_pc = dpc;
			bi->min_pc = dpc;
			bi->max_pc = entry;
			bi->size = di - bi->di;
			my_block_cache.add_to_cl_list(bi);
			my_block_cache.add_to_active_list(bi);
			decode_cache_p += bi->size;
#if PPC_PROFILE_COMPILE_TIME
			compile_time += (clock() - start_time);
#endif

			// Execute all cached blocks
		  pdi_execute:
			for (;;) {
				const int r = bi->size % 4;
				di = bi->di + r;
				int n = (bi->size + 3) / 4;
				switch (r) {
				case 0: do {
						di += 4;
						di[-4].execute(this, di[-4].opcode);
				case 3: di[-3].execute(this, di[-3].opcode);
				case 2: di[-2].execute(this, di[-2].opcode);
				case 1: di[-1].execute(this, di[-1].opcode);
					} while (--n > 0);
				}

				if (!spcflags().empty()) {
					if (!check_spcflags())
						goto return_site;

					// Force redecoding if cache was invalidated
					if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
						spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
						invalidated_cache = true;
						break;
					}
				}

				if ((bi->pc != pc()) && ((bi = my_block_cache.find(pc())) == NULL))
					break;
			}
		}
#else
		goto do_interpret;
#endif
	}
#endif
  do_interpret:
	for (;;) {
		uint32 opcode = vm_read_memory_4(pc());
		const instr_info_t *ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_instruction(opcode);
#endif
#if PPC_FLIGHT_RECORDER
		if (is_logging())
			record_step(opcode);
#endif
#ifdef __MINGW32__
		assert(ii->execute.default_call_conv_ptr() != 0);
#else
		assert(ii->execute.ptr() != 0);
#endif
		ii->execute(this, opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_registers();
#endif
		if (!spcflags().empty() && !check_spcflags())
			goto return_site;
	}
  return_site:
	// Tell upper level we invalidated cache?
	if (invalidated_cache)
		spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	--execute_depth;
}

void powerpc_cpu::execute()
{
	execute(pc());
}

void powerpc_cpu::init_decode_cache()
{
#if PPC_DECODE_CACHE
	decode_cache = (block_info::decode_info *)vm_acquire(DECODE_CACHE_SIZE);
	if (decode_cache == VM_MAP_FAILED) {
		fprintf(stderr, "powerpc_cpu: Could not allocate decode cache\n");
		abort();
	}

	D(bug("powerpc_cpu: Allocated decode cache: %d KB at %p\n", DECODE_CACHE_SIZE / 1024, decode_cache));
	decode_cache_p = decode_cache;
	decode_cache_end_p = decode_cache + DECODE_CACHE_MAX_ENTRIES;
#if FLIGHT_RECORDER
	// Leave enough room to last call to record_step()
	decode_cache_end_p -= 2;
#endif
#if PPC_EXECUTE_DUMP_STATE
	// Leave enough room to last calls to dump state functions
	decode_cache_end_p -= 2;
#endif
#endif
}

void powerpc_cpu::kill_decode_cache()
{
#if PPC_DECODE_CACHE
	vm_release(decode_cache, DECODE_CACHE_SIZE);
#endif
}

void powerpc_cpu::invalidate_cache()
{
	D(bug("Invalidate all cache blocks\n"));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.clear();
	my_block_cache.initialize();
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
#endif
#if PPC_ENABLE_JIT
	codegen.invalidate_cache();
#endif
#if PPC_DECODE_CACHE
	decode_cache_p = decode_cache;
#endif
}

void powerpc_block_info::invalidate()
{
#if PPC_DECODE_CACHE
	// Don't do anything if this is a predecoded block
	if (di)
		return;
#endif
#if DYNGEN_DIRECT_BLOCK_CHAINING
	for (int i = 0; i < MAX_TARGETS; i++) {
		link_info * const tli = &li[i];
		uint32 tpc = tli->jmp_pc;
		// For any jump within page boundaries, reset the jump address
		// to the target block resolver (trampoline)
		if (tpc != INVALID_PC && ((tpc ^ pc) >> 12) == 0)
			dg_set_jmp_target(tli->jmp_addr, tli->jmp_resolve_addr);
	}
#endif
}

void powerpc_cpu::invalidate_cache_range(uintptr start, uintptr end)
{
	D(bug("Invalidate cache block [%08x - %08x]\n", start, end));
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
#if DYNGEN_DIRECT_BLOCK_CHAINING
	if (use_jit) {
		// Invalidate on page boundaries
		start &= -4096;
		end = (end + 4095) & -4096;
		D(bug("    at page boundaries [%08x - %08x]\n", start, end));
	}
#endif
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	my_block_cache.clear_range(start, end);
#endif
}
