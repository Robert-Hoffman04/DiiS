// Task ph-arm7: -DDIIS_ARM7PROF diagnostic. See jit_arm7_prof.h.
#include "jit_arm7_prof.h"
#ifdef DIIS_ARM7PROF
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../harness/harness.h"
#include "../harness/harness_profile.h"
#include <ogc/lwp_watchdog.h>
#include "../armcpu.h"

struct A7Ent { u32 key, key2, n; u64 t; u64 a, b; };
enum { A7N = 1024 };
static A7Ent s_mem[A7N], s_fb[A7N], s_run[A7N];

static A7Ent* a7find(A7Ent* tab, u32 key, u32 key2)
{
	u32 h = (key * 2654435761u ^ key2 * 40503u) & (A7N - 1);
	for (u32 i = 0; i < A7N; i++, h = (h + 1) & (A7N - 1)) {
		A7Ent& e = tab[h];
		if (e.n == 0) { e.key = key; e.key2 = key2; return &e; }
		if (e.key == key && e.key2 == key2) return &e;
	}
	return nullptr;
}

void a7pMem(u32 addr, u32 size, bool write, u32 ticks)
{
	A7Ent* e = a7find(s_mem, addr, (write ? 0x100u : 0u) | size);
	if (e) { e->n++; e->t += ticks; e->a = NDS_ARM7.R[15]; }
}
void a7pFb(u32 pc, u32 op, bool thumb, u32 ticks)
{
	A7Ent* e = a7find(s_fb, op, thumb);
	if (e) { e->n++; e->t += ticks; e->a = pc; }
}
void a7pRun(u32 pc, u32 cycles, u32 insns, u32 ticks, u32 exitPC)
{
	A7Ent* e = a7find(s_run, pc, 0);
	if (e) { e->n++; e->t += ticks; e->a += cycles; e->b += insns; e->key2 = 0; (void)exitPC; }
}

static int cmpT(const void* x, const void* y)
{
	const A7Ent* a = (const A7Ent*)x; const A7Ent* b = (const A7Ent*)y;
	return a->t < b->t ? 1 : a->t > b->t ? -1 : 0;
}

static void dump(const char* tag, A7Ent* tab, u32 frame, int top)
{
	static A7Ent tmp[A7N];
	int m = 0; u64 tn = 0, tt = 0;
	for (int i = 0; i < A7N; i++) if (tab[i].n) { tmp[m++] = tab[i]; tn += tab[i].n; tt += tab[i].t; }
	qsort(tmp, m, sizeof(A7Ent), cmpT);
	harness_profile_emitf("a7p %s frame=%u distinct=%d n=%llu us=%llu", tag, frame, m,
		(unsigned long long)tn, (unsigned long long)ticks_to_microsecs(tt));
	for (int i = 0; i < m && i < top; i++)
		harness_profile_emitf("a7p %s k=%08x k2=%x n=%u us=%llu a=%llu b=%llu", tag,
			tmp[i].key, tmp[i].key2, tmp[i].n, (unsigned long long)ticks_to_microsecs(tmp[i].t),
			(unsigned long long)tmp[i].a, (unsigned long long)tmp[i].b);
	memset(tab, 0, sizeof(A7Ent) * A7N);
}

extern "C" void a7pEmit(u32 frame)
{
	dump("mem", s_mem, frame, 40);
	dump("fb", s_fb, frame, 20);
	dump("run", s_run, frame, 30);
}
#endif
