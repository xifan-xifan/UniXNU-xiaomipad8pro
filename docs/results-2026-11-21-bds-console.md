# BDS console markers (2026-11-21)

The diagnostic patch applied after the piano platform patch on a clean archive of Mu Basecore `cfb93724a06ff86ff5ba971413368a7809ee42a5`. The isolated UbuntuPiano DEBUG CLANGPDB build passed. All three UTF-16 markers are present in BdsDxe, and its exact executable bytes occur in the generated FVMAIN.

The diagnostic boot container reused the previous BootShim, DTB, and FAT payload. The pack step replaced the fixed-size FD and recalculated the Android v2 header ID after reproducing the old ID. The Ubuntu reference verifier passed with zero failures. The diagnostic payload offers restart and shutdown.

The build log and local build record are in `UbuntuPiano:/tmp/piano-bds-console.BEyee3/`. Screen output on the tablet remains pending. Record the last visible marker during the next temporary boot. If the screen stays blank, this test cannot distinguish an early stall from an unavailable console.
