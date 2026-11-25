# Owned real-size AVC fixtures

Leo Zhang encoded FFmpeg-generated testsrc2 patterns as progressive, 8-bit,
limited-range BT.709 AVC. high1080 is 1920×1080 High level4.0 (coded1088,
bottom crop8), main720 is 1280×720 Main level3.1. Both contain CABAC, B-frames,
60 frames at30fps and two closed GOPs. No downloaded/personal media is included.

The manifest records relative reproduction arguments, every compressed packet's
SHA256/DTS/PTS, independent Annex-B normalization identities and every software
decoded active NV12 picture's SHA256. These references precede tablet testing;
hardware output must never replace them. video-avc-audit reads actual CoreMedia
formats and compressed samples without creating any VT decoder session.
