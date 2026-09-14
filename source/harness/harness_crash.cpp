/*
    harness_crash.cpp - see harness_crash.h. Compiles to nothing without
    -DDESMUME_HARNESS -DHARNESS_CRASH (verify: `nm ... | grep harness_crash`
    and `grep harness_expect_fail` both empty on a release build).
*/
#include "harness.h"            // sub-flag fan-out + harness_transport.h
#include "harness_crash.h"

#if defined(DESMUME_HARNESS) && defined(HARNESS_CRASH)

#include "harness_wire.h"

#include <tuxedo/ppc/exception.h>   // PPCContext, PPCExcptCurPanicFn
#include <ogc/machine/processor.h>  // mfmsr/mtmsr, MSR_* (via tuxedo/ppc/spr.h)
#include <stdio.h>
#include <string.h>

// MEM1 window (cached). Used to sanity-check stack pointers and return
// addresses during the backtrace walk so a garbage frame chain can't fault
// us a second time inside the panic handler.
#define CRASH_MEM_LO 0x80003100u
#define CRASH_MEM_HI 0x81800000u

static PPCExcptPanicFn s_prevPanic = 0;
static volatile int    s_inPanic   = 0;   // re-entrancy guard

static const char *crash_cause(unsigned exid)
{
	switch (exid) {
	case PPC_EXCPT_RESET:   return "RESET";
	case PPC_EXCPT_MCHK:    return "MACHINE-CHECK";
	case PPC_EXCPT_DSI:     return "DSI";
	case PPC_EXCPT_ISI:     return "ISI";
	case PPC_EXCPT_IRQ:     return "EXTERNAL-INT";
	case PPC_EXCPT_ALIGN:   return "ALIGNMENT";
	case PPC_EXCPT_UNDEF:   return "PROGRAM";
	case PPC_EXCPT_FPU:     return "FPU-UNAVAIL";
	case PPC_EXCPT_DECR:    return "DECREMENTER";
	case PPC_EXCPT_SYSCALL: return "SYSCALL";
	case PPC_EXCPT_TRACE:   return "TRACE";
	case PPC_EXCPT_PM:      return "PERF-MON";
	case PPC_EXCPT_BKPT:    return "BREAKPOINT";
	default:               return "UNKNOWN";
	}
}

static void harness_crash_panic(unsigned exid, PPCContext *ctx)
{
	// Best-effort, one shot. If we somehow trap again while building the
	// report, don't recurse - go straight to the old handler.
	if (!s_inPanic && ctx) {
		s_inPanic = 1;

		// The trap entered with external interrupts masked (MSR[EE]=0). The
		// transport's send path (NET = IOS socket IPC, SD = IOS FS IPC) needs
		// interrupts to make progress, so a blocking send would hang here
		// forever. Re-enable EE/RI/FP for the one best-effort packet - the
		// machine is going to halt right after anyway. Standard debug-stub move.
		mtmsr(mfmsr() | MSR_EE | MSR_RI | MSR_FP);
		_sync();

		static char buf[1600];   // static: the machine is dying, don't touch the stack
		int n = snprintf(buf, sizeof buf,
			"cause %s\npc  0x%08x\nlr  0x%08x\nmsr 0x%08x\ncr  0x%08x\n"
			"ctr 0x%08x\nxer 0x%08x\n",
			crash_cause(exid), ctx->pc, ctx->lr, ctx->msr, ctx->cr,
			ctx->ctr, ctx->xer);

		for (int i = 0; i < 32 && n > 0 && n < (int)sizeof buf; i += 4)
			n += snprintf(buf + n, sizeof buf - n,
				"r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x\n",
				i,   ctx->gpr[i],   i+1, ctx->gpr[i+1],
				i+2, ctx->gpr[i+2], i+3, ctx->gpr[i+3]);

		// Short PPC back-chain walk. r1 -> [r1] = caller frame; the caller's
		// return address is in its LR save slot at caller_frame+4.
		u32 sp = ctx->gpr[1];
		for (int depth = 0; depth < 20 && n > 0 && n < (int)sizeof buf; depth++) {
			if (sp < CRASH_MEM_LO || sp >= CRASH_MEM_HI || (sp & 3)) break;
			u32 nextsp = *(volatile u32 *)sp;
			u32 ra     = *(volatile u32 *)(sp + 4);
			if (ra >= CRASH_MEM_LO && ra < CRASH_MEM_HI)
				n += snprintf(buf + n, sizeof buf - n, "bt  0x%08x\n", ra);
			if (nextsp <= sp) break;   // back chain must climb
			sp = nextsp;
		}

		u32 len = ((size_t)n < sizeof buf) ? (u32)n : (u32)(sizeof buf - 1);
		harness_send(HARNESS_PKT_CRASH, buf, len);
	}

	// Chain to whatever was installed before us (the guru screen / halt), so
	// behaviour downstream of the report is exactly as it was.
	if (s_prevPanic && s_prevPanic != &harness_crash_panic)
		s_prevPanic(exid, ctx);
}

void harness_crash_init(void)
{
	if (s_prevPanic) return;   // already installed
	s_prevPanic = PPCExcptCurPanicFn;
	if (s_prevPanic == &harness_crash_panic) return;
	PPCExcptCurPanicFn = &harness_crash_panic;
}

void harness_expect_fail(const char *expr, const char *msg,
                         const char *file, int line)
{
	// Trim any directory prefix so the payload stays short and stable.
	const char *base = file ? file : "?";
	for (const char *p = base; *p; p++)
		if (*p == '/' || *p == '\\') base = p + 1;

	char payload[512];
	payload[0] = 0;   // pass/fail byte: 0 == fail
	int n = 1 + snprintf(payload + 1, sizeof payload - 1,
		"%s:%d  EXPECT(%s)%s%s", base, line,
		expr ? expr : "?",
		(msg && *msg) ? "  " : "", (msg && *msg) ? msg : "");
	u32 len = ((size_t)n < sizeof payload) ? (u32)n : (u32)(sizeof payload - 1);
	harness_send(HARNESS_PKT_ASSERT, payload, len);
}

#endif // DESMUME_HARNESS && HARNESS_CRASH
