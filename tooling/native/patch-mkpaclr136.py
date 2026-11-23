import pathlib
src = pathlib.Path("/private/tmp/mkpaclr133.py").read_text()
out = src.replace('piano-paclr133', 'piano-paclr136')
assert out.count('piano-paclr136') == 1
old = 'if ((_ticks % 100u) == 0u) { thread_yield_to_preemption(); }'
new = ('if ((_ticks % 100u) == 0u) { printf("piano-reset: s136 yield %llu\\\\n", _ticks); '
       'thread_yield_to_preemption(); printf("piano-reset: s136 resume %llu\\\\n", _ticks); }')
assert out.count(old) == 1
out = out.replace(old, new, 1)
old2 = 'kernel_thread_start(piano_reset_spin, NULL, &th)'
assert out.count(old2) == 1
out = out.replace(old2, 'kernel_thread_start_priority(piano_reset_spin, NULL, 95, &th)', 1)
pathlib.Path("/private/tmp/mkpaclr136.py").write_text(out)
print("wrote mkpaclr136.py")