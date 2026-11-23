import pathlib
src = pathlib.Path("/private/tmp/mkpaclr141.py").read_text()
out = src.replace("piano-paclr141", "piano-paclr142")
assert out.count("piano-paclr142") == 1

# ufs142: keep the delay/idle rtclock service that ufs140 removed and let the
# bound ticker act as a backstop for long stretches without delay() or idle.
old = "/* ufs140: the bound ticker owns rtclock */"
assert out.count(old) == 4, out.count(old)
out = out.replace(old, "piano_gic_irqs++; rtclock_intr(0);")

pathlib.Path("/private/tmp/mkpaclr142.py").write_text(out)
print("wrote mkpaclr142.py")
