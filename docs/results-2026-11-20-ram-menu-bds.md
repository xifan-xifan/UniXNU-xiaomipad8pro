# RAM diagnostic menu dispatch (2026-11-20)

The staged Mu Basecore BDS build initializes the console and starts the UniXNU application from the firmware volume that supplied BdsDxe. It supplies the `unixnu:firmware` load option used by the pinned UniXNU application. If the application cannot start or returns, BDS stops before processing variable-driven boot options.

The WSL build passed. The decompressed FD contains the generated FVMAIN and the exact rebuilt BdsDxe executable, including the firmware marker. The repacked Android boot container passed the Ubuntu reference verifier with zero failures. The FD, image and verifier log are in `/tmp/piano-ram-build.0YIaUj/menu-bds-final.ac77mrn7/` in UbuntuPiano. Original ABL behavior, console initialization and the menu launch on hardware remain unverified.
