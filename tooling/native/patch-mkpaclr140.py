import pathlib, re
src = pathlib.Path("/private/tmp/mkpaclr139.py").read_text()
out = src.replace("piano-paclr139", "piano-paclr140")
assert out.count("piano-paclr140") == 1

# delay/idle paths stop calling rtclock: the bound ticker owns it
out, n = re.subn(r"piano_gic_irqs\+\+;\s*rtclock_intr\(0\);", "/* ufs140: the bound ticker owns rtclock */", out)
assert n >= 1, n

# start the bound ticker again
old = "((kern_return_t)KERN_SUCCESS)"
assert out.count(old) == 1
out = out.replace(old, "kernel_thread_start(piano_simple_tick, NULL, &th)", 1)
old2 = "if (0 && th != NULL) {"
assert out.count(old2) == 1
out = out.replace(old2, "if (th != NULL) {", 1)

anchor = 'print("paclr139 idle timer service installed")'
assert out.count(anchor) == 1
insert = r"""

# ---- paclr140: a ticker bound to a secondary CPU owns the timer ----
_mr = (dst / "machine_routines.c").read_text()
_tick = r'''
extern void piano_watchdog_ping(void);
extern processor_t cpu_to_processor(int cpu);

static void
piano_simple_tick(void *param __unused, wait_result_t wr __unused)
{
	uint64_t frq, ctl, deadline;
	extern void rtclock_intr(unsigned int);
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	processor_t _p = cpu_to_processor(3);
	if (_p != PROCESSOR_NULL) { thread_bind(_p); }
	printf("piano-reset: bound ticker run thread=%p cpu=%u\n", (void *)current_thread(), (unsigned)current_processor()->cpu_id);
	for (;;) {
		__builtin_arm_wsr64("CNTV_TVAL_EL0", frq / 1000);
		__builtin_arm_wsr64("CNTV_CTL_EL0", 1);
		__builtin_arm_isb(ISB_SY);
		deadline = mach_absolute_time() + frq / 500;
		do { ctl = __builtin_arm_rsr64("CNTV_CTL_EL0"); } while (((ctl & 4u) == 0u) && (mach_absolute_time() < deadline));
		if (ctl & 4u) {
			extern uint64_t piano_gic_irqs;
			__builtin_arm_wsr64("CNTV_CTL_EL0", 2);
			__builtin_arm_isb(ISB_SY);
			piano_gic_irqs++;
			rtclock_intr(0);
		}
		piano_watchdog_ping();
		piano_reset_service_check();
	}
}
void
piano_simple_tick_decl(void)
{
}
'''
_mr = _mr.replace("static uint64_t piano_idle_log;", _tick + "\nstatic uint64_t piano_idle_log;", 1)
(dst / "machine_routines.c").write_text(_mr)
print("paclr140 bound ticker installed")
"""
out = out.replace(anchor, anchor + insert, 1)
pathlib.Path("/private/tmp/mkpaclr140.py").write_text(out)
print("wrote mkpaclr140.py")
