# B2 full-duplex audio: static analysis and RAM measurement plan

2026-11-27. Target: Xiaomi Pad 8 Pro (`piano`, SM8750). Source baseline:
`d2ec099f0bfab762002187eebbadeb7f04bb4be8`, branch
`codex/b2-audio-duplex-ana`. Analysis only: no driver changes, image build,
tablet access, deployment or push. The protocol below proposes a later attended
RAM session; it does not report measurements performed by this task.

The reported failure is occasional playback dips/stutters when microphone
capture starts or runs alongside playback. Independent functional passes do
not close sustained duplex continuity. The
[calibrated AVFoundation result](../results-2026-11-27.md#calibrated-microphone-and-c920-playback)
used a **−36 dBFS stimulus**, not a −36 dBFS recorded microphone level. Its
4-second recording included a brief speaker stimulus; the measured microphone
tone window was −12.90 dBFS RMS, −4.55 dBFS peak, with zero clipped samples.
That provides limited acoustic overlap evidence, not a long duplex stress or
input-open transition test against uninterrupted output. The
[installed idle baseline](../results-2026-11-27.md#audio-duplex-baseline)
reports `timing=0`, protection request 3, MIPLAY request 1, gain 243 and healthy
idle GLINK. It explicitly leaves the active concurrent ledger pending.

**Finding:** input startup does not explicitly restart Speaker in our code.
Apple HAL/VirtualAudio may reconfigure its duplex route; that behavior remains
unobserved for the failing window. There is a concrete shared-worker stall
exposure at VA startup, despite separate host engine threads and buffers.
Signed sound age and the default `period240` speaker burst reserve are already
implemented. The optional 480-frame host batching path retains a different,
insufficient reserve policy. None of these source facts identifies the current
audible failure without synchronized active evidence.

## Evidence boundaries and reference pin

Source citations use repo-relative `file:symbol` references at the source
baseline. To keep the tables readable:

- `A/` expands to `package/PianoAudio/`.
- `N/` expands to `package/PianoMicNS/`.
- `P/` expands to `package/PianoPower/`.
- `K/` denotes a path relative to the **Linux** repository at
  `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`.

The Linux comparison was read using `git show` at that exact commit, including
the board DTS, PCM, GPR, GLINK, VA and FS19xx sources. The reference checkout's
HEAD differs; it is not substituted for the pin. Linux is a behavioral
specification; no GPL implementation is copied.

[audio.md](audio.md), especially §5, “Capture FIFO startup and clock
reconciliation,” “Speaker output epoch startup,” and §§12–13, documents the
IOAudio2 ABI, recent FIFO policies, protection, MIPLAY and clock-hold experiment.
Its older topology/kernel pins and staged bring-up text describe their own
revisions. Current source resolves differences: the DSP transport exists,
Mic is mono at the HAL, the shared worker sleeps 5 ms while running, and the
default reader requires current-epoch PCM before unmute. Historical narrative
or a passing host assertion is not installed acceptance.

## Paths, clocks and period240

Playback, in data order:

```text
app -> coreaudiod / Apple IOAudio2 + VirtualAudio
    -> Speaker mapped stereo S16 ring -> piano-audio-out timed thread
    -> AudioLive stereo FIFO -> safety/DC/gain/fade -> quad S16 DMA slots
    -> GPR WRITE V2 -> stream 0x4001 -> [optional MIPLAY 0xb000019f]
    -> speaker 0xb0000002: MFC -> SP_RX -> splitter -> LOG -> TDM sink
    -> secondary TDM RX, lane 1 -> four FS19xx amplifiers

protection feedback: amplifiers -> secondary TDM TX, lane 0
    -> V/I 0xb00000d5: TDM source -> LOG -> splitter -> SP_TX
    <-> SP_RX control link (not microphone samples)
```

`A/AudioSpkProt.c:spk_open_payload`, `spk_mip_open_payload`, `spk_plan_init`
and `A/AudioSpkProt.h:SPK_SG_DEV`, `SPK_SG_VI`, `SPK_SG_MIP` define these
graphs. MIPLAY contains SYNC/SAL, loggers, MFCs, Dolby DAP, MiSound/rotation
and volume modules; it runs only inside the protection plan. In the normal
plan ordering, speaker/SP_RX and stream are opened before V/I is started;
V/I starts before speaker and stream. The header's older “opened first”
description is not the order of every supported variant. Protection request
3 selects endpoint-owned clocks, omitting the CPU PRM TDM-bit-clock request
(`SPK_VAR_ENDPOINT_CLOCK`). An unprotected fallback uses device `0x4005`
from `A/PianoAudioReach.c:ar_open_speaker_device`, not `0xb0000002`.

Capture, in data order:

```text
DMIC pins + VA pair clocks -> VA DEC0 / DEC1
    -> VA TX0 codec DMA -> mic graph 0x4008 -> capture graph 0x4003
    -> LOG / MFC / converter / encoder / RD_SHMEM -> GPR READ V2 completions
    -> shared worker: validate + stereo-to-mono + startup zeros
    -> mono FIFO -> piano-audio-in timed thread -> Mic mapped ring -> app
    -> alternatively, raw mono tap -> PianoMicNS RNNoise/FIFO -> HAL -> app
```

See `A/PianoAudioReach.c:ar_open_mic_device`, `ar_open_capture_stream`,
`ar_config_mic_device`, `ar_config_capture_stream`;
`A/AudioLive.c:capture_period`; `A/AudioTap.h:audio_tap_write`.
Legacy routing averages DEC0 and DEC1; other supported routes select DEC0.
The tap branches from the worker before input-FIFO clock slips and digital
level; it carries level/mute metadata that PianoMicNS subsequently applies.
It can keep capture alive after the raw Mic device stops
(`A/AudioLive.c:mic_wanted`).

| Geometry / clock | Source and derived budget |
| --- | --- |
| Host mapped rings | `A/AudioTiming.h:piano_audio_timing_profile`: 48 kHz, 7680 frames / 160 ms modulus and zero-timestamp period in both modes; stereo output 30720 bytes, mono input 15360 bytes. Capacity is not guaranteed producer lead. |
| `period240` | Mode 0: host engine takes 240 frames / 5 ms. Mode 1: 480 / 10 ms, latched independently at each StartIO. Neither changes HAL's 7680-frame ring modulus; neither fixes the app callback size. |
| DSP periods | `A/AudioSession.h:AUDIO_PERIOD_FRAMES`, `AUDIO_SPK_PERIOD_BYTES`, `AUDIO_MIC_PERIOD_BYTES`: always 480 frames / 10 ms; playback 4-channel S16 = 3840 bytes; capture 2-channel S16 = 1920 bytes. Two host batches normally supply one playback DSP period. |
| DSP slots / queue | Eight slots per direction, four outstanding (`AUDIO_SPK_SLOTS`, `AUDIO_MIC_SLOTS`, `A/AudioLive.h:AUDIO_LIVE_DEPTH`, `AUDIO_LIVE_MIC_DEPTH`). Storage is 80 ms per direction, but at most 40 ms is posted; completion arrival does not reveal remaining hardware lead. |
| Software FIFOs | `A/AudioLive.h`: output 4096 stereo frames / 85.33 ms, target 960 / 20 ms; input 8192 mono / 170.67 ms, center 1920 / 40 ms. Input center = 960 reserve + 240 deadband + 720 completion-batch bias. Nominal usable trough is 960 / 20 ms. |
| Published latency | `A/PianoAudioDevice.cpp:PianoAudioDevice::publish` adds engine latency 240 to transport latency: output 240 + 1920 + 960 = 3120 / 65 ms; input 240 + 480 + 1920 = 2640 / 55 ms. Safety offset 480 / 10 ms is separately published. These are software declarations, not measured acoustic latency. |
| Host / DSP clocks | `PianoAudioDevice::run` advances on mach absolute time; DSP endpoint clocks are independent and reconciled with counted single-frame drops/repeats (`A/AudioLive.c:drift_update`, `take_period`, `capture_period`). Same nominal 48 kHz and raw devices' clock domain 1 do not phase-lock ADSP and APPS. |
| DMA address separation | `A/AudioSession.h`: speaker offset `0x00000`, 30720 bytes; parameters `0x08000`–`0x0ffff`; mic `0x10000`, 15360 bytes; total slice `0x20000`. `A/AudioBoard.cpp:dmaSetup`, `publish` map write-combined and publish with a barrier. `A/AudioSpkProt.c:spk_plan_init` bounds the speaker/parameter region; capture has its own map/pump. |

The pinned board description,
`K/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts:/sound`,
uses `SECONDARY_TDM_RX_0` with four 32-bit slots and `VA_CODEC_DMA_TX_0`
with `lpass_vamacro`. Its `&lpass_vamacro` sets 2.4 MHz DMIC sampling;
its TDM backend requests the uncoupled bit clock. The speaker clock is
48000 × 4 × 32 = 6.144 MHz. `A/AudioSequence.c:VA_STEPS` requests
LPASS macro/digital-codec votes and TX-core MCLK 19.2 MHz for VA.

Speaker output and V/I share the TDM interface/clock; VA microphones use
separate LPI DMIC pins and pair clocks. Pin names containing `i2s1` do not
make the VA path an I2S microphone. Neither the active board links nor our
graph builders use SLIMbus or a SoundWire microphone endpoint. Generic
VA codec SoundWire options do not establish use by this route. LPASS roots,
PRM/ADSP execution, memory bandwidth and firmware resource management remain
shared; transient vote/clock interactions are `unknown`. The Linux DTS
explicitly lacks board speaker protection and does not establish concurrent
qualification. Its topology is externally loaded; the kernel pin alone does
not establish running Linux graph IDs or live `hw_params`.

## Startup, teardown and interference analysis

### HAL lifecycle versus cold-start silence

This driver implements an IOAudio2-compatible user-client ABI on `IOService`,
not an `IOAudioEngine` subclass. There is no audio `IOWorkLoop`, command gate
or `IOTimerEventSource` in the inspected PianoAudio implementation. There is
one timed kernel thread and one lock per physical device, plus a separate
AudioReach worker (`A/PianoAudioDevice.cpp:initRole`, `startIO`, `run`;
`A/PianoAudio.cpp:PianoAudio::thread`; `A/AudioDsp.cpp:liveRun`).

Input StartIO calls `transportSet(false, true)`, preserving existing output
intent under `gTransportLock`. `A/AudioLive.c:audio_live_running` changes output
epoch only on false-to-true output intent. Opening or closing Mic alone
neither zeros Speaker's mapped ring nor increments its epoch. Duplicate
Speaker StartIO while already running is also a no-op. Our code does not
require an output restart when input opens.

If HAL calls Speaker StopIO followed by StartIO, however,
`PianoAudioDevice::startIO` zeros that ring, resets sample time/writeEnd,
increments seed and reanchors its host clock. `audio_live_running` publishes
a new output epoch; `audio_live_tick` discards older-epoch FIFO data and clears
priming. Even if the DSP graph remains warm, output must re-prime and run
through the fade/mute state machine. A HAL restart can create a gap without
a DSP failure. A graph restart additionally repeats local amp startup,
silent DSP periods and graph commands.

`A/AudioLive.c:stream_begin`, `spk_stream_tick`, `spk_has_prefill` require
four silent returns and accepted nonzero PCM from the current epoch. Default
mode requires 2880 frames before unmute: one four-slot burst plus a 960-frame
reserve. `A/AudioSafety.c:audio_chain_restart`, `audio_chain_fade` use a
960-frame / 20 ms fade. Silence-only StartIO does not power the speakers;
PCM offered while the graph is not accepting is dropped/counted. Expected
cold-start silence is not a steady-state underrun.

Apple's current `coreaudiod`/IOA2/VirtualAudio implementation and app-specific
aggregate/VPIO route behavior are outside these sources. ABI comments and
[audio.md](audio.md) support the design, not a fresh binary trace. Registry
snapshots can miss short restarts. The decisive witness is an **unexpected
output stop/start, output seed and epoch change** around input open, paired
with audio timestamps and the acoustic envelope. Exclude sleep reanchor
(`piano_audio_devices_power`) as a separate seed-change cause. Graph-start
counters alone cannot detect a warm HAL restart.

### Thread ownership, locks and GLINK/GPR serialization

`PianoAudioDevice::run` releases its device lock before `clock_delay_until`
and reacquires it before copying PCM and calling the transport. Speaker/Mic
locks are separately allocated. `gTransportLock` serializes combined
StartIO/StopIO intent; normal `play`/`capture` do not acquire it. `gWakeLock`
guards a wake flag and condition sleep; sleeping releases the lock. There is
no evidence of a shared speaker/mic device-lock deadlock.

The shared execution owner is `A/AudioDsp.cpp:liveRun`: health read -> inbox
drain -> `audio_live_tick` -> registry work -> relative sleep. `audio_live_tick`
services speaker, mic, then clock hold; running paths return 5 ms, transitions
2 ms, both down 2000 ms. Work time, scheduler delay and sleep are additive.
`onReceive` only copies a packet into the 16 × 512-byte SPSC inbox
(`A/AudioDspLink.c:audio_inbox_put`); **it does not wake this worker**.
Completion drain includes capture DMA loads, mixing, statistics and optional
tap/diagnostic copies before the next refill.

`A/AudioLive.c:others_busy`, `plan_send` permit one outstanding control command
across speaker, microphone, protection monitor and hold plans. A 2000 ms
answer deadline is asynchronous state-machine time, not a mutex held while
waiting. PCM READ/WRITE loops in `spk_stream_tick` and `capture_tick` do not
use `others_busy`; they can enqueue while a control answer is pending.
`A/AudioSequence.c:ar_seq_next` executes **local actions synchronously**,
blocking both pumps even when they sleep rather than consume CPU.

`A/AudioDsp.cpp:sessionSend` enqueues on PianoPower's `adsp_apps` channel.
`P/PianoAdsp.cpp:piano_adsp_send_keep` holds `gClientLock` around a bounded
message copy/count update. `pumpSends` releases that mutex before GLINK send,
serving one message per channel per round; it does not prioritize speaker
refills over microphone/monitor packets within their common audio queue.
That queue has 16 messages. Other channels, remote intents, the physical TX
FIFO and the ADSP owner are shared with charger/sensor traffic. Queue refusal
causes counted retry; successful enqueue does not prove remote consumption.
No lock here spans an audio command's round trip.

`P/PianoAdsp.cpp:waitEvents`, `replyPollSlice`, `serviceGlink` poll doorbells:
20 ms steady slices, 1 ms for bounded recent control-reply waits. This differs
from the audio worker's 5 ms sleep. Sends wake the DSP owner, but there is no
demonstrated audio completion IRQ-to-worker deadline guarantee. A 20 ms poll
cadence can batch 10 ms completions; it does not prove DSP consumption stopped.
Host `in_flight` includes consumed buffers whose DONE is not yet processed.

| Shared-worker occupancy | Exact source, bound and implication |
| --- | --- |
| VA power-up while speaker plays | `A/AudioVa.c:decimator_up`, `audio_va_on` call 1 + optional 1 + 1 + 6 ms sleeps for each of two decimators: **16–18 ms requested sleeps**, plus mapping/MMIO/logging and scheduler delay. `A/AudioBoard.cpp:vaSleep` uses `IOSleep`. The suggested 16–21 ms interval is a measurement hypothesis; 21 ms is not a source-proven upper bound. |
| Amp startup while capture runs | `A/AudioAmps.c:audio_amps_start_capped` calls `A/Fs19xx.c:fs19xx_start` sequentially: four × 10 ms sleeps = at least 40 ms requested sleep plus I2C. Capture completion processing/reposting pauses during this local action. |
| Amp shutdown while capture runs | `audio_amps_off` -> `fs19xx_shutdown`: four × 35 ms = at least 140 ms requested sleep plus I2C. Speaker idle/fallback teardown can exceed input's nominal 20 ms trough and four posted READ periods. This is a transition exposure, not proof of recurring steady dips. |
| Amp status/gain service | `A/AudioLive.c:spk_stream_tick`, `status_sample`; `A/AudioAmps.c:audio_amps_poll`: refill first, then one register read or one amp gain write per pass. A full group has 24 reads, now spread across passes; diagnosing a current monolithic 24-read stall would be incorrect. Default status interval is 500 ms; actual bus/stretch/service cost is unmeasured. |
| Protection/VBAT monitor | `A/AudioLive.c:mon_tick`, `live_report`; `A/AudioSpkProt.c:spk_monitor_plan`: every 2000 ms, battery registry lookup, optional DAP pregain SET, VBAT SET and live-data GET; first monitor starts immediately after stream-up. Waits are asynchronous; lookup, staging, parsing and four log lines occupy the worker. Monitor parameters do not overlap PCM. |
| Logging / health / publication | `A/AudioDsp.cpp:micHalReport`, `dspCapture`, `liveRun`; `A/AudioLive.c:audio_live_mic_report`: synchronous input-engine reports about every second under its device lock, worker reports about every second, registry updates, health lock and completion mixing. Bounds/correlation are missing. Local mutexes, global logging resources or preceding iterations can delay service. |

Four queued DSP periods provide at most 40 ms of PCM, not a 40 ms scheduling
guarantee. A 16–18 ms VA pause after refill may fit; after delayed completion
delivery with little actual lead, it may not. `A/AudioSession.h:AUDIO_LATE_MS`
is 250 ms: a shorter audible starvation can escape the safety timeout. Buffer
capacity, nominal reserve and the late-stop limit must not be conflated.

### False silence, reserve policy and late input wakes

The worker snapshots `now` **before** draining the inbox. Concurrently,
`A/AudioLive.c:audio_live_play` publishes a newer `sound_at`. With `now=1000`
and `sound_at=1001`, unsigned subtraction wraps to 4294967295 ms and can
classify valid output as idle, starting our own 20 ms fade. Capture completion
processing widens that race window. `sound_old` now uses signed age by default
(`audio_live_init`); `machdep.piano_audio_age=0` explicitly selects the old
control. Ahead stamps are harmless under the signed policy for these bounded
idle intervals. Healthy PCM counters cannot exclude a fade created after FIFO
read. Check `duplex-obs1` ahead/fades/as-silent alongside actual age policy;
do not re-propose an already present signed-age fix as missing.

`A/AudioLive.c:spk_epoch_lead`, `spk_prefill`, `take_period` distinguish the
two host batching modes. Default 240-frame mode retains pending refill burst
plus 960-frame reserve, with a minimum 2880-frame trim threshold. The
480-frame experiment retains a 2400-frame threshold and trims to 960. With
2880 frames available and four free 480-frame slots, that mode discards 1920,
fills two slots, then short-reads two: **960 missing frames / 20 ms**. Its
960-frame initial prefill is also smaller than a four-slot burst. Existing
`A/tests/duplex_schedule_host.c:main` exercises production `take_period` and
models alternative engine delays; it is a synthetic schedule, not a recorded
hardware completion trace. The live signature must include actual mode 1,
a new skip and shortages in that refill batch. Idle `timing=0` readback cannot
establish a previously started engine's latched mode.

This analysis reran the existing host model: 1,871,588 checks passed, including
mode 0 missing 0 versus mode 1 missing 960 frames in the four-slot case.
Synthetic wait-return, lock-acquisition and previous-body delays also produced
identical late-wake maxima with continuous capture PCM. This validates the
source-level counterexamples and measurement ambiguity, not hardware timing.

For batch B, `PianoAudioDevice::run` uses deadline
`D = startTime + (done+B)/48000`; it measures `W` after wait **and lock
reacquisition**. `late-periods` counts `W-D > B/48000`, not every late wake.
At period240 an 8 ms late pass counts; at period480 the same delay does not.
An overdue loop catches up one batch at a time, advancing `done` by B without
skipping frames. FIFO reserve can keep samples intact despite late input wakes;
a late mapped-ring write can still miss an app read deadline.

The current body is outside that iteration's wake measurement, but its cost
can make the next deadline late. Plain `kernel_thread_start` supplies no
explicit audio time-constraint policy here. Existing totals cannot separate
timer expiry, scheduler delay, same-device lock acquisition and previous-body
logging/copy cost. The global `piano_audio_dev_late` is checked before the wait,
shared by both engines and updated non-atomically; it is a kernel global, **not
an exported sysctl**. Use per-direction `HAL wake` stop reports, recognizing
that they provide totals/maxima rather than a per-event timeline.

### PianoMicNS processing and timestamps

`N/PianoMicNS.c:StartIO` opens/maps the read-only tap and initializes RNNoise
on the first user, under `gLock`; last-user `StopIO` reports, frees and closes
it. This can start capture even if the raw Mic engine is stopped. Startup
allocations occur outside `DoIOOperation`, but their impact on concurrent HAL
route setup is unmeasured. The plug-in has no output device or direct Speaker
stop call.

`N/MicNsCore.h:MICNS_BLOCK`, `MICNS_RESERVE` specify 480-frame / 10 ms
RNNoise blocks and 2400-frame / 50 ms reserve beyond a read. First reads
prime; underrun causes silence and re-priming. Changed tap runs, lapped readers
or excessive backlog can reanchor/trim, with counters. Processed FIFO and
sample-time output ring each hold 16384 frames / 341.33 ms. Advertised
latency is `kLatency = 2400 + 2*480 = 3360` frames / 70 ms, safety offset 0;
stream latency 0. It is a declaration, not a measured total from DMIC to app,
and should not simply be added to raw Mic's FIFO latency because the tap
bypasses that FIFO. RNNoise processing/lookahead, batching and priming
contribute startup behavior.

`N/PianoMicNS.c:GetZeroTimeStamp` derives wraps from the first StartIO host
anchor, 16384-frame zero period and mach timebase; seed increments on
first-user starts. It does not derive timestamps from READ_DONE or measured
ADSP progress. `propertyData` advertises nominal 48 kHz, stable/raw host clock
and **clock domain 0**, unlike physical Speaker/Mic's domain 1. Matching nominal
rates do not prove HAL treats them as one clock domain.
`produce`/`DoIOOperation` cache output by input sample time for repeated reads.

The core's `micns_read`, `pull`, `process_block` allocate no memory or mutex
after startup. `N/MicNsCore.c:new_state` primes a silent frame at startup;
`N/third_party/rnnoise/src/denoise.c:rnnoise_init`, `check_init` allocate recurrent
state and initialize FFT tables there, before `rnnoise_process_frame` is used
for captured frames. `pull` snapshots unread tap frames instead of chasing a
moving writer. Work remains bursty: several RNNoise blocks can execute in one HAL
callback, and `produce` can catch up more than one chunk. Each block's average
below 10 ms alone would not prove a 5 ms or other app callback deadline.
**The complete callback is not strictly free of logging:** `DoIOOperation`
calls `report` after roughly 2 seconds, then every 30 seconds; statistics/logging
run on that callback. `read-us` measures `micns_read`, excluding reporting and
other plug-in work. `gCore`/output-ring production is also shared mutable state
without a callback mutex; concurrent-client correctness depends on HAL
serialization, which this source does not establish. Cached repeated-time
reads under serialized calls do not prove arbitrary parallel callback safety.

RNNoise runs in the HAL driver service's capture callback, not the kernel
speaker engine. CPU/HAL scheduling interference is plausible; there is no
direct DSP output-lock dependency. Compare explicit raw Mic and PianoMicNS
routes in one boot, with VPIO/AGC/echo cancellation disabled. Intentional NS
tone attenuation is not transport loss. Raw-route stutter does not require
RNNoise as its cause.

## Pinned Linux comparison

| Reference at `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` | Comparison and limit |
| --- | --- |
| `K/sound/soc/qcom/qdsp6/q6apm-dai.c:q6apm_dai_open`, `q6apm_dai_prepare`, `q6apm_dai_trigger`, `event_handler` | Per-substream runtime/graph state. ALSA period/buffer steps are 480 frames; capture prequeues periods at trigger and reposts on READ_DONE; playback notifies on WRITE_DONE and queues producer periods in `q6apm_dai_ack`. This differs from our host timer plus shared-worker refill. Our period240 is a host batch, not a forbidden 240-frame DSP request. |
| `q6apm_dai_hardware_capture`, `CAPTURE_MIN_PERIOD_SIZE` in that file | Capture minimum is 6144 **bytes**, 2–8 periods, 2–4 channels, S16/S24. At stereo S16, 1536 frames is the byte floor; the 480-frame step makes 1920 frames / 40 ms the smallest aligned period. Our 1920-byte / 480-frame READ is smaller than that Linux host constraint; source alone cannot turn it into a universal firmware minimum. Live Ubuntu period selection is not established. |
| `K/sound/soc/qcom/qdsp6/q6apm.c:q6apm_write_async`, `q6apm_read`, `graph_callback` | Per-graph mutex protects fragment indices, released before asynchronous GPR send; callbacks update hardware pointer and deliver period events. Separate graphs still share the packet router. |
| `K/sound/soc/qcom/qdsp6/audioreach.c:audioreach_send_cmd_sync`, `audioreach_graph_send_cmd_sync`; `K/sound/soc/qcom/qdsp6/q6prm.c:q6prm_send_cmd_sync` | Synchronous waits up to 5 seconds under the applicable APM/PRM/graph mutex. Linux is not globally lock-free; separate command/runtime contexts do not validate our worker's occupancy budget. |
| `K/drivers/soc/qcom/apr.c:apr_callback`, `apr_rxwq`, `gpr_do_rx_callback`, `pkt_router_send_svc_pkt` | RX queues a single-thread workqueue and invokes graph callbacks outside the service-list lock; TX uses a per-service lock and `rpmsg_trysend`. Our RX callback only copies, with no immediate audio-worker wake. |
| `K/drivers/rpmsg/qcom_glink_native.c:qcom_glink_tx`, `qcom_glink_trysend` | Shared TX spinlock protects FIFO writes; blocking waits drop it. Linux also shares FIFO/intents, so sharing alone does not explain why one platform stutters. |
| `K/sound/soc/codecs/lpass-va-macro.c:va_macro_enable_dec`, `va_dmic_clk_enable`, `va_macro_runtime_resume` | Same decimator/HPF/DMIC sequencing class, including 1/1/1/6 ms waits per decimator and runtime clock management. Linux DAPM operations are not inline local actions in `apr_rxwq`; ours run on the common audio worker. |
| `K/sound/soc/codecs/fs19xx.c:fs19xx_start_work`, `fs19xx_dai_trigger`, `fs19xx_start_up`, `fs19xx_shut_down` | Per-amp mutex and delayed startup work, 10–11 ms startup and 35 ms shutdown sleep. Our common audio worker executes four starts/shutdowns sequentially. |
| `K/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts:/sound`, `&q6apmbedai`, `&lpass_vamacro`; `K/sound/soc/qcom/sc8280xp.c:sc8280xp_tdm_hw_params` | Relevant board endpoints, TDM slot geometry and VA clocks agree. No board V/I/MIPLAY concurrent performance evidence is provided. Generic `audioreach_speaker_protection*` helpers do not prove those graphs run on this board. |

## Path & Check Matrix

`ours-OK` means the stated static invariant exists; it does **not** mean duplex
hardware acceptance. `ours-missing` identifies an absent protection/observation.
`unknown` identifies unestablished runtime duration, firmware or external
behavior. Shared ownership is explicit instead of being a fourth status.

| Stage / check | Status | Exact file/symbol evidence and remaining discriminator |
| --- | --- | --- |
| App route and HAL aggregate reconfiguration on input open/close | `unknown` | `A/PianoAudioDevice.cpp` ABI/VirtualAudio contract; `N/PianoMicNS.c:propertyData`. Need device UIDs, callbacks/seed and app timestamp/route events. |
| Independent physical device allocation, lock and ring | `ours-OK` | `A/PianoAudioDevice.cpp:PianoAudioDevice::initRole`, `piano_audio_devices_attach`. Separate Speaker/Mic buffers, threads and locks. |
| Start/stop selectors preserve other direction | `ours-OK` | `PianoAudioUserClient::externalMethod` selectors 0/10, 1/11; `transportSet`; `A/AudioLive.c:audio_live_running`. |
| Output reset witness / restart-induced re-prime | `ours-OK` | `PianoAudioDevice::startIO`, `run`; `A/AudioLive.c:audio_live_tick`, `spk_has_prefill`: seed, epoch and ledger/events exist. Need failing-window observations. |
| Raw clock declarations and ring modulus | `ours-OK` | `A/AudioTiming.h:piano_audio_timing_profile`; `PianoAudioDevice::publish`, `run`: fixed 7680 frames, mach-clock anchors, domain 1. Actual HAL domain handling remains above this stage. |
| Audio engine deadline policy | `ours-missing` | `PianoAudioDevice::startIO`, `run`: plain kernel thread and deadline wait; no explicit audio deadline reservation. |
| Engine wait/lock/body latency separation | `ours-missing` | `PianoAudioDevice::run` samples wake after lock; `HAL wake` has only totals/max. No exported per-boundary timeline. |
| HAL write lead at an audible dip | `unknown` | `PianoAudioDevice::run`; `A/AudioDsp.cpp:piano_audio_producer_trace`: `writeEnd` snapshots exist, but no continuous upstream PCM/timestamp trace here. |
| Stereo output FIFO publication | `ours-OK` | `A/AudioLive.c:audio_live_play`; `A/AudioSafety.c:audio_ring_write`, `audio_ring_read`: SPSC publication, drops/short reads counted. |
| Signed sound age by default | `ours-OK` | `A/AudioLive.c:sound_old`, `spk_active`, `audio_live_init`; `A/AudioDsp.cpp:ageControl`: old policy explicit; actual runtime choice must be read. |
| Cold/warm period240 epoch PCM and burst reserve | `ours-OK` | `A/AudioLive.c:spk_prefill`, `spk_has_prefill`, `take_period`, `spk_stream_tick`: 2880-frame arm and burst + 960 retain. |
| Equivalent reserve for optional batch480 | `ours-missing` | Same functions, gated by `spk_epoch_lead`; `A/tests/duplex_schedule_host.c:main`: four-slot trim counterexample. |
| Safety conversion, ceiling and fades | `ours-OK` | `A/AudioLive.c:fill_period`; `A/AudioSafety.c:audio_stereo_to_quad`, `audio_chain_run`, `audio_chain_fade`: intentional fades can coexist with healthy transport counts. |
| Speaker/V/I/MIPLAY topology and rollback | `ours-OK` | `A/AudioSpkProt.c:spk_open_payload`, `spk_mip_open_payload`, `spk_plan_init`; `A/AudioSequence.c:ar_seq_next`: explicit graphs, reverse undo, variant-dependent order. |
| Protection/MIPLAY active during failure | `unknown` | `A/AudioDsp.cpp:spkState`; `A/AudioLive.c:prot_choose`, `mip_choose`, `mip_report`: requests/readiness alone do not identify active graphs/fallbacks. |
| VBAT/live-data/pregain monitor isolation | `ours-OK` | `A/AudioLive.c:mon_tick`; `A/AudioSpkProt.c:spk_monitor_plan`, `spk_plan_init`: separate parameters and asynchronous answers; worker cost remains unmeasured. |
| Shared-worker worst-case occupancy bound | `ours-missing` | `A/AudioDsp.cpp:liveRun`; `A/AudioSequence.c:ar_seq_next`: synchronous VA/amp actions, logging, relative sleeps; tick max excludes inbox drain/sleep. |
| Refill-first sliced amplifier status | `ours-OK` | `A/AudioLive.c:spk_stream_tick`, `status_sample`; `A/AudioAmps.c:audio_amps_poll`: one field per pass. Bus duration is `unknown`. |
| Amp transitions isolated from capture repost | `ours-missing` | `A/AudioAmps.c:audio_amps_start_capped`, `audio_amps_off`; `A/Fs19xx.c:fs19xx_start`, `fs19xx_shutdown`: common-worker sleeps block both pumps. |
| Control serialization separate from PCM pumps | `ours-OK` | `A/AudioLive.c:others_busy`, `plan_send`, `spk_stream_tick`, `capture_tick`: PCM independent of control-answer gate. |
| Completion inbox bounds / copy-only receive | `ours-OK` | `A/AudioDsp.cpp:onReceive`; `A/AudioDspLink.c:audio_inbox_put`, `audio_inbox_get`: 16 × 512 bytes with overflow/oversize counts. |
| Completion wake and live inbox-loss export | `ours-missing` | `onReceive` does not call `liveWake`; no steady live sysctl exports `gInbox.dropped`/`too_long`. Do not invent an inbox sysctl. |
| TX enqueue lock released before GLINK send | `ours-OK` | `P/PianoAdsp.cpp:piano_adsp_send_keep`, `pumpSends`: queue mutex around copy/count, send outside it; round-robin across channels. |
| TX FIFO / intents / owner saturation in duplex | `unknown` | `P/PianoAdsp.cpp:pumpSends`, `serviceGlink`; `P/AdspObserveKernel.h:observeGlink`, `observeAdsp`: passive snapshots conditional, no per-audio-message residence timeline. |
| Audio completion IRQ dispatch | `ours-missing` | `P/PianoAdsp.cpp:waitEvents`, `replyPollSlice`; `A/AudioDsp.cpp:onReceive`, `liveSleep`: polled path and timed worker. |
| Direction-specific DMA/map/pump geometry | `ours-OK` | `A/AudioSession.h:AUDIO_DMA_*`; `A/AudioBoard.cpp:dmaSetup`, `publish`; `A/AudioSequence.c:ar_pump_init`, `ar_pump_init_capture`: disjoint slots, four in flight. |
| Buffer ownership / READ_DONE integrity | `ours-OK` | `A/AudioSequence.c:ar_pump_done`; `A/PianoAudioReach.c:ar_parse`; `A/AudioLive.c:audio_live_receive`: token, handle, origin, address/range and alignment checks before copy/reuse. |
| Firmware DMA underrun, cache visibility, graph budget | `unknown` | `A/AudioBoard.cpp:publish`; `A/AudioLive.c:fill_period`, `capture_period`; `K/sound/soc/qcom/qdsp6/q6apm.c:graph_callback`. Host DONE/timeout counts are not a hardware XRUN counter. |
| VA route, votes, prepare/on/start order | `ours-OK` | `A/AudioSequence.c:VA_STEPS`, `kMicLive`; `A/AudioVa.c:audio_va_on`; `A/AudioBoard.cpp:vaMic`: configure between graph prepares and starts. |
| VA startup isolated from running speaker | `ours-missing` | `A/AudioVa.c:decimator_up`; `A/AudioBoard.cpp:vaSleep`: 16–18 ms requested sleep on shared worker. |
| TDM vs DMIC wiring / external bus distinction | `ours-OK` | `A/AudioSpkProt.h:SPK_LANE_RX`, `SPK_LANE_VI`; `A/AudioVa.c:audio_va_pins`; pinned board DTS `/sound`: VA is not the V/I TDM source. |
| Shared LPASS root/vote transient and DSP load | `unknown` | `A/AudioSequence.c:VA_STEPS`; `A/AudioLive.c:hold_gate`, `hold_tick`; pinned VA codec/runtime PM. Hold switch is a later hypothesis test, not proof. |
| Mic mix, settle and FIFO reserve | `ours-OK` | `A/AudioLive.c:capture_period`, `audio_live_capture`; `A/AudioLive.h:AUDIO_LIVE_MIC_TARGET`: first 100 ms zeros, fixed center/trough, missing/overrun counts. |
| Mic digital gain/mute and HAL delivery | `ours-OK` | `A/AudioLive.c:audio_live_capture`; `A/AudioCapture.h:AudioCaptureHal`; `A/AudioDsp.cpp:micHalReport`: requested/delivered/startup/missing/unavailable accounting, mute separate. |
| Tap lifetime / metadata / raw route distinction | `ours-OK` | `A/PianoAudioTap.cpp:PianoMicTapUserClient::start`, `release_hold`; `A/AudioTap.h:audio_tap_write`, `audio_tap_read`; `A/AudioLive.c:mic_wanted`. Other tap users can retain capture after app close. |
| NS block FIFO and bounded snapshot processing | `ours-OK` | `N/MicNsCore.c:micns_read`, `pull`, `process_block`; `N/MicNsCore.h:MICNS_*`: no per-read allocation/lock, bounded snapshot, startup/missing/slips counted. |
| Entire NS callback free of logging | `ours-missing` | `N/PianoMicNS.c:DoIOOperation`, `report`: audio-callback report/log; core timing excludes it. |
| NS timestamps and cached sample-time reads | `ours-OK` | `N/PianoMicNS.c:GetZeroTimeStamp`, `produce`, `DoIOOperation`, `propertyData`: host-derived 16384-frame anchors, 70 ms latency, domain 0. |
| NS worst-case callback time / HAL serialization | `unknown` | Same functions and shared `gCore`/`gRing`; StartIO/StopIO use `gLock`, IO callback does not. Need route/client-count and deadline evidence. |
| Failure/quiet/sleep lifecycle | `ours-OK` | `A/AudioDspLink.c:adg_observe`; `A/AudioLive.c:stream_fault`, `mic_fault`, `go_off`, `audio_live_sleep`; `A/PianoAudioDevice.cpp:piano_audio_devices_power`: explicit guard stops/reanchor confounders. |
| Sustained acoustic duplex and coded continuity | `unknown` | Reported symptom, short calibrated result and idle baseline above. Requires active protocol; absent evidence remains unknown. |

## One-boot RAM measurement protocol

Future work only. Use one already-qualified RAM image with known source/KC
and plug-in provenance; do not change the installed image. Keep its observed
timing, age, gain, ceiling, protection, MIPLAY, clock-hold, volume and mic hardware
route constant throughout the primary comparison. Do not enable old age or
batch480 merely to reproduce their known defects.

### Read-only preflight and capture limits

Use an already unlocked, authorized desktop and existing microphone permission.
No TCC seeding, daemon restart, `sysctl -w`, register poke, firmware GET,
silent protection probe or new debug switch belongs to this protocol. Preserve
raw logs/WAVs in an untracked session directory named by `$session_dir`, with
restricted access. Publish only sanitized counters and synthetic summaries;
do not copy full registry, boot arguments, identifiers or calibration into
tracked files.

The session has a **240-second wall-clock cap**, two raw input-open transitions
(one with continuous output, one with output stopped), and one optional NS
comparison. Read sysctls at phase boundaries and 1 Hz during active phases,
at most 240 snapshot sets. Read the ledger at that cadence and promptly at a
reported dip; cap extra event reads at eight. Take filtered registry snapshots
at phase boundaries, not every 5 ms. Stop on transport fatal/guard-off, amp
fault, unexpected mute/fallback, clipping or route change; preserve the
preceding window. No retries extend the cap. Record observer overhead.
The future runner must enforce the deadline and cap each console/log stream
at 32 MiB, snapshot output at 64 MiB and each PCM recording at 128 MiB.
Reaching a cap stops that observer and makes its coverage incomplete; it
cannot be graded as zero events. Use the existing authorized root observer
for root-only exports; permission errors mean unavailable, not zero.

Exact read-only commands for the future operator in the test boot:

```sh
/usr/bin/sw_vers
/usr/sbin/sysctl kern.boottime kern.osrelease
/usr/sbin/sysctl machdep.piano_audio_timing machdep.piano_audio_age \
  machdep.piano_audio_duplex_hold machdep.piano_audio_spk \
  machdep.piano_audio_miplay machdep.piano_audio_gain
/usr/sbin/sysctl machdep.piano_audio_spk_state \
  machdep.piano_audio_mic_route machdep.piano_audio_mic_route_state
/usr/sbin/sysctl machdep.piano_audio_ledger
/usr/sbin/ioreg -r -c PianoAudioDevice -l -w 0
/usr/sbin/ioreg -r -c PianoAudio -l -w 0
/usr/sbin/sysctl machdep.piano_glink_fifo machdep.piano_glink_owner \
  machdep.piano_smem_state
```

Read RAM boot provenance from its build/launch receipt and console. If
boot-argument confirmation is needed, read `kern.piano_bootargs` (fallback
`kern.bootargs`) into the untracked record only. ENOENT means **observer absent**,
not zero. `P/PianoAdsp.cpp:piano_adsp_start` registers passive GLINK/SMEM
observers only in existing sensor level-4 mode. Do not change that mode for
this session. Missing exports keep transport attribution unknown.
`machdep.piano_audio_spk_live` is optional when already supported; its status
does not substitute for the primary state/counter snapshots.

For `PianoAudioDevice`, retain only `device UID`, `is running`, `clock domain`,
`sample rate`, `io buffer frame size`, latency/safety offsets,
`piano-audio-transport`, `piano-audio-timing-contract`,
`piano-audio-ring-contract`, `piano-audio-volume`, `piano-audio-mute`,
`piano-audio-input-level`, `piano-audio-input-mute` when present. For
`PianoAudio`, retain `piano-audio-live` and relevant audio contract/state fields.
Obtain HAL selected device UID, rate, callback frame count, running state,
clock domain, latency, safety and timestamp seed through the test client's
read-only CoreAudio property/timestamp calls; ioreg does not show virtual
PianoMicNS. Record the actual `HAL ring` mode at each StartIO.

Capture the existing kernel console and a bounded unified-log stream from
before first StartIO through final drain. Start this observer, interrupting
it at the session deadline:

```sh
/usr/bin/log stream --style compact --level info --predicate \
  '(process == "kernel" AND (eventMessage CONTAINS "PianoAudio:" OR eventMessage CONTAINS "PianoPower:")) OR process == "coreaudiod" OR process == "audiomxd" OR subsystem == "com.unixnu.piano.micns"'
```

Retain the existing kernel-console source if IOLog is absent from unified
logging; an empty stream cannot prove no driver event. Do not enable extra
logging preferences. Use monotonic mach time plus `mach_timebase_info` in the
client; record mach/UTC pairs per phase for console correlation. For external
audio, cross-correlate a synthetic onset marker and periodic known signal
to estimate offset **and clock drift**. Matching file start times alone is
insufficient.

| Observable, exact producer | Required fields / interpretation |
| --- | --- |
| `A/PianoAudioDevice.cpp:startIO`, `run` | `HAL ring: output/input` mode, ring, period, seed; `HAL ring read` periods/sound/frames/write-end; `mic HAL stop`; per-direction `HAL wake` late-periods/max-us. Pair stop reports to starts. Do not invent sysctls for `piano_audio_dev_*` globals. |
| `A/AudioDsp.cpp:dspRunning` | `IO window start: warm`; `IO window stop`: sound, underruns, skips, fifo-dropped, polls, tick-max-us, slice-tick-max-us. Tick max excludes inbox drain/health/sleep. |
| `A/AudioLive.c:dx_report` | `duplex-obs1`: elapsed ms, refill/Mic-up ticks, minimum host in-flight depth, depth-0/≤1 ticks, largest WRITE-send gap, queue-full delta, signed/OLD policy, ahead stamps, fades and fades-as-silent. Depth 0 is a host observation, not a DSP XRUN event. |
| `A/AudioDsp.cpp:ledgerRead`; `A/AudioLedger.h` | Keep header `written`, `overwritten`, `capacity=128` and all CSV columns. Events: 1 refill anomaly, 2 start, 3 stop, 4 park, 5 rearm, 6 prime, 7 initial lead. Fields: `ms`, epoch, mode, want_out, fill_before/after, want/got/missing, producer_ms/frames/late_us, sent/returned/inflight/done_batch, monitor_start/end/field, skips/drops/repeats/slip, hal_sample/host/writeEnd, missing_total, boundary_ms, sleep_gen, state, U. |
| Ledger caveats | It records anomalies/boundaries, not every healthy refill. Sequence gaps/overwrite or EAGAIN make reconstruction incomplete; permit one immediate retry, otherwise mark unavailable. A Start row can contain the previous epoch snapshot before worker epoch assignment; align with following rows/HAL seed. `producer_late_us=4294967295` means concurrent/unavailable context. `hal_host` is mach ticks; other event times are milliseconds. Monitor fields describe amp-status slices, not protection GET duration. |
| `A/AudioDsp.cpp:spkState`; `A/AudioLive.c:spk_begin`, `stream_fault`, `mon_tick`, `mip_report` | Protection/MIPLAY request/ready/active/failed/starts/fallbacks; live-reads, vbat-sends, preg-updates; graph/amp start, STOPPED/refusal/timeout/quiet/fallback. Ready at idle does not mean active. |
| `A/AudioDsp.cpp:micHalReport`; `A/AudioLive.c:audio_live_mic_report` | Per-HAL run requested/delivered/startup/missing/unavailable/muted/mute-nonzero/fault, PCM stats; per-DSP run sent/returned/flight/errors/late/status, bytes/min/max/offsets/short/zero/align/origin/range; settle/full/active-full/idle-tail/FIFO-missing/overrun/drop/repeat/fill/primed/drift target. HAL and DSP runs have different lifetimes. |
| `N/PianoMicNS.c:report`, `StartIO`, `StopIO`, `tapOpen` | users/client/start result, tap opens/run/written; reads/delivered/startup/missing/muted/blocks/drops/repeats/anchors/runs/lapped/excess/fill; rnnoise-us and read-us averages/maxima, CPU/cycles. Report-time correlation alone is not causation. |
| `P/AdspObserveKernel.h:observeAdsp`, `observeGlink`; `P/AdspObserve.h:owner` | Keep owner_consistent/status/truncated, FIFO pending/indices, broken/tx_blocked/RX/TX messages, return debt/retry, channel intent availability/fill; poll_calls/recv_reads/recv_nonempty/clear_calls/send_calls/times; fatal_reports/link_generation. Do not infer unexported remote-mask/ACK or software enqueue depth from intent fill. |
| `P/PianoAdsp.cpp:replyPollSlice`, `timed`, `serviceGlink` | Existing pending-reply 1 ms median/max/pending/expired, deferred/retired GLINK returns and slow-client-callback lines. Sensor boundary traces are not audio per-packet timelines. |

### Synthetic dual-tone and coded-microphone schedule

Use a pre-generated 48 kHz source with phase-continuous 997 Hz and 1499 Hz
components. Scale their **combined worst-case peak to −36 dBFS** (each
equal-amplitude component at most approximately −42.03 dBFS), with 20 ms
start/end ramps. Keep the established comfortable speaker volume and all
protection limits unchanged. Continuous app playback maintains a stable output
route; it does not invoke the driver's bounded internal test-tone facility.

Feed an independent external emitter into the tablet mic: a 2303 Hz carrier
phase-coded by repeated Barker-13 `+++++--++-+-+`, 20 ms per chip, 40 ms guard
between 260 ms codewords. Use synthetic sound only, fixed positions and a
quiet reference. Calibrate beforehand so recorded peaks remain below −6 dBFS
with zero clipped samples; emitter digital dBFS does not predict microphone
gain. An external recorder continuously captures the tablet speakers, including
the instant input opens. Band-separate dual tones and mic code offline.

The test client must keep output rendering continuous while explicitly opening
and closing input, recording sample/host timestamps, actual callback lengths,
device/route changes, input/output start/stop receipts, seed changes and
callback discontinuities in preallocated memory. Save traces after phases;
do not log/allocate on each audio callback. This is a client requirement, not
a claim that an existing helper implements the full experiment. Use the
calibrated AVFoundation raw route for principal reproduction; an existing HAL
recorder may provide a separately labelled raw-HAL comparison within the cap.
Keep VPIO/AGC/AEC disabled in the primary run.

| Session seconds, maximum | Operation / discriminator |
| --- | --- |
| 0–10 | Passive preflight, external recorder already running; confirm observers, image, UIDs and quiet control. |
| 10–20 | Start dual-tone output only; wait past cold prime/fade. Establish external envelope baseline. |
| 20–82 | Open **raw Mic** while output continues; allow 2 seconds for startup, then require 60 settled seconds with external code, testing recurring dips and multiple 2-second monitor cycles. |
| 82–92 | Close raw input with output continuing; observe immediate HAL behavior and delayed mic release/vote changes. |
| 92–98 | Stop output, drain both paths for 6 seconds; verify no tap user remains before interpreting a later cold start. |
| 98–108 | Open raw input alone with coded stimulus; establish settled capture/code timing. |
| 108–120 | Start output while input remains open; test reciprocal amp-start exposure. |
| 120–126 | Stop output, keep raw input; observe fade, amp shutdown and capture continuity. |
| 126–132 | Close input and let both paths release. |
| 132–142 | Optional: restart output only after release. |
| 142–204 | Optional: open **PianoMicNS** explicitly with output active; allow 2 seconds to settle, then compare 60 seconds against raw. Do not switch system default or add VPIO. If unavailable, skip and retain NS status unknown. |
| 204–220 | Close NS, retain output 10 seconds, then stop/drain 6; final snapshots. End by 240 seconds including overhead. |

The 20-second raw-open and 142-second NS-open boundaries test input joining
continuous output; reverse order starts input while output is stopped. If
preparation overruns, shorten/omit the optional phase; do not silently shorten
raw steady capture below 60 seconds and still claim that gate passed. If raw
startup exceeds 2 seconds, extend that phase within the cap by reducing the
optional comparison, or mark the steady gate incomplete.

Fit playback components independently in 5 ms windows; compare envelopes
against output-only baseline and inspect dip depth and phase continuity. Flag
a ≥6 dB envelope loss lasting ≥5 ms outside scheduled ramps as a candidate,
then inspect raw waveforms. This threshold defines the experiment, not universal
audibility or a firmware-XRUN rule. Cross-correlate successive mic codewords
in 10 ms steps, refine at sample resolution, fit a clock-drift line and retain
lost/repeated-codeword or sudden lag-step events. Account for deliberate
single-frame slips separately from missing blocks. Do not grade RNNoise tone
attenuation/code rejection as PCM loss; its primary comparison is callback
timing/timestamps/counters against raw.

Exclude graph startup's first 100 ms mic zeros, FIFO/NS priming and intentional
speaker ramps from **steady** loss grades, but report actual durations and
any output dip at input open. Do not discard the transition under test.
Raw HAL accounting should satisfy
`requested = delivered + startup + missing + unavailable`, with muted frames
a subset of delivered. Require zero post-prime missing/active-full/overrun,
zero wire errors/new faults/fallbacks and no unexpected output restart; late
host wakes and intentional clock slips get separate totals. No dips in a
bounded run closes only that observed window.

### Same-boot classification and limits

| Synchronized signature | Classification / next decision |
| --- | --- |
| Unexpected output StopIO/StartIO, seed/epoch reset and re-prime after input open, aligned with acoustic gap | HAL/route restart established at driver boundary; attribute caller to coreaudiod/app only with matching receipts. Warm DSP graph does not exclude it. |
| No restart; ahead stamps + old age + fades-as-silent, supplied PCM intact | Existing unsigned-age path implicated. Check deployment/selection of already implemented signed policy. |
| Actual batch480 + new >2400-to-960 trim + four-slot burst + 960 missing | Conditional software reserve defect associated with this window. Does not apply to confirmed period240. |
| Stable epoch; producer late/write lead worsens before FIFO short reads; worker/transport otherwise supplied | Host/HAL production starvation candidate. Input late-wake totals cannot locate output producer delay. |
| Stable producer/FIFO supply; long WRITE gap/low depth and tick/slice cost near VA/amp/monitor event | Common-worker service exposure implicated. Tick max excludes some boundaries; correlation is not a measured device-lock hold. |
| Queue-full with GLINK pending/intents/debt/tx_blocked or owner progress stalls | Transport queue/owner pressure candidate. Separate enqueue retry from publish; snapshots do not time each command. |
| Timely supplied PCM, stable seeds, no software fade/fallback, acoustic dip persists | Downstream DSP/protection/MIPLAY/TDM/clock behavior remains `unknown`. No exported firmware XRUN counter identifies it; healthy WRITE_DONE is insufficient. |
| Raw clean; NS misses deadlines or re-primes with elevated read/block cost | HAL plug-in processing/scheduling candidate. Include report cost/client concurrency; low average CPU does not close worst-case timing. |
| Only input late-period counts rise; code/PCM continuous | Scheduling observation, not demonstrated capture loss. Enlarging batch changes the threshold without establishing a fix. |

This protocol distinguishes restarts, observed software shortages and
transition/transport correlations in **one boot**. It cannot guarantee a unique
root cause from counters without per-event timestamps. If same-boot evidence
is ambiguous, preserve `unknown`; do not infer DSP underrun from queue depth
or workloop contention from a late input wake.

Missing discriminators are bounded timestamps before/after engine wait,
lock and body; worker drain/tick/sleep and VA/amp action boundaries; audio
enqueue-to-GLINK-publish/DONE latency; and firmware-supported underrun/clock
events with a defined timebase. Existing authorized scheduler traces, if
available, may distinguish timer expiry, runnable delay and lock holders.
Adding tracepoints, firmware commands or a RAM instrumentation image is
separate future work, not performed here. Static analysis cannot recover
missing timestamps retrospectively.

## Fix-image gate

```text
Status: HOLD
Scope: B2 full-duplex audio static analysis only; no code or image change.
Evidence: independent functional passes, short calibrated acoustic overlap,
          idle installed baseline and source invariants; active failure unclassified.
Required: one-boot synchronized raw duplex transitions/60-second steady window,
          latched mode + age + seed/epoch, ledger, input health and external audio.
Unknowns: HAL restart behavior, actual worker/transport delay and DSP/clock XRUNs.
Next decision: select one causal layer from the observed signature before proposing
               a minimal change and matching validation; ambiguity keeps HOLD.
Deployment: none; installed profile and hardware untouched; never push.
```
