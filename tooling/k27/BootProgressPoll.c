// The verified root build calls the optional boot-progress observer after every authenticated
// block, so unlike the development build (which never references it) NativeUfs has to link one:
// a weak reference is still an unresolved symbol to the collection link step. The source runtime
// archive carries a no-op definition, which is what NativeUfs.cpp says the weak declaration
// stands in for; this is that no-op, so the piano collection resolves the reference without
// inventing a poll provider. docs/results-2026-11-21-stock-policy-provider.md.
void boot_progress_poll (void) {}
