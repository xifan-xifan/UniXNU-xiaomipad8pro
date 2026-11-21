# SystemOS explicit APFS checks (2026-11-22)

On the Mac, the retained unshimmed SystemOS image was attached read-only without mounting. The image-to-device lookup selected the attached store and its sole APFS volume. The volume UUID matched `284548A8-FC87-434E-A0C8-A4038E83704D` before invoking the checker on its raw device.

Both `fsck_apfs -n <raw-volume>` and `fsck_apfs -q <raw-volume>` exited 0. The full verification reported the expected UUID and the quick check reported a clean filesystem. The image detached successfully. SHA-256 before attachment and after detachment matched `c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d`.

This qualifies explicit offline read-only verification and quick checks on this SystemOS fixture. Boot-time device resolution, the launchd `-fy` repair request, and checks on mounted native volumes still require qualification.
