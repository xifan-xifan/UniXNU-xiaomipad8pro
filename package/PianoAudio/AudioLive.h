/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoAudio's levels 8-10 (docs/drivers/audio.md section 8): the macOS devices on the DSP, after levels 4-6 passed in the same boot
 * (the session hands over the four amplifiers configured and powered down, the TDM pins muxed, the PCM streams through bypass and
 * the audio slice mapped: AudioSessionConfig.keep):
 *   8  the Speaker device's periods (AudioPcm.h's dsp transport: its engine thread writes them into the ring below) played through
 *      the speaker plan: brought up when the device runs with sound, the amplifiers started muted at the playing gain (at most 160
 *      at this level) and unmuted after four silent periods came back AND the output epoch's PCM lead is primed, every 10 ms DSP period taken from the ring through the
 *      safety chain (the slots, the DC blocker, the master gain, the fades, the -20 dBFS ceiling of this level), the amplifiers'
 *      group check while they play; taken down (a fade to silence, the amplifiers muted, after idle_ms without sound their power,
 *      the plan undone) when the device stops, is muted or plays only silence
 *   9  also the daily level: the playing gain up to the core's FS19XX_GAIN_CAP (231), the ceiling up to -1 dBFS, the kernel's own
 *      master volume and mute (boot arguments, the Speaker device's properties) ramped without a click; the HAL's own volume works
 *      on the samples before they come here
 *   10 also the Mic device: the capture plan (the VA clocks, the mic graphs, the VA macro's DMIC path between their prepare and
 *      start) brought up while the device runs, the capture pump's two-channel periods mixed to mono into the Mic's ring, the
 *      first 100 ms dropped (the start transient); also while a tap client is open (AudioTap.h: "Mic (noise reduced)"), each
 *      period's mono frames going into the tap as well
 * The rules of AudioSession.h hold here too: one DSP command at a time over both plans, each answer checked, the reverse unwind;
 * nothing new while PianoPower asks for quiet (a running stream stops, and comes back after); at any sign of trouble in PianoPower's
 * health no DSP command any more, the amplifiers muted, powered down and held in reset at once. A refused command or period, or an
 * amplifier whose status changes while it plays, ends that path for the boot; a late period stops it (the third time for the boot).
 * Two clocks: the devices run on the host clock, the DSP on the TDM's; each ring's fill is held near its target by dropping or
 * repeating single frames (counted).
 * Speaker protection (levels 9-10, piano-audio-spk=1 or machdep.piano_audio_spk=1 while the speaker is down; AudioSpkProt.h): the
 * speaker plan is the stock FSADSP pair instead of the passthrough device graph, with the two private files the session's ops carry;
 * while it runs, every SPK_MONITOR_MS the battery voltage goes to SP_RX and its live data is read and logged ("PianoSpeakerProtV1
 * live ch..."). Any FSADSP command error (the plan, the monitor, a refused or late period on that graph) undoes the pair and the
 * speaker comes back on the passthrough graph in the same boot ("PianoSpeakerProtV1: FALLBACK"); missing or mismatched private
 * files leave it on the passthrough graph from the start. The amplifiers' gain cap and every safety rule above are unchanged.
 * MIPLAY (piano-audio-miplay or machdep.piano_audio_miplay, default 0 off; AudioSpkProt.h): inside the protection plan only, the stock
 * Dolby/MiSound subgraph between the stream and the speaker graph's MFC, with the private file spk-dolby.bin. Every speaker start logs
 * its MIPLAY state ("PianoMiplayV1: speaker start N: MIPLAY ..."); any error of a start or a run with MIPLAY (a plan step, the
 * monitor, a refused or late period) undoes it all and the speaker comes back on the protection plan without MIPLAY
 * ("PianoMiplayV1: FALLBACK ..."), until a new request; the protection plan's own fallback to passthrough is unchanged.
 * Portable C, new code for PianoAudio: the owner ticks it with the time and PianoPower's health, feeds it what arrives and supplies
 * the hardware through the session's AudioSessionOps; the devices' threads only touch AudioLiveShared. Host-tested in
 * tests/live_host.c against models of the DSP, the amplifiers and both devices' engines in simulated time. */
#ifndef PIANO_AUDIO_LIVE_H
#define PIANO_AUDIO_LIVE_H
#include <stdint.h>
#include "AudioTiming.h"
#include "AudioLedger.h"
#include "AudioSession.h"
#include "AudioCapture.h"
#include "AudioTap.h"
#include "AudioSpkProt.h"
#include "AudioVa.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_LIVE_RING 4096u            /* independent stereo DSP FIFO: 85 ms */
#define AUDIO_LIVE_TARGET 960u            /* 20 ms before the four-period DSP queue */
#define AUDIO_LIVE_DEPTH PIANO_AUDIO_IN_FLIGHT /* speaker periods queued at the DSP while playing: 40 ms */
#define AUDIO_LIVE_IDLE_MS 3000u         /* no sound (or no device) this long: the path goes down */
#define AUDIO_LIVE_IDLE_MAX_MS 600000u
#define AUDIO_LIVE_STATUS_MS 500u        /* the amplifiers' status while they play */
#define AUDIO_LIVE_LATE_STOPS 3u         /* late stops of a path before it is off for the boot */
#define AUDIO_LIVE_MIC_RING 8192u        /* mono FIFO: center + batch + band + consumer phase need >4096 */
#define AUDIO_LIVE_MIC_DEPTH PIANO_AUDIO_IN_FLIGHT
#define AUDIO_LIVE_MIC_SETTLE_MS 100u    /* the capture's first periods: zeros */
#define AUDIO_LIVE_SLIP_BAND 240u        /* frames the smoothed fill may stray from where it settled before single frames slip */
/* The producer samples before every write in a completion batch. Its mean
 * can exceed the trough by (depth-1)*period/2. Include that bias AND the
 * controller's deadband so a nominal 20 ms trough is not actually zero. */
#define AUDIO_LIVE_MIC_RESERVE 960u
#define AUDIO_LIVE_MIC_BATCH_BIAS ((AUDIO_LIVE_MIC_DEPTH - 1u) * AUDIO_PERIOD_FRAMES / 2u)
#define AUDIO_LIVE_MIC_TARGET (AUDIO_LIVE_MIC_RESERVE + AUDIO_LIVE_SLIP_BAND + AUDIO_LIVE_MIC_BATCH_BIAS)
#define AUDIO_VOLUME_STEP 1024           /* Q15 per 10 ms period: the master volume's ramp (a full swing in 0.32 s) */
#define AUDIO_LIVE_IDLE_TICK_MS 2000u    /* the tick with both paths down (the transport wakes the owner sooner) */
/* The latency the transport adds, for the devices' registry (AudioPcm.h output_latency/input_latency). */
#define AUDIO_LIVE_OUTPUT_LATENCY (AUDIO_LIVE_DEPTH * AUDIO_PERIOD_FRAMES + AUDIO_LIVE_TARGET)
#define AUDIO_LIVE_START_FRAMES AUDIO_LIVE_OUTPUT_LATENCY /* four-slot burst plus the 960-frame reserve */
#define AUDIO_LIVE_INPUT_LATENCY (AUDIO_PERIOD_FRAMES + AUDIO_LIVE_MIC_TARGET)

/* ---- what the devices' engine threads share with the owner's thread (single producer, single consumer per ring) */
typedef struct {
    AudioRing out;                       /* stereo: the Speaker's engine writes, the live thread reads */
    AudioRing in;                        /* mono: the live thread writes, the Mic's engine reads */
    uint64_t mic_control;                /* requested/applied route, generation and input/release gate */
    int want_out, want_in;               /* the devices run (the transport's running()) */
    int accepting;                       /* the speaker stream takes the ring now (else play() drops: nothing would read it) */
    int input_primed;                    /* consumer owns; reserve reached after graph start */
    int capturing;                       /* the Mic's ring is fed now */
    int sound;                           /* a period with a non-zero sample came while the speaker path was down */
    uint32_t sound_at;                   /* the time of the last such period (ms) */
    int age_signed;                      /* duplex-age1 (private DUPLEX-R3.md): 1 the sound age is signed (the fix), 0 the old unsigned test */
    int have_sound;
    int have_pcm;                        /* nonzero PCM actually accepted in this output epoch */
    uint32_t output_head;                /* StartIO's producer boundary; only the consumer discards older PCM */
    uint32_t output_frames;              /* reader geometry; the 480-frame experiment retains its old trim policy */
    int32_t volume_q15;                  /* level 9: the master volume (0..32767), from the device's property or the boot argument */
    int muted;                           /* level 9: the master mute */
    uint32_t amp_gain_requested;          /* sysctl -> worker; only the worker touches the amplifier bus */
    int32_t input_q15;
    int input_muted;
    int input_fault;                      /* unavailable/fault zeros, distinct from requested mute */
    uint32_t input_controls, input_runs;
    AudioCaptureHal input_hal;            /* one HAL StartIO, single engine writer */
    uint64_t offered, taken, dropped, captured, controls;
    uint32_t producer_seq, epoch, boundary_ms, producer_ms, producer_frames, producer_late_us;
    uint64_t hal_sample, hal_host, hal_write_end;
    int spk_prot;                         /* speaker protection asked for (the boot argument, then the sysctl); taken at a start */
    uint32_t spk_prot_gen;                /* every request: a new one clears a fallback of this boot */
    int duplex_hold;                      /* duplex-hold1 asked for (AR_HOLD_* mask: the boot argument, then the sysctl) */
    uint32_t duplex_hold_gen;             /* every request: a new one clears a failure of this boot */
    int miplay;                           /* MIPLAY asked for (spk_miplay_valid: the boot argument, then the sysctl); taken at a start */
    uint32_t miplay_gen;                  /* every request: a new one clears a MIPLAY fallback of this boot */
    /* spk-probe1: a silent protection probe asked for (audio_live_spk_probe), taken by the worker */
    uint32_t spk_probe;                   /* atomic mailbox: idle, pending request, reserved, or closed for teardown */
    /* The microphone tap (AudioTap.h): its shared header and ring (null: no tap), and how many tap clients are open; while one is,
     * the capture plan runs as if the Mic device did, and every capture period also goes into the tap. */
    PianoTapHeader *tap;
    int tap_open;

} AudioLiveShared;
/* The rings' storage: AUDIO_LIVE_RING_BYTES and AUDIO_LIVE_MIC_RING_BYTES (the owner's, kept as long as sh). */
#define AUDIO_LIVE_RING_BYTES (AUDIO_LIVE_RING * 2u * 2u)
#define AUDIO_LIVE_MIC_RING_BYTES (AUDIO_LIVE_MIC_RING * 2u)
void audio_live_shared_init(AudioLiveShared *sh, int16_t *out_storage, int16_t *in_storage, int32_t volume_q15);
/* The devices' side (AudioPcm.h's dsp transport, from their engine threads; running() with the engine stopped). play: the frames
 * taken (all of them: what does not fit, or comes while the path is down, is dropped and counted); *wake 1 when the owner's thread
 * should run now (the first sound while the speaker path is down). capture: the mono frames given (the rest is silence). */
void audio_live_running(AudioLiveShared *sh, int output, int input);
uint32_t audio_live_play(AudioLiveShared *sh, const int16_t *stereo, uint32_t frames, uint32_t now_ms, int *wake);
uint32_t audio_live_capture(AudioLiveShared *sh, int16_t *mono, uint32_t frames);
/* Level 9: the master volume in percent (0 silent, 100 unity, the shared -36 dB squared curve) and the mute; out of range: unchanged. */
void audio_live_control(AudioLiveShared *sh, int volume_percent, int mute);
/* A/B policy selector: only 192 or 231; invalid values leave the request unchanged. Level 8 still clamps to 160. */
int audio_live_gain_select(AudioLiveShared *sh, unsigned gain);
/* Speaker protection on (1) or off (0) from the next start of the speaker path; -1 for another value. */
int audio_live_spk_select(AudioLiveShared *sh, int on);
/* MIPLAY (AudioSpkProt.h spk_miplay_valid: 0 off, 1 or 2 the perf mode, +4 the pregain following the volume, +16 x (r + 1) the
 * rotation r) from the next start of the protection plan; -1 for another value. */
int audio_live_miplay_select(AudioLiveShared *sh, int request);
/* duplex-age1: 1 the signed sound age (default), 0 the old unsigned test (A/B control); applies from the next worker tick. */
int audio_live_age_select(AudioLiveShared *sh, int signed_age);
/* spk-probe1, only with the configuration's spk_probe (piano-audio-spk-probe=1): the speaker
 * protection pair brought up WITHOUT sound for `request` seconds (1..AUDIO_SPK_PROBE_MAX_S): no PCM is needed, the master mute does
 * not stop it, the amplifiers stay powered down (or, with + AUDIO_SPK_PROBE_AMPS, are powered up and held MUTED); the monitor runs
 * (battery voltage, live data); then the path goes down as after idle. Real sound during a probe takes over as usual (without the
 * amplifiers, that start ends first and the path comes back with them). Taken at the worker's next tick when the speaker path is
 * down and protection is asked for and ready; sleep cancels it. Returns EINVAL for another value, EBUSY while a request is pending
 * or a probe owns the path, and ENXIO while the worker is asleep or stopped. */
#define AUDIO_SPK_PROBE_MAX_S 30u
#define AUDIO_SPK_PROBE_AMPS 100u
int audio_live_spk_probe(AudioLiveShared *sh, uint32_t request);
/* duplex-hold1, an A/B switch (default 0: unchanged). 1: the LPASS macro and digital codec votes, 3: those and the VA (TX core)
 * mclk, requested by a plan of their own BEFORE the first path (speaker or Mic) starts and released only after both are down, so
 * that a Mic start or stop while the speaker plays changes no PRM resource; the Mic's plan then skips the held steps. Taken when
 * both paths are down and one starts (never while the Mic holds its own votes: no second owner of one vote); -1 for another value. */
int audio_live_hold_select(AudioLiveShared *sh, int mask);
void audio_live_input_control(AudioLiveShared *sh, int level_percent, int mute);
/* The tap (AudioTap.h): a client opened (+1) or closed (-1); returns the clients open now. The owner's thread then starts (or, after
 * idle_ms with neither a tap client nor the Mic device, stops) the capture plan; the caller wakes it. */
int audio_live_tap_client(AudioLiveShared *sh, int delta);
int32_t audio_volume_q15(int percent);
/* The ceiling for an attenuation of db dB below full scale (1..40; others: the level 8 ceiling, -20 dBFS). */
int32_t audio_ceiling_db(unsigned db);

/* ---- the live paths */
/* Speaker protection's larger state (the parsed private files and this start's plan with its steps): the owner's allocation, kept
 * for the boot (inside AudioLive it would take that past the kernel's 16 KiB typed allocation). */
typedef struct {
    SpkProt prot;
    SpkPlan plan;
    SpkMip mip;                          /* MIPLAY's private file as parsed at init */
} AudioLiveSpk;

typedef struct {
    uint32_t level;                      /* 8, 9 or 10 (others are clamped) */
    unsigned gain;                       /* the amplifiers' gain while playing: at most 160 at level 8, FS19XX_GAIN_CAP from 9 */
    int32_t ceiling;                     /* the chain's ceiling: AUDIO_CEILING_TEST at level 8, at most AUDIO_CEILING_MAX from 9 */
    uint32_t idle_ms;                    /* no sound or no device this long: the path goes down */
    uint32_t status_ms;                  /* the amplifiers' status while they play */
    uint32_t answer_ms;                  /* each DSP command's answer */
    int mic;                             /* level 10: the Mic's capture path (0: the Mic stays silent) */
    int mic_route, mic_diag;
    void (*mic_route_apply)(int route, uint32_t generation); /* owner only; no MMIO */
    int spk_prot;                        /* piano-audio-spk: speaker protection asked for from the first start */
    AudioLiveSpk *spk_state;             /* its state, the owner's (zeroed, kept for the boot); null: passthrough only */
    int duplex_hold;                     /* piano-audio-duplex-hold: audio_live_hold_select's mask from the first start (0 off) */
    int age_old;                         /* piano-audio-age=0: the old unsigned sound-age test (A/B control); default the signed one */
    int miplay;                          /* piano-audio-miplay: audio_live_miplay_select's request from the first start (0 off) */
    int spk_probe;                       /* piano-audio-spk-probe=1: audio_live_spk_probe's requests are taken (0: ignored) */
} AudioLiveConfig;
void audio_live_defaults(AudioLiveConfig *c, uint32_t level);

enum {LiveDown = 0, LiveStarting, LiveRunning, LiveStopping, LiveOff};
enum {SpkPriming = 0, SpkPlaying, SpkFading, SpkMuting, SpkQuiet};

typedef struct {
    int32_t avg, setpoint;               /* 64ths of a frame */
    uint32_t samples;
} AudioLiveDrift;

typedef struct {
    ArSeq seq;
    int active, in_flight, pending_step, pending_undo;
    int quiet;                           /* no line per command (the protection monitor every 2 s): only errors */
    uint32_t unsent, sent_at;
    const char *name;
    uint8_t pkt[GPR_MAX_PACKET];
} AudioLivePlan;

/* Optional single pre-mix window, armed explicitly for the next capture run.
 * Format: eight LE u32 (magic "PMR1", generation, run, signed route, start/end
 * owner ms, frame/period counts), then periods and interleaved LE S16 stereo. */
#define AUDIO_MIC_DIAG_MS 8000u
#define AUDIO_MIC_DIAG_PERIODS (AUDIO_MIC_DIAG_MS / 10u)
#define AUDIO_MIC_DIAG_FRAMES (AUDIO_MIC_DIAG_PERIODS * AUDIO_PERIOD_FRAMES)
#define AUDIO_MIC_DIAG_MAGIC 0x31524d50u
typedef struct {
    uint32_t arrival_ms, frames, dsp_time_lo, dsp_time_hi;
} AudioMicPeriod;
typedef struct {
    uint32_t magic, generation, run;
    int32_t route;
    uint32_t start_ms, end_ms, frames, periods;
    AudioMicPeriod period[AUDIO_MIC_DIAG_PERIODS];
    int16_t pcm[AUDIO_MIC_DIAG_FRAMES * 2u];
} AudioMicWindow;
typedef struct {
    int requested, applied, diag_requested, diag_applied, stopped, armed, recorded;
    uint32_t generation;
} AudioMicRouteStatus;

typedef struct {
    AudioLiveConfig cfg;
    AudioSessionOps ops;
    AudioDspGuard *guard;
    const AudioPorts *ports;
    AudioAmps *amps;
    AudioLiveShared *sh;
    int finished, off, held, was_held, aborting;
    uint32_t started_at, now;
    /* ---- the speaker path */
    AudioLivePlan splan;
    ArSpeakerPlan speaker;
    int spk_state, spk_mode, spk_reason, spk_ending;
    ArPump pump;
    AudioChain chain;
    AudioGuard sguard;
    int unmuted, dsp_error, stream_unsent, hold_from_sound;
    int spk_primed;
    uint32_t prime_left;                 /* exclude the first four PCM periods from drift calibration */
    unsigned amp_bad, late_stops;
    uint32_t stream_at, next_status, quiet_at, stream_len, mute_after;
    int32_t gain_now;                    /* the master gain as ramped */
    AudioLiveDrift spk_drift;
    AudioAmpsPoll status_poll;
    int status_poll_active;
    unsigned gain_target, gain_slot;
    int gain_changing, gain_witness;
    unsigned gain_reports;               /* at most eight four-amplifier snapshots per boot */
    uint32_t status_polls, status_slices;
    uint8_t spkt[GPR_MAX_PACKET];
    int16_t stereo[AUDIO_PERIOD_FRAMES * 2];
    int16_t quad[AUDIO_PERIOD_FRAMES * 4];
    int32_t peak;
    uint32_t spk_starts, spk_stops, periods, sound_periods, underruns, skipped, drops, repeats;
    /* ---- speaker protection (AudioSpkProt.h) */
    AudioLiveSpk *spk;                   /* the private files as parsed at init, this start's plan (cfg.spk_state) */
    int prot_ready;                      /* both files valid: protection can run */
    int prot_active;                     /* this start of the speaker path is the protection pair */
    int prot_fallback;                   /* an FSADSP error while it ran: down, then back on the passthrough graph */
    int prot_failed;                     /* a fallback happened: passthrough until a new request (spk_prot_gen) */
    uint32_t prot_gen;
    const char *prot_why;                /* why it is not ready, or the last fallback */
    uint32_t prot_starts, prot_fallbacks;
    AudioLivePlan xplan;                 /* the monitor: battery voltage, live data */
    SpkMonitor mon;
    uint32_t mon_at, plan_token;
    int mon_stopping, live_bad, vbat_noted;
    SpkLive live_data;
    uint32_t live_reads, vbat_sends, vbat_uv;
    uint32_t live_at;                    /* the worker's time of the last live data (live_reads > 0) */
    /* ---- spk-probe1 (audio_live_spk_probe): the silent protection probe */
    uint32_t probe_until, probe_requests, probe_runs, probe_refused, probe_reads_from, probe_reads, probe_up;
    int probing;                         /* a probe was taken and has not ended */
    int probe_amps;                      /* this probe powers the amplifiers up (muted) */
    int probe_only;                      /* the current plan was started for the probe and has had no playback takeover */
    int probe_noamps;                    /* this start of the speaker path is the probe's alone with the amplifiers left powered down */
    const char *probe_why;               /* the last probe's outcome or refusal (a static string) */
    /* ---- MIPLAY (AudioSpkProt.h), inside the protection pair */
    int mip_ready;                       /* its private file is valid */
    int mip_active;                      /* this start of the protection pair has MIPLAY */
    int mip_failed;                      /* a MIPLAY fallback happened: none until a new request (miplay_gen) */
    uint32_t mip_request;                /* this start's request (spk_miplay_valid) */
    uint32_t mip_gen;
    const char *mip_why;                 /* why it is not ready, or the last fallback */
    uint32_t mip_starts, mip_fallbacks, preg_updates;
    int32_t preg_sent;                   /* the DAP's pregain the DSP has (1/16 dB) */
    int mon_preg;                        /* the monitor round in flight carries a pregain update */
    /* ---- the mic path */
    AudioLivePlan mplan;
    ArMicPlan micplan;
    int mic_state, mic_reason, mic_ending, cap_unsent, cap_error;
    unsigned mic_late_stops;
    ArPump cap;
    uint8_t cpkt[GPR_MAX_PACKET];
    uint32_t cap_len, mic_at, mic_idle_at;
    int mic_idle;
    AudioLiveDrift mic_drift;
    int16_t mono[AUDIO_PERIOD_FRAMES + 1];
    uint32_t mic_starts, mic_periods, mic_drops, mic_repeats, mic_full;
    int va_on, mic_route;
    uint32_t mic_route_generation;
    AudioMicWindow *mic_window;           /* backing, published at the first explicit arm, kept for the boot */
    AudioMicWindow *mic_recording;        /* owner only while capturing into the backing */
    /* ---- duplex-hold1 (audio_live_hold_select): the held VA clocks, their plan and this boot's record */
    AudioLivePlan hplan;
    unsigned hold_mask;                   /* what hplan holds (or brings up) now; 0: no hold plan */
    unsigned mic_held;                    /* the mask this Mic run's plan skipped (0: it holds its own votes) */
    int hold_failed;                      /* a hold plan failed: 1 none until a new request (duplex_hold_gen), 2 none for the boot */
    int hold_demand;                      /* a path waited for the hold in this tick (it begins at the next: keep the hold) */
    uint32_t hold_gen, hold_starts, hold_failures;
    /* Reset per DSP capture start, independent of a HAL open/close window. */
    AudioCapturePcm mic_pcm[3];            /* DEC0, DEC1, mono before settling/control */
    uint64_t mic_frames, mic_settle, mic_full_frames, mic_tail_frames;
    uint64_t mic_underrun_base, mic_overrun_base;
    uint32_t mic_drop_base, mic_repeat_base, mic_full_base;
    uint32_t mic_first_read, mic_first_pcm, mic_diag_at;
    /* ---- the exchange */
    uint32_t sends, queue_full, received, stray, timeouts;
    /* 2026-11-21 duplex-obs1 (private DUPLEX.txt sections 8.4 and 9): the speaker refill's DSP queue depth and send cadence per
     * 2 s window while the speaker plays, with the Mic's state; instrumentation only, no timing change */
    uint32_t dx_ticks, dx_starved, dx_low, dx_depth_min, dx_gap_max, dx_last_send, dx_last_log, dx_qfull_base, dx_mic_ticks;
    uint32_t dx_ahead, dx_fades, dx_fades_silent;   /* duplex-age1: sound stamped after this tick's now; fade-outs (silent ones) */
    uint32_t ahead_total, fades_silent_total;      /* the same for the whole boot (not reset per window) */
    int sleeping, parked, sleep_failed, sleep_quiet, sleep_safe;
    uint32_t sleep_generation, ledger_epoch, ledger_out, done_batch;
    uint32_t monitor_start, monitor_end, monitor_field;
    AudioLedger *ledger;   /* optional fixed allocation owned for the boot */

} AudioLive;

/* After audio_session_tick finished with the hardware kept (audio_session_kept): the session's amplifiers, ops, guard and ports.
 * 0, or -1 (nothing set) when the session did not hand over. */
int audio_live_init(AudioLive *l, const AudioLiveConfig *c, const AudioSession *session, AudioLiveShared *sh);
/* One tick at now_ms with PianoPower's health (o: the observation; null keeps the guard's state). Returns the milliseconds until
 * the next tick wants to run (5 while a stream runs, 2 while a plan moves, AUDIO_LIVE_IDLE_TICK_MS with both paths down: the
 * transport wakes the owner at the first sound), 0 once finished (the guard went off: everything is down and the amplifiers are
 * held in reset). */
uint32_t audio_live_tick(AudioLive *l, const AdgObservation *o, uint32_t now_ms);
void audio_live_receive(AudioLive *l, const uint8_t *packet, uint32_t length, uint32_t now_ms);
/* The owner's stop: both paths down (hard: without DSP commands), the amplifiers held in reset, the TDM pins given back, then
 * finished. */
void audio_live_abort(AudioLive *l, int hard, uint32_t now_ms);
/* Reversible PM path: no abort of a command in flight, no guard reset. The
 * caller first parks both HAL readers; timeout cancels sleep, never forces unmap. */
void audio_live_sleep(AudioLive *l, uint32_t generation);
int audio_live_parked(AudioLive *l);  /* 1 safe, 0 draining, -1 unsafe */
int audio_live_rearm(AudioLive *l);
void audio_live_sleep_fail(AudioLive *l); /* owner-thread safe mute, retain all uncertain backing */
AudioMicRouteStatus audio_live_mic_route_status(const AudioLiveShared *sh);
/* Positive BSD errno; no hardware access. A change cancels any armed/completed
 * window. The owner serializes apply with StartIO via the shared control word. */
int audio_live_mic_request(AudioLiveShared *sh, int diagnostic, int value);
/* Serialize arm/discard and snapshot copies in the caller. Backing, when arming,
 * must remain allocated for the life of the live worker. */
int audio_live_mic_record(AudioLive *l, int on, AudioMicWindow *backing);
const AudioMicWindow *audio_live_mic_window(const AudioLive *l);
void audio_live_mic_report(AudioLive *l, const char *event, uint32_t now_ms);
const char *audio_live_state(int state);
typedef struct {
    uint32_t requests, runs, refused, until, reads, live_reads, live_at;
    int enabled, probing, prot_active;
    const char *why;
    SpkLive live_data;
    uint32_t re25[4];
} AudioLiveSpkStatus;
/* Capture on the worker. Readers format a copy of this snapshot. */
void audio_live_spk_status(const AudioLive *l, AudioLiveSpkStatus *status);
/* spk-probe1: the probe's state and the last live data per amplifier as one line, relative values only (r0 in permille of that
 * amplifier's Re25, f0 Hz, Q, coil temperature in 0.1 C, the read's age in ms at now, a plausibility word): never the calibration
 * itself. Returns the length written (always terminated when cap > 0). */
unsigned audio_live_spk_live_text(const AudioLiveSpkStatus *status, uint32_t now, char *buf, unsigned cap);
#define AUDIO_SPK_LIVE_OK_MIN 700u       /* permille of Re25 */
#define AUDIO_SPK_LIVE_OK_MAX 1500u
#define AUDIO_SPK_F0_OK_MIN 200u         /* Hz */
#define AUDIO_SPK_F0_OK_MAX 2000u

#ifdef __cplusplus
}
#endif
#endif
