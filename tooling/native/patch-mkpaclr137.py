import pathlib, re
src = pathlib.Path("/private/tmp/mkpaclr133.py").read_text()
out = src.replace("piano-paclr133", "piano-paclr137")
assert out.count("piano-paclr137") == 1

marker = 'print("paclr70 spin patch", len(cp))'
assert out.count(marker) == 1
insert = r"""

# ---- paclr137: service the architected timer inside the delay spin ----
_svc = r'''
/* piano: when the delay path spins with callouts dead, poll this CPU's
 * architected timer and feed the XNU rtclock directly. This keeps timeouts,
 * thread_call deadlines and wakeups advancing while a blocked thread spins.
 */
volatile uint64_t piano_delay_ticks;
void
piano_delay_timer_service(void)
{
	static uint64_t next;
	static volatile uint32_t in_service;
	uint64_t frq, now, ctl;
	extern void rtclock_intr(unsigned int);
	if (in_service) { return; }
	now = mach_absolute_time();
	if (now < next) { return; }
	in_service = 1;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	if ((ctl & 1u) == 0u) {
		__builtin_arm_wsr64("CNTV_TVAL_EL0", frq / 1000);
		__builtin_arm_wsr64("CNTV_CTL_EL0", 1);
		__builtin_arm_isb(ISB_SY);
		ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	}
	if (ctl & 4u) {
		uint64_t iar, daif;
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
		next = mach_absolute_time() + frq / 1000;
		piano_delay_ticks++;
		if (piano_delay_ticks <= 3u || (piano_delay_ticks & 1023u) == 0u) {
			printf("piano-reset: delay-service %llu\n", piano_delay_ticks);
		}
	}
	in_service = 0;
}
'''
(dst / "clock.c").write_text((dst / "clock.c").read_text() + _svc)
print("paclr137 delay timer service installed")
"""
out = out.replace(marker, marker + insert, 1)

pat = re.compile(r"while \(mach_absolute_time\(\) < piano_deadline\) \{\s*\}")
out, n = pat.subn("while (mach_absolute_time() < piano_deadline) { extern void piano_delay_timer_service(void); piano_delay_timer_service(); }", out)
assert n == 2, n
pathlib.Path("/private/tmp/mkpaclr137.py").write_text(out)
print("wrote mkpaclr137.py")