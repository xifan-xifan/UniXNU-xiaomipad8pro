import pathlib
src = pathlib.Path("/private/tmp/mkpaclr140.py").read_text()
out = src.replace("piano-paclr140", "piano-paclr141")
assert out.count("piano-paclr141") == 1

# ufs140 halted inside rtclock_intr after "s126 sched stat ok": the ticker
# prints while the interrupted boot thread may hold the console lock. Start
# the ufs126 probe counters past their print window so irq context stays quiet.
old = "static unsigned s126_n;"
assert out.count(old) == 2, out.count(old)
out = out.replace(old, "static unsigned s126_n = 2; /* ufs141: irq context stays quiet */")

pathlib.Path("/private/tmp/mkpaclr141.py").write_text(out)
print("wrote mkpaclr141.py")
