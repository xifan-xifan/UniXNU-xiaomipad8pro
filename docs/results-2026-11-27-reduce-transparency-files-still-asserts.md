# 2026-11-27 16:36 (image 05a94b5c): Reduce transparency written root:wheel changes nothing; the backdrop assertion stands on 27.3

The question this run exists to answer: `docs/drivers/soft-metal.md` 11 stages Reduce transparency through the seed's boot job and
12 reports that on 26.6 that changed nothing, and this unit's seed has no boot job at all (`org.droidvm.run.0.plist`), so the
preferences went into the seed itself as `root:wheel` files (`PIANO_REDUCE_TRANSPARENCY=files`,
`tooling/k27/stage-soft-metal.py --reduce-transparency-files`). Staging reported all five domains, stored uid/gid 0/0 mode 644:

```
/Library/Managed Preferences/com.apple.universalaccess.plist
/Library/Managed Preferences/piano/com.apple.universalaccess.plist
/Library/Preferences/com.apple.universalaccess.plist
/Users/piano/Library/Preferences/com.apple.universalaccess.plist
/private/var/root/Library/Preferences/com.apple.universalaccess.plist
```

Image `05a94b5c0a75317d9ea0715af24652d648a74fbdabab49b148a51b4481f0fd86` (Mac `out/ram-verified-8748-sm/`,
Windows `out/k273-ram-8748-sm/`), run `logs/native-runs/ram-k273-rt-20261127-163542/`, booted 16:36:01, self-reset at 420 s,
HyperOS back on adb 16:44:54. Nothing written to the medium.

The answer is no, on 27.3 as on 26.6:

```
NativeSoftMetal: WindowServer: load: CA option 66 CA_DISABLE_FRAMEBUFFER_COMPRESSION set
NativeGopDisplay: first frame (swap 1); boot console released from the screen (at 147537 ms)
exec: pid 113 WindowServer -> Assertion failed: (r.update()->_backdrop_invalidations_finalized) … prepare_layers_roi, file ogl-layer.cpp, line 7151
                                    -> exit: pid 113 WindowServer status 0x6
exec: pid 279 WindowServer -> alive at the reset
exec: pid 168 universalaccessd … exit: pid 168 universalaccessd status 0x9 … exec: pid 297 universalaccessd
NativeGopDisplay: thumb frame 1: 3200x2136 BGRA, 0.0% non-black / thumb frame 4: … 0.0% non-black
```

`universalaccessd` runs, so the preference domain is live and still nothing about the panel changes: the first swap lands at
147.5 s (the 15:39 run's 154.4 s again), the compositor's frames carry no dirty region and are black, and the same single
WindowServer abort ends the boot-session server. That closes the lever: the assertion is not something a client preference
removes, which is what section 12 concluded and section 13 now measures on this unit and on 27.3. The CPU device stays where
section 7 puts it, and the desktop goes to the A830 path
([results-2026-11-27-turnip-inputs-are-public.md](results-2026-11-27-turnip-inputs-are-public.md)).

Restaging a seed is also why `Data store state 7 (kept) … seed cde8be03` repeats: every build that stages the plugin gives a new
seed, and the store guard holds the installed store while running Data volatile for that boot. Persistence needs the ESP and the
seed to be built together, and that is deliberately after a desktop exists.
