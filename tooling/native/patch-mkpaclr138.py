import pathlib, re
src = pathlib.Path("/private/tmp/mkpaclr137.py").read_text()
out = src.replace("piano-paclr137", "piano-paclr138")
assert out.count("piano-paclr138") == 1

before = out
out = re.sub(r"kernel_thread_start\(piano_reset_spin, NULL, &th\)", "((kern_return_t)KERN_SUCCESS)", out, count=1)
assert out != before
before = out
out = out.replace("if (th != NULL) {", "if (0 && th != NULL) {", 1)
assert out != before

anchor = '(dst / "machine_routines.c").write_text(_mr)'
assert out.count(anchor) == 1
helper = r"""

# ---- paclr138: public watchdog/deadline helper for the delay service ----
_mr = (dst / "machine_routines.c").read_text()
_mr += r'''
void
piano_reset_service_check(void)
{
	uint64_t now;
	static uint64_t last_ping;
	if (last_ping == 0 || (mach_absolute_time() - last_ping) >= 1000000000ull) {
		last_ping = mach_absolute_time();
		piano_watchdog_ping();
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	if (piano_reset_after_ns != 0 && now >= piano_reset_after_ns) {
		printf("piano-reset-after: %llu s reached, PSCI SYSTEM_RESET\n", piano_reset_after_ns / NSEC_PER_SEC);
		delay(200000);
		piano_psci_reset();
	}
}
'''
(dst / "machine_routines.c").write_text(_mr)
print("paclr138 reset service helper installed")
"""
out = out.replace(anchor, anchor + helper, 1)

old = 'extern void rtclock_intr(unsigned int);\n\tif (in_service) { return; }'
assert out.count(old) == 1, out.count(old)
out = out.replace(old, 'extern void rtclock_intr(unsigned int);\n\textern void piano_reset_service_check(void);\n\tif (in_service) { return; }', 1)
old2 = 'next = mach_absolute_time() + frq / 1000;\n\t\tpiano_delay_ticks++;'
assert out.count(old2) == 1, out.count(old2)
out = out.replace(old2, 'next = mach_absolute_time() + frq / 1000;\n\t\tpiano_delay_ticks++;\n\t\tpiano_reset_service_check();', 1)

pathlib.Path("/private/tmp/mkpaclr138.py").write_text(out)
print("wrote mkpaclr138.py")