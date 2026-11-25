# Panic text on the linear scanout

`0073-k27-panic-text-on-linear-scanout.patch` adds a diagnostic channel to the
0069 panic-hold kernel with 0071's deliberate-panic control. It requires a rebuilt
kernel. It is inactive unless `piano-panic-text=1` and a panic hold is enabled.

The formatted panic output is copied before either console callback can touch a
stopped output device. A fixed 32 KiB capture preserves the beginning, and the
existing type 4/9 panic-hold entry renders the panic message, kernel addresses,
and first twelve backtrace frames using XNU's existing font at fivefold scale.
It writes only the already mapped 3200 x 2136 BGRA boot scanout. This diagnostic
must only be used with the copy display path: no DPU flips. It does not allocate,
map memory, acquire a lock, access a persistent store or call a service in panic.
The ordinary hold and reset continue after the drawing.

A camera positive control must read the deliberate-panic string and backtrace
frames before the channel is used to diagnose another panic. The host renderer
checks text selection and the exact scanout geometry with memory guards:

```
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tooling/k27/piano-panic-text/test_panic_text.c -o /tmp/piano-panic-text-test
/tmp/piano-panic-text-test
```

The portable core header here and the one carried by the patch must stay identical.
