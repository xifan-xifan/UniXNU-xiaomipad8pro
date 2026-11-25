# Owned HEVC qualification fixtures

Leo Zhang encoded the project's generated V8 High1080 pattern as hvc1 Main10
and Main8. Each contains60 frames at30fps,1920×1080, limited BT.709, closed IDR
start, B pictures and WPP. Main10 exercises P010/x420; the source remains the
same ordered RGYBMC bars and moving diagonal/markers used by AVC optical review.
There is no third-party, personal or Apple media in these files.

Reproduce from this folder with FFmpeg/libx265 (encoder versions can change
compressed hashes):

```sh
for depth in 10 8; do
  if [ "$depth" = 10 ]; then fmt=yuv420p10le; profile=main10; name=main10; else fmt=yuv420p; profile=main; name=main8; fi
  ffmpeg -nostdin -y -threads 2 -i ../v8/high1080.mp4 -map 0:v:0 -an -frames:v 60 \
    -vf "format=$fmt" -c:v libx265 -preset fast -crf 18 -pix_fmt "$fmt" -profile:v "$profile" \
    -x265-params 'pools=2:frame-threads=2:wpp=1:keyint=60:min-keyint=60:scenecut=0:open-gop=0:bframes=2:b-adapt=0:repeat-headers=0:info=0:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited' \
    -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -tag:v hvc1 \
    -video_track_timescale 90000 -movflags +faststart "$name-1080.mp4"
done
```

The manifest pins complete movie hashes. Each reference records hvcC facts,
every compressed packet's SHA/PTS/DTS and independent Annex-B normalization.
`tests/hevc_files_host.py` audits real file samples through the shared parser;
`tooling/native/video-avc-audit --all` also checks actual CoreMedia formats and
sample identities without creating a decoder. These are host admission checks,
not hardware or stock-player qualification.
