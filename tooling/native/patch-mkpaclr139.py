import pathlib, re
src = pathlib.Path("/private/tmp/mkpaclr138.py").read_text()
out = src.replace("piano-paclr138", "piano-paclr139")
assert out.count("piano-paclr139") == 1

anchor = 'print("paclr138 reset service helper installed")'
assert out.count(anchor) == 1
insert = r"""

# ---- paclr139: the idle thread services the architected timer ----
_mr = (dst / "machine_routines.c").read_text()
_idle_def = r'''
extern void piano_reset_service_check(void);
static uint64_t piano_idle_log;
void
piano_idle_timer_service(void)
{
	uint64_t frq, ctl, iar, daif, deadline;
	extern void rtclock_intr(unsigned int);
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	if ((ctl & 1u) == 0u) {
		__builtin_arm_wsr64("CNTV_TVAL_EL0", frq / 1000);
		__builtin_arm_wsr64("CNTV_CTL_EL0", 1);
		__builtin_arm_isb(ISB_SY);
	}
	deadline = mach_absolute_time() + frq / 1000;
	do { ctl = __builtin_arm_rsr64("CNTV_CTL_EL0"); } while (((ctl & 4u) == 0u) && (mach_absolute_time() < deadline));
	if (piano_idle_log < 3u) { piano_idle_log++; printf("piano-reset: idle-service enter ctl=%llx\n", ctl); }
	if (ctl & 4u) {
		__asm__ volatile("mrs %0, daif" : "=r"(daif));
		__asm__ volatile("msr daifset, #0xf" ::: "memory");
		iar = __builtin_arm_rsr64("S3_0_C12_C12_0");
		if ((iar & 0xffffffu) < 1020u) {
			__builtin_arm_wsr64("S3_0_C12_C12_1", iar);
			__builtin_arm_isb(ISB_SY);
		}
		__builtin_arm_wsr64("CNTV_CTL_EL0", 2);
		__builtin_arm_isb(ISB_SY);
		if ((iar & 0xffffffu) == 27u) {
			extern uint64_t piano_gic_irqs;
			piano_gic_irqs++;
			rtclock_intr(0);
		}
		__asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
	}
	piano_reset_service_check();
}

void
machine_idle(void)
'''
old_idle = "void\nmachine_idle(void)\n{"
assert _mr.count(old_idle) == 1, _mr.count(old_idle)
_mr = _mr.replace("void\nmachine_idle(void)", _idle_def, 1)
_mr = _mr.replace("void\nmachine_idle(void)\n{", "void\nmachine_idle(void)\n{\n\tpiano_idle_timer_service();\n\treturn;\n", 1)
(dst / "machine_routines.c").write_text(_mr)
print("paclr139 idle timer service installed")
"""
out = out.replace(anchor, anchor + insert, 1)
pathlib.Path("/private/tmp/mkpaclr139.py").write_text(out)
print("wrote mkpaclr139.py")