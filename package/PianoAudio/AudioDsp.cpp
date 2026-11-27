// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio level 2: the DSP link over PianoPower's client contract (package/include/PianoAdspContract.h, docs/drivers/charging.md
// section 9). The channel "adsp_apps" is registered (the DSP opens it: we answer; 20 receive intents of 512 bytes, the DT's
// qcom,intents), PianoPower's health is watched through AudioDspLink.h's guard, and one question goes to the DSP's graph manager:
// is the framework ready (APM_CMD_GET_SPF_STATE from the port set's APM port: Ubuntu's port 1 by default, piano-audio-ports=N for
// another, piano-audio-ports-try=1 to walk the others while nobody answers). Nothing else is sent at this level.
// Levels 4-7 (piano_audio_levels, AudioSession.h) then run on the same channel, inbox and guard, with the hardware of AudioBoard.cpp;
// from piano-audio=8 levels 8-10 (AudioLive.h) follow for the rest of the boot on what levels 5 and 6 set up, fed by the macOS
// devices through the dsp transport defined here (AudioPcm.h). PianoAudio never starts, stops or restarts the DSP and votes nothing:
// PianoPower owns it (piano-power>=2 in the same boot).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <PianoAdspContract.h>
#include "AudioLog.h"
#include "AudioBoard.h"
#include "AudioDspLink.h"
#include "AudioLive.h"
#include "AudioPcm.h"
#include "AudioPower.h"
#include <sys/sysctl.h>
#include <sys/kauth.h>
SYSCTL_DECL(_machdep);
#include "AudioSession.h"

extern "C" {
// Post-mortem: the link's state (AudioLink AlWaiting..AlOff) | the guard's reason << 8 | queries << 16.
volatile uint32_t piano_audio_dsp_word = 0;
// Post-mortem: levels 4-7: the session's phase | the results of levels 4..7 << 8 (two bits each: AudioLevel*).
volatile uint32_t piano_audio_levels_word = 0;
// Post-mortem: levels 8-10: the speaker path's state | the mic path's << 4 | the speaker's mode << 8 | its starts << 16.
volatile uint32_t piano_audio_live_word = 0;
}

namespace {

AudioInbox gInbox;
AudioLink gLink;
PianoAdspChannel *gChannel;
int gChannelUp;

void onState(void *, int up) {__atomic_store_n(&gChannelUp, up ? 1 : 0, __ATOMIC_RELEASE);}
// On PianoPower's DSP thread: copy and return (the contract's rule 2).
void onReceive(void *, const uint8_t *data, uint32_t length) {audio_inbox_put(&gInbox, data, length);}
const PianoAdspClient kClient = {onState, onReceive};

uint32_t nowMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return uint32_t(ns / 1000000);
}

void observe(AdgObservation &o, const PianoAdspHealth &h)
{
    o.contract_ok = h.version == PIANO_ADSP_HEALTH_VERSION;
    o.dsp_up = h.dsp_up != 0;
    o.fatal = h.fatal != 0;
    o.fatal_reports = h.fatal_reports;
    o.link_generation = h.link_generation;
    o.charger_pd = h.charger_pd;
    o.charger_generation = h.charger_generation;
    o.quiet = h.quiet;
    o.crash = h.crash;
    o.channel_up = __atomic_load_n(&gChannelUp, __ATOMIC_ACQUIRE);
}

}  // namespace

// Runs on PianoAudio's thread until the exchange ends (ready, gave up, off) or wait_s pass; the summary for the log and ioreg.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size)
{
    audio_inbox_init(&gInbox);
    audio_link_init_ports(&gLink, ports, try_ports ? (1u << AUDIO_PORT_SETS) - 1u : 0u, 10, settle_ms);
    const AudioPorts *set = audio_ports(gLink.set);
    piano_audio_log("dsp: GPR ports: set %d (%s: %s): APM from %#x, PRM from %#x, data from %#x%s", gLink.set, set->name, set->source,
                    set->apm, set->prm, set->data, try_ports ? "; the others are tried while nobody answers" : "");
    int lastSet = gLink.set;
    gChannel = piano_adsp_channel("adsp_apps", AUDIO_INBOX_BYTES, 20, 0, &kClient, nullptr);
    if (!gChannel) {
        snprintf(summary, size, "adsp_apps refused by PianoPower (name taken or slots full)");
        piano_audio_log("dsp: %s", summary);
        return -1;
    }
    piano_audio_log("dsp: channel adsp_apps registered (20 x %u-byte intents; the DSP opens it, we answer)", AUDIO_INBOX_BYTES);
    const uint32_t start = nowMs();
    int lastGuard = -1, lastChannel = -1;
    unsigned lastQueries = 0;
    uint8_t q[GPR_HEADER_BYTES], in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (gLink.state != AlReady && gLink.state != AlGaveUp && gLink.state != AlOff) {
        const uint32_t now = nowMs();
        if (now - start >= wait_s * 1000) break;
        bzero(&h, sizeof h);
        piano_adsp_health(&h);
        observe(o, h);
        for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_link_receive(&gLink, in, n, now);
        const uint32_t n = audio_link_tick(&gLink, &o, now, q);
        if (n && piano_adsp_send(gChannel, q, n)) audio_link_unsent(&gLink);   // the queue is full: asked again at the next tick
        if (o.channel_up != lastChannel) {
            piano_audio_log("dsp: channel adsp_apps %s (PianoPower stage %d, dsp_up %u, charger_pd %#x gen %u, link gen %u, quiet %#x)",
                            o.channel_up ? "up" : "not up", h.stage, h.dsp_up, h.charger_pd, h.charger_generation, h.link_generation, h.quiet);
            lastChannel = o.channel_up;
        }
        if (gLink.guard.state != lastGuard) {
            piano_audio_log("dsp: guard %s%s%s", gLink.guard.state == AdgRun ? "run" : gLink.guard.state == AdgWait ? "wait" : "OFF",
                            gLink.guard.state == AdgOff ? ": " : "", gLink.guard.state == AdgOff ? adg_reason(gLink.guard.reason) : "");
            lastGuard = gLink.guard.state;
        }
        if (gLink.set != lastSet) {
            const AudioPorts *newPorts = audio_ports(gLink.set);
            piano_audio_log("dsp: nobody answered from port %#x: GPR ports set %d (%s: %s) now: APM %#x, PRM %#x, data %#x",
                            audio_ports(lastSet)->apm, gLink.set, newPorts->name, newPorts->source, newPorts->apm, newPorts->prm,
                            newPorts->data);
            lastSet = gLink.set;
            lastQueries = 0;
        }
        if (gLink.spf.queries != lastQueries) {
            piano_audio_log("dsp: APM_CMD_GET_SPF_STATE %u sent from port %#x (so far: %u answers, last state %u, %u refused, %u timeouts)",
                            gLink.spf.queries, gLink.spf.port, gLink.spf.answers, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts);
            lastQueries = gLink.spf.queries;
        }
        piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
        IOSleep(20);
    }
    piano_audio_dsp_word = uint32_t(gLink.state) | uint32_t(gLink.guard.reason) << 8 | gLink.spf.queries << 16;
    if (gLink.state == AlReady)
        snprintf(summary, size, "APM ready (status 1) after %u queries from port %#x (set %d, %s), %u ms after the first; inbox dropped %u",
                 gLink.spf.queries, gLink.spf.port, gLink.set, audio_ports(gLink.set)->name, gLink.ready_at - gLink.first_sent,
                 gInbox.dropped + gInbox.too_long);
    else if (gLink.state == AlOff)
        snprintf(summary, size, "stopped for the rest of the boot: %s (crash \"%s\")", adg_reason(gLink.guard.reason), h.crash);
    else if (gLink.state == AlGaveUp)
        snprintf(summary, size, "APM not ready after %u queries from port %#x (last status %u, refused %u, timeouts %u; sets tried %#x)",
                 gLink.spf.queries, gLink.spf.port, gLink.spf.last_status, gLink.spf.errors, gLink.spf.timeouts, gLink.tried);
    else
        snprintf(summary, size, "no answer within %u s (channel %s, guard %s, %u queries)", wait_s, o.channel_up ? "up" : "not up",
                 gLink.guard.state == AdgRun ? "run" : "wait", gLink.spf.queries);
    piano_audio_log("dsp: %s", summary);
    return gLink.state == AlReady ? 0 : -1;
}

// ------------------------------------------------------------------------------------------------------- levels 4-7
namespace {

AudioSession gSession;

void sessionLog(void *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sessionLog(void *, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    IOLog("PianoAudio: %s\n", line);
}

int sessionSend(void *, const uint8_t *packet, uint32_t length)
{
    return gChannel && piano_adsp_send(gChannel, packet, length) == 0 ? 0 : -1;
}

uint32_t levelsWord(const AudioSession &s)
{
    uint32_t w = uint32_t(s.phase) & 0xffu;
    for (unsigned l = 4; l <= 7; ++l) w |= (uint32_t(s.result[l]) & 3u) << (8 + 2 * (l - 4));
    return w;
}

// ------------------------------------------------------------------------------------------------ levels 8-10
// The dsp transport (AudioPcm.h): the devices' engine threads hand their periods to AudioLive.h's shared state; the first sound
// while the speaker path is down, and every start or stop of a device, wakes the live thread (which otherwise sleeps 2 s).
AudioLiveShared *gShared;
AudioLive *gLive;
IOLock *gWakeLock;
bool gWake;
uint32_t gIoSound, gIoUnderruns, gIoSkipped, gIoPolls;
uint64_t gIoDropped;
bool gIoWarm;
uint32_t gTickMaxUs, gSliceMaxUs;
uint32_t gPmRequest, gPmAck, gPmSleep, gPmGeneration;
int gPmResult, gPmFailed;

uint32_t gMicHalReportAt, gMicHalControls;

void micHalReport(const char *event)
{
    const AudioCaptureHal &h = gShared->input_hal; // engine thread, or stopped
    piano_audio_log("mic HAL: run %u event %s requested %llu delivered %llu startup %llu missing %llu muted %llu mute-nonzero %llu first-frame+1 %llu gain-q15 %d mute %d unavailable %llu fault %d",
        gShared->input_runs, event, (unsigned long long)h.requested, (unsigned long long)h.delivered,
        (unsigned long long)h.startup, (unsigned long long)h.missing, (unsigned long long)h.muted,
        (unsigned long long)h.mute_nonzero, (unsigned long long)h.first_delivery,
        __atomic_load_n(&gShared->input_q15, __ATOMIC_ACQUIRE), __atomic_load_n(&gShared->input_muted, __ATOMIC_ACQUIRE),
        (unsigned long long)h.unavailable, __atomic_load_n(&gShared->input_fault, __ATOMIC_ACQUIRE));
    piano_audio_log("mic HAL PCM: run %u frames %llu nonzero %llu peak %u sum %lld squares %llu clipped %llu",
        gShared->input_runs, (unsigned long long)h.pcm.frames, (unsigned long long)h.pcm.nonzero, h.pcm.peak,
        (long long)h.pcm.sum, (unsigned long long)h.pcm.squares, (unsigned long long)h.pcm.clipped);
}

uint32_t elapsedUs(uint64_t start)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return uint32_t(ns / 1000u);
}

void liveWake()
{
    if (!gWakeLock) return;
    IOLockLock(gWakeLock);
    gWake = true;
    IOLockWakeup(gWakeLock, &gWake, true);
    IOLockUnlock(gWakeLock);
}

void liveSleep(uint32_t ms)
{
    IOLockLock(gWakeLock);
    if (!gWake) {
        uint64_t deadline;
        clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
        IOLockSleepDeadline(gWakeLock, &gWake, deadline, THREAD_UNINT);
    }
    gWake = false;
    IOLockUnlock(gWakeLock);
}

void dspRunning(void *, int output, int input)
{
    const bool wasInput = __atomic_load_n(&gShared->want_in, __ATOMIC_ACQUIRE) != 0;
    if (!input && wasInput) micHalReport("stop");
    const bool wasOutput = __atomic_load_n(&gShared->want_out, __ATOMIC_ACQUIRE) != 0;
    if (gLive && output && !wasOutput) {
        gIoSound = gLive->sound_periods;
        gIoUnderruns = gLive->underruns;
        gIoSkipped = gLive->skipped;
        gIoPolls = gLive->status_polls;
        gIoDropped = gShared->dropped;
        gIoWarm = gLive->spk_state == LiveRunning && __atomic_load_n(&gShared->accepting, __ATOMIC_ACQUIRE);
        gTickMaxUs = gSliceMaxUs = 0;
        piano_audio_log("live: IO window start: warm %u; monitor-slice1:refill-first", unsigned(gIoWarm));
    } else if (gLive && !output && wasOutput) {
        piano_audio_log("live: IO window stop: warm %u sound %u underruns %u skips %u fifo-dropped %llu polls %u tick-max-us %u slice-tick-max-us %u",
                        unsigned(gIoWarm), gLive->sound_periods - gIoSound, gLive->underruns - gIoUnderruns,
                        gLive->skipped - gIoSkipped, (unsigned long long)(gShared->dropped - gIoDropped),
                        gLive->status_polls - gIoPolls, gTickMaxUs, gSliceMaxUs);
    }
    __atomic_store_n(&gShared->boundary_ms,nowMs(),__ATOMIC_RELEASE);
    audio_live_running(gShared, output, input);
    if (input && !wasInput) {
        gMicHalReportAt = nowMs();
        gMicHalControls = __atomic_load_n(&gShared->input_controls, __ATOMIC_ACQUIRE);
        micHalReport("start");
    }
    liveWake();
}
uint32_t dspPlay(void *, const int16_t *stereo, uint32_t frames)
{
    int wake = 0;
    const uint32_t n = audio_live_play(gShared, stereo, frames, nowMs(), &wake);
    if (wake) liveWake();
    return n;
}
uint32_t dspCapture(void *, int16_t *mono, uint32_t frames)
{
    const bool first = !gShared->input_hal.first_delivery;
    const uint32_t n = audio_live_capture(gShared, mono, frames);
    const uint32_t now = nowMs(), controls = __atomic_load_n(&gShared->input_controls, __ATOMIC_ACQUIRE);
    if (first && n) piano_audio_log("mic HAL first: run %u host-ms %u frames %u", gShared->input_runs, now, n);
    if (now - gMicHalReportAt >= 1000 || controls != gMicHalControls) {
        micHalReport(controls != gMicHalControls ? "control" : "tick");
        gMicHalReportAt = now; gMicHalControls = controls;
    }
    return n;
}
void dspControl(void *, int volume, int mute)
{
    audio_live_control(gShared, volume, mute);
    liveWake();   // an unmute while sound plays brings the speaker path up again
}
void dspInputControl(void *, int level, int mute)
{
    audio_live_input_control(gShared, level, mute);
    piano_audio_log("mic control: input-level %d mute %d gain-q15 %d effective-mute %d contract mic-input1:linear-db:pre-HAL",
        level, mute, __atomic_load_n(&gShared->input_q15, __ATOMIC_ACQUIRE), __atomic_load_n(&gShared->input_muted, __ATOMIC_ACQUIRE));
}
const PianoPcmTransport kDsp = {"dsp", nullptr, AUDIO_LIVE_OUTPUT_LATENCY, AUDIO_LIVE_INPUT_LATENCY, dspRunning, dspPlay, dspCapture,
                                dspControl, dspInputControl};

void liveSummary(const AudioLive &l, char *line, size_t size)
{
    snprintf(line, size, "level %u: speaker %s (%u starts, %u periods, %u with sound, %u underruns, %u slips), mic %s (%u starts, %u periods); "
             "frames offered %llu, taken %llu, dropped %llu; speaker protection %s (%u starts, %u fallbacks, %u live reads)", l.cfg.level,
             audio_live_state(l.spk_state), l.spk_starts, l.periods, l.sound_periods, l.underruns, l.drops + l.repeats,
             audio_live_state(l.mic_state), l.mic_starts, l.mic_periods, (unsigned long long)l.sh->offered, (unsigned long long)l.sh->taken,
             (unsigned long long)l.sh->dropped, !l.prot_ready ? "not ready" : l.prot_active ? "active" : l.prot_failed ? "fallen back" :
             __atomic_load_n(&l.sh->spk_prot, __ATOMIC_ACQUIRE) ? "on" : "off", l.prot_starts, l.prot_fallbacks, l.live_reads);
}

uint32_t liveWord(const AudioLive &l)
{
    return (uint32_t(l.spk_state) & 0xfu) | (uint32_t(l.mic_state) & 0xfu) << 4 | (uint32_t(l.spk_mode) & 0xffu) << 8 | l.spk_starts << 16;
}

struct sysctl_oid gLedgerOid;
struct sysctl_oid gGainOid;
int gainControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || gLive->cfg.level < 9) return ENXIO;
    int value = int(__atomic_load_n(&gShared->amp_gain_requested, __ATOMIC_ACQUIRE));
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (audio_live_gain_select(gShared, unsigned(value))) return EINVAL;
    piano_audio_log("aud-gain231-v1: requested gain %d (worker applies; active readback follows)", value);
    return 0;
}
// Speaker protection (AudioSpkProt.h): machdep.piano_audio_spk asks for the protection pair (odd: 1, or 1 + the round-2 variant
// bits SPK_VAR_*) or the passthrough graph (0) from the
// next start of the speaker path (it is down after idle_ms without sound); a new request also clears a fallback of this boot.
// machdep.piano_audio_spk_state is the worker's view, one line (read only; plain words and counters, no calibration value).
struct sysctl_oid gSpkOid, gSpkStateOid;
int spkControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || !gShared) return ENXIO;
    int value = __atomic_load_n(&gShared->spk_prot, __ATOMIC_ACQUIRE);
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if ((value & 1) && !gLive->prot_ready) return ENOENT;   // the private files are missing or mismatched (the log says which)
    if (audio_live_spk_select(gShared, value)) return EINVAL;
    piano_audio_log("PianoSpeakerProtV1: request %d (taken at the next start of the speaker path)", value);
    liveWake();
    return 0;
}
int spkState(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) return EPERM;
    if (!gLive || !gShared) return ENXIO;
    const AudioLive &l = *gLive;
    char line[480];
    const int n = snprintf(line, sizeof line, "request %d ready %d active %d failed %d starts %u fallbacks %u live-reads %u vbat-sends %u "
                           "vbat-uv %u speaker %s why %s; miplay request %d ready %d active %d failed %d starts %u fallbacks %u "
                           "preg %d preg-updates %u why %s", __atomic_load_n(&gShared->spk_prot, __ATOMIC_ACQUIRE), l.prot_ready, l.prot_active,
                           l.prot_failed, l.prot_starts, l.prot_fallbacks, l.live_reads, l.vbat_sends, l.vbat_uv, audio_live_state(l.spk_state),
                           l.prot_why ? l.prot_why : "-", __atomic_load_n(&gShared->miplay, __ATOMIC_ACQUIRE), l.mip_ready, l.mip_active,
                           l.mip_failed, l.mip_starts, l.mip_fallbacks, int(l.preg_sent), l.preg_updates, l.mip_why ? l.mip_why : "-");
    return SYSCTL_OUT(req, line, size_t(n < int(sizeof line) ? n : int(sizeof line) - 1) + 1);
}
// spk-probe1 (AudioLive.h audio_live_spk_probe): machdep.piano_audio_spk_probe, write S = 1..30:
// the protection pair up S seconds without sound, the amplifiers left powered down; S + 100: powered up and held muted.
// Writes return ENOENT while protection is not ready. Read: the ms left of the running probe.
// machdep.piano_audio_spk_live (read only): the probe's state and the last live data per amplifier, relative values only.
struct sysctl_oid gSpkProbeOid, gSpkLiveOid;
AudioLiveSpkStatus gSpkLiveStatus;

void publishSpkLive()
{
    AudioLiveSpkStatus status;
    audio_live_spk_status(gLive, &status);
    IOLockLock(gWakeLock);
    gSpkLiveStatus = status;
    IOLockUnlock(gWakeLock);
}

int spkProbeControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || !gShared) return ENXIO;
    IOLockLock(gWakeLock);
    const bool probing = gSpkLiveStatus.probing;
    const uint32_t until = gSpkLiveStatus.until;
    IOLockUnlock(gWakeLock);
    const int32_t left = probing ? int32_t(until - nowMs()) : 0;
    int value = left > 0 ? left : 0;
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (!gLive->cfg.spk_probe) return EPERM;
    if (!gLive->prot_ready) return ENOENT;
    const int queued = audio_live_spk_probe(gShared, uint32_t(value));
    if (queued) return queued;
    piano_audio_log("spk-probe1: request %d (taken at the worker's next tick)", value);
    liveWake();
    return 0;
}
int spkLive(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) return EPERM;
    if (!gLive) return ENXIO;
    IOLockLock(gWakeLock);
    const AudioLiveSpkStatus status = gSpkLiveStatus;
    IOLockUnlock(gWakeLock);
    char line[640];
    const unsigned n = audio_live_spk_live_text(&status, nowMs(), line, sizeof line);
    return SYSCTL_OUT(req, line, size_t(n) + 1);
}
// MIPLAY (AudioSpkProt.h): machdep.piano_audio_miplay asks for the Dolby/MiSound subgraph inside the protection plan from its next start
// (0 off; 1 or 2 the perf mode; +4 the DAP's pregain follows the master volume; +16 x (r + 1) the rotation r); a new request also clears a
// MIPLAY fallback of this boot. Its state is in machdep.piano_audio_spk_state ("miplay ...").
struct sysctl_oid gMiplayOid;
int miplayControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || !gShared) return ENXIO;
    int value = __atomic_load_n(&gShared->miplay, __ATOMIC_ACQUIRE);
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (value && !gLive->mip_ready) return ENOENT;   // its private file is missing or mismatched (the log says which)
    if (audio_live_miplay_select(gShared, value)) return EINVAL;
    piano_audio_log("PianoMiplayV1: request %d (taken at the next start of the protection plan)", value);
    liveWake();
    return 0;
}
void registerSpk()
{
    bzero(&gSpkOid, sizeof gSpkOid);
    gSpkOid.oid_parent = &sysctl__machdep_children; gSpkOid.oid_number = OID_AUTO;
    gSpkOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gSpkOid.oid_name = "piano_audio_spk"; gSpkOid.oid_handler = spkControl;
    gSpkOid.oid_fmt = "I"; gSpkOid.oid_descr = "Speaker protection from the next speaker start: 1 the FSADSP pair, 0 passthrough";
    gSpkOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gSpkOid);
    bzero(&gSpkStateOid, sizeof gSpkStateOid);
    gSpkStateOid.oid_parent = &sysctl__machdep_children; gSpkStateOid.oid_number = OID_AUTO;
    gSpkStateOid.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gSpkStateOid.oid_name = "piano_audio_spk_state"; gSpkStateOid.oid_handler = spkState;
    gSpkStateOid.oid_fmt = "A"; gSpkStateOid.oid_descr = "PianoSpeakerProtV1: request, readiness, fallbacks, live reads";
    gSpkStateOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gSpkStateOid);
    bzero(&gMiplayOid, sizeof gMiplayOid);
    gMiplayOid.oid_parent = &sysctl__machdep_children; gMiplayOid.oid_number = OID_AUTO;
    gMiplayOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gMiplayOid.oid_name = "piano_audio_miplay"; gMiplayOid.oid_handler = miplayControl;
    gMiplayOid.oid_fmt = "I";
    gMiplayOid.oid_descr = "MIPLAY (Dolby) inside speaker protection from its next start: 0 off, 1/2 perf mode, +4 pregain tracked, +16(r+1) rotation r";
    gMiplayOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gMiplayOid);
    if (!gLive->cfg.spk_probe || gLive->cfg.level < 9) return;
    publishSpkLive();
    bzero(&gSpkLiveOid, sizeof gSpkLiveOid);
    gSpkLiveOid.oid_parent = &sysctl__machdep_children; gSpkLiveOid.oid_number = OID_AUTO;
    gSpkLiveOid.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gSpkLiveOid.oid_name = "piano_audio_spk_live"; gSpkLiveOid.oid_handler = spkLive;
    gSpkLiveOid.oid_fmt = "A"; gSpkLiveOid.oid_descr = "spk-probe1: probe state and per-amp live data (permille of Re25, f0, Q, 0.1 C)";
    gSpkLiveOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gSpkLiveOid);
    bzero(&gSpkProbeOid, sizeof gSpkProbeOid);
    gSpkProbeOid.oid_parent = &sysctl__machdep_children; gSpkProbeOid.oid_number = OID_AUTO;
    gSpkProbeOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gSpkProbeOid.oid_name = "piano_audio_spk_probe"; gSpkProbeOid.oid_handler = spkProbeControl;
    gSpkProbeOid.oid_fmt = "I"; gSpkProbeOid.oid_descr = "spk-probe1: protection pair S s without sound (1..30; +100 amps up muted)";
    gSpkProbeOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gSpkProbeOid);
}
// duplex-hold1 (A/B switch, private DUPLEX-VOTES.txt; AudioLive.h audio_live_hold_select): machdep.piano_audio_duplex_hold 0 off, 1 the
// LPASS macro and digital codec votes, 3 also the VA (TX core) mclk, held by their own plan from before the first path starts until both
// are down; taken when both paths are down and one starts. Each speaker start logs "duplex-hold1: speaker start N: hold H ...".
struct sysctl_oid gHoldOid;
int holdControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || !gShared) return ENXIO;
    int value = __atomic_load_n(&gShared->duplex_hold, __ATOMIC_ACQUIRE);
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (audio_live_hold_select(gShared, value)) return EINVAL;
    piano_audio_log("duplex-hold1: request %d (taken when both paths are down and one starts; speaker %s, Mic %s)", value,
                    audio_live_state(gLive->spk_state), audio_live_state(gLive->mic_state));
    liveWake();
    return 0;
}
void registerHold()
{
    bzero(&gHoldOid, sizeof gHoldOid);
    gHoldOid.oid_parent = &sysctl__machdep_children; gHoldOid.oid_number = OID_AUTO;
    gHoldOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gHoldOid.oid_name = "piano_audio_duplex_hold"; gHoldOid.oid_handler = holdControl;
    gHoldOid.oid_fmt = "I";
    gHoldOid.oid_descr = "duplex-hold1 A/B: 0 off, 1 hold the LPASS macro+dcodec votes, 3 also the VA mclk, from before the first path to both down";
    gHoldOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gHoldOid);
}
// duplex-age1 (A/B switch, private DUPLEX-R3.md): machdep.piano_audio_age 1 the signed sound age (the fix, default), 0 the old unsigned
// test that faded a playing speaker out and in whenever the engine stamped its sound a millisecond after the worker's tick.
struct sysctl_oid gAgeOid;
int ageControl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (!gLive || !gShared) return ENXIO;
    int value = __atomic_load_n(&gShared->age_signed, __ATOMIC_ACQUIRE);
    const int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (audio_live_age_select(gShared, value)) return EINVAL;
    piano_audio_log("duplex-age1: request %d (%s sound age from the next tick)", value, value ? "signed" : "the OLD unsigned");
    return 0;
}
void registerAge()
{
    bzero(&gAgeOid, sizeof gAgeOid);
    gAgeOid.oid_parent = &sysctl__machdep_children; gAgeOid.oid_number = OID_AUTO;
    gAgeOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gAgeOid.oid_name = "piano_audio_age"; gAgeOid.oid_handler = ageControl;
    gAgeOid.oid_fmt = "I";
    gAgeOid.oid_descr = "duplex-age1 A/B: 1 signed sound age (default), 0 the old unsigned test";
    gAgeOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gAgeOid);
}
void registerGain()
{
    bzero(&gGainOid, sizeof gGainOid);
    gGainOid.oid_parent = &sysctl__machdep_children; gGainOid.oid_number = OID_AUTO;
    gGainOid.oid_kind = int(CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2);
    gGainOid.oid_name = "piano_audio_gain"; gGainOid.oid_handler = gainControl;
    gGainOid.oid_fmt = "I"; gGainOid.oid_descr = "Daily FS19xx request: 192, 231, or 243 (applied only under speaker protection, else 231); aud-gain231-v1";
    gGainOid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gGainOid);
}
// Route writes are requests only. The worker publishes applied settings and
// the stopped boundary. The raw window's arm and copy share a user-side lock;
// the worker fills only armed backing and publishes it once, immutably.
enum MicRouteOid {MicRouteValue, MicRouteDiag, MicRouteRecord, MicRouteState, MicRoutePcm, MicRouteCount};
struct sysctl_oid gMicRouteOid[MicRouteCount];
IOLock *gMicRecordLock;
AudioMicWindow *gMicWindow;

int micRouteControl(struct sysctl_oid *oidp, void *, int selector, struct sysctl_req *req)
{
    if (!gLive || !gLive->cfg.mic) return ENXIO;
    const AudioMicRouteStatus s = audio_live_mic_route_status(gShared);
    if (selector == MicRouteState) {
        if (req->newptr) return EPERM;
        char line[160];
        const int n = snprintf(line, sizeof line, "request %d applied %d gen %u diag-request %d diag-applied %d stopped %d armed %d recorded %d",
                               s.requested, s.applied, s.generation, s.diag_requested, s.diag_applied, s.stopped, s.armed, s.recorded);
        return SYSCTL_OUT(req, line, size_t(n) + 1);
    }
    if (selector == MicRoutePcm) {
        if (req->newptr || !kauth_cred_issuser(kauth_cred_get())) return EPERM;
        IOLockLock(gMicRecordLock);
        const AudioMicWindow *w = audio_live_mic_window(gLive);
        int err = 0;
        if (w) {
            err = SYSCTL_OUT(req, w, offsetof(AudioMicWindow, period));
            if (!err) err = SYSCTL_OUT(req, w->period, w->periods * sizeof w->period[0]);
            if (!err) err = SYSCTL_OUT(req, w->pcm, w->frames * 2u * sizeof w->pcm[0]);
        }
        IOLockUnlock(gMicRecordLock);
        return err;
    }
    if (selector == MicRouteRecord && !kauth_cred_issuser(kauth_cred_get())) return EPERM;
    int value = selector == MicRouteValue ? s.requested : selector == MicRouteDiag ? s.diag_requested : s.armed;
    int err = sysctl_handle_int(oidp, &value, 0, req);
    if (err || !req->newptr) return err;
    if (selector == MicRouteRecord) {
        IOLockLock(gMicRecordLock);
        if (value == 1 && !gMicWindow && s.stopped && s.diag_applied)
            gMicWindow = static_cast<AudioMicWindow *>(IOMallocData(sizeof(AudioMicWindow)));
        err = audio_live_mic_record(gLive, value, gMicWindow);
        IOLockUnlock(gMicRecordLock);
    } else {
        err = audio_live_mic_request(gShared, selector == MicRouteDiag, value);
    }
    if (!err) liveWake();
    piano_audio_log("mic-route1: submit %s value %d result %d", oidp->oid_name, value, err);
    return err;
}

void registerMicRoute()
{
    static const char *const names[] = {
        "piano_audio_mic_route", "piano_audio_mic_route_diag", "piano_audio_mic_route_record",
        "piano_audio_mic_route_state", "piano_audio_mic_route_pcm",
    };
    gMicRecordLock = IOLockAlloc();
    if (!gMicRecordLock) piano_audio_log("mic-route1: pre-mix diagnostic lock allocation failed");
    for (int i = 0; i < MicRouteCount; ++i) {
        if (!gMicRecordLock && (i == MicRouteRecord || i == MicRoutePcm)) continue;
        sysctl_oid &oid = gMicRouteOid[i];
        oid.oid_parent = &sysctl__machdep_children;
        oid.oid_number = OID_AUTO;
        const unsigned type = i == MicRouteState ? CTLTYPE_STRING : i == MicRoutePcm ? CTLTYPE_OPAQUE : CTLTYPE_INT;
        oid.oid_kind = int(type | (i < MicRouteState ? CTLFLAG_RW : CTLFLAG_RD) | CTLFLAG_LOCKED | CTLFLAG_OID2);
        oid.oid_name = names[i];
        oid.oid_arg2 = i;
        oid.oid_handler = micRouteControl;
        oid.oid_fmt = i == MicRouteState ? "A" : i == MicRoutePcm ? "S,AudioMicWindow" : "I";
        oid.oid_version = SYSCTL_OID_VERSION;
        sysctl_register_oid(&oid);
    }
}

int ledgerRead(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) return EPERM;
    if (!gLive) return ENXIO;
    constexpr size_t cap=100000;
    char *buf=static_cast<char *>(IOMallocData(cap));
    if (!buf) return ENOMEM;
    const AudioLedger *l=gLive->ledger;
    if (!l) {IOFreeData(buf,cap); return ENXIO;}
    const uint64_t n=__atomic_load_n(&l->written,__ATOMIC_ACQUIRE), first=n>AUDIO_LEDGER_CAP?n-AUDIO_LEDGER_CAP:0;
    size_t used=size_t(snprintf(buf,cap,"speaker-ledger1 written=%llu overwritten=%llu capacity=128\n"
        "seq,event,ms,epoch,mode,want_out,fill_before,fill_after,want,got,missing,producer_ms,producer_frames,producer_late_us,sent,returned,inflight,done_batch,monitor_start_ms,monitor_end_ms,monitor_field,skips,drops,repeats,slip,hal_sample,hal_host,hal_writeEnd,missing_total,boundary_ms,sleep_gen,state,U\n",
        (unsigned long long)n,(unsigned long long)first));
    int err=0;
    for (uint64_t i=first;i<n;++i) {
        uint64_t w[AUDIO_LEDGER_WORDS];
        if (!audio_ledger_get(l,i,w)) {err=EAGAIN; break;}
        used+=size_t(snprintf(buf+used,cap-used,"%llu",(unsigned long long)i));
        for (unsigned k=0;k<AUDIO_LEDGER_WORDS;++k)
            used+=size_t(snprintf(buf+used,cap-used,",%llu",(unsigned long long)w[k]));
        used+=size_t(snprintf(buf+used,cap-used,"\n"));
    }
    if (!err) err=SYSCTL_OUT(req,buf,used+1);
    IOFreeData(buf,cap);
    return err;
}
void registerLedger()
{
    bzero(&gLedgerOid,sizeof gLedgerOid);
    gLedgerOid.oid_parent=&sysctl__machdep_children; gLedgerOid.oid_number=OID_AUTO;
    gLedgerOid.oid_kind=int(CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_OID2);
    gLedgerOid.oid_name="piano_audio_ledger"; gLedgerOid.oid_handler=ledgerRead;
    gLedgerOid.oid_fmt="A"; gLedgerOid.oid_descr="Bounded speaker refill event ledger (no PCM); sleep-contract1";
    gLedgerOid.oid_version=SYSCTL_OID_VERSION;
    sysctl_register_oid(&gLedgerOid);
}

// For the rest of the boot (until PianoPower's health stops it): the live paths on the session's hardware.
void liveRun(const PianoAudioLevelArgs *a)
{
    AudioLiveConfig lc;
    audio_live_defaults(&lc, a->level);
    if (a->liveGain) lc.gain = a->liveGain;
    if (a->liveCeilingDb) lc.ceiling = audio_ceiling_db(a->liveCeilingDb);
    if (a->liveIdleMs) lc.idle_ms = a->liveIdleMs;
    lc.mic_route = a->micRoute;
    lc.mic_diag = a->micRouteDiag;
    lc.mic_route_apply = pianoaudio::boardSetMicRoute;
    lc.spk_prot = int(a->spk);   // 0, or the round-2 mask (AudioSpkProt.h SPK_VAR_*), checked by PianoAudio.cpp and AudioLive
    lc.age_old = int(a->ageOld);   // duplex-age1 A/B control
    lc.miplay = int(a->miplay);   // MIPLAY: 0, or AudioSpkProt.h's request (checked by PianoAudio.cpp and AudioLive)
    lc.spk_probe = int(a->spkProbe);   // spk-probe1: 0 ignores machdep.piano_audio_spk_probe, 1 takes it
    lc.duplex_hold = int(a->duplexHold);   // duplex-hold1: 0, 1 or 3 (checked by PianoAudio.cpp and AudioLive)
    static AudioLiveSpk *spkState;   // kept for the boot, as gLive
    if (!spkState) spkState = IOMallocType(AudioLiveSpk);
    if (spkState) bzero(spkState, sizeof *spkState);
    lc.spk_state = spkState;
    gLive = IOMallocType(AudioLive);
    if (!gLive || audio_live_init(gLive, &lc, &gSession, gShared)) {
        piano_audio_log("live: levels 8-10 not run: %s", gLive ? "the session did not hand the hardware over" : "no memory");
        if (gLive) IOFreeType(gLive, AudioLive);
        gLive = nullptr;
        return;
    }
    gLive->ledger=static_cast<AudioLedger *>(IOMallocData(sizeof(AudioLedger)));
    if (gLive->ledger) {bzero(gLive->ledger,sizeof(AudioLedger)); registerLedger();}
    else {piano_audio_log("sleep-contract1: ledger allocation failed; readiness gate held"); return;}
    if (lc.level >= 9) registerGain();
    registerSpk();
    registerAge();
    if (lc.mic) {registerMicRoute(); registerHold();}
    // The microphone tap for "Mic (noise reduced)" (AudioTap.h, MIC-NS section 8): published once the capture path can run.
    if (lc.mic && a->tap) {
        const int tap = piano_audio_tap_publish(gShared, liveWake);
        piano_audio_log("tap1: %s (service %s, user client %s, %u frames)", tap ? "NOT published (no memory)" : "published",
                        PIANO_TAP_SERVICE, PIANO_TAP_USER_CLIENT, PIANO_TAP_FRAMES);
    } else if (lc.mic) {
        piano_audio_log("tap1: off (piano-audio-tap=0)");
    }
    piano_audio_power_ready();
    uint32_t handled=0;
    uint8_t in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    char line[300] = "", last[300] = "";
    uint32_t lastWord = ~0u, registryAt = 0;
    while (!gLive->finished) {
        const uint32_t now = nowMs();
        bzero(&h,sizeof h); piano_adsp_health(&h); observe(o,h);
        const uint32_t request=__atomic_load_n(&gPmRequest,__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&gPmFailed,__ATOMIC_ACQUIRE)) gLive->sleep_failed=1;
        if (request!=handled && !gLive->sleep_failed) {
            if (__atomic_load_n(&gPmSleep,__ATOMIC_ACQUIRE)) {
                if (!gLive->sleeping) {
                    if (piano_audio_devices_power(1)) audio_live_sleep_fail(gLive);
                    else audio_live_sleep(gLive,++gPmGeneration);
                    piano_audio_log("sleep-contract1: drain gen %u periods %u U %u",gPmGeneration,gLive->periods,gLive->underruns);
                }
            } else if (gLive->parked && adg_observe(gLive->guard,&o,now)==AdgRun) {
                if (audio_live_rearm(gLive) || piano_audio_devices_power(0)) gLive->sleep_failed=1;
                else {
                    piano_audio_log("sleep-contract1: on-ready gen %u periods %u U %u",gPmGeneration,gLive->periods,gLive->underruns);
                    __atomic_store_n(&gPmResult,0,__ATOMIC_RELEASE);
                    __atomic_store_n(&gPmAck,request,__ATOMIC_RELEASE); handled=request;
                }
            } else if (!gLive->parked || gLive->guard->state==AdgOff) gLive->sleep_failed=1;
        }
        for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_live_receive(gLive, in, n, now);
        const uint32_t slices = gLive->status_slices;
        const bool playing = gLive->unmuted;
        const uint64_t tickStart = mach_absolute_time();
        const uint32_t d = audio_live_tick(gLive, &o, now);
        const uint32_t us = elapsedUs(tickStart);
        if (playing && us > gTickMaxUs) gTickMaxUs = us;
        if (slices != gLive->status_slices && us > gSliceMaxUs) gSliceMaxUs = us;
        if (gLive->sleep_failed) audio_live_sleep_fail(gLive);
        if (request!=handled && __atomic_load_n(&gPmSleep,__ATOMIC_ACQUIRE)) {
            const int parked=audio_live_parked(gLive);
            if (parked!=0) {
                if (parked<0) audio_live_sleep_fail(gLive);
                piano_audio_log("sleep-contract1: %s gen %u periods %u U %u",parked==1?"off-ready":"off-unsafe",gPmGeneration,gLive->periods,gLive->underruns);
                __atomic_store_n(&gPmResult,parked==1?0:-1,__ATOMIC_RELEASE);
                __atomic_store_n(&gPmAck,request,__ATOMIC_RELEASE); handled=request;
            }
        }
        if (lc.spk_probe) publishSpkLive();
        const uint32_t word = liveWord(*gLive);
        piano_audio_live_word = word;
        if (word != lastWord || (lc.mic && now - registryAt >= 1000)) {
            liveSummary(*gLive, line, sizeof line);
            if (strcmp(line, last)) piano_audio_set_property("piano-audio-live", line);
            strlcpy(last, line, sizeof last);
            lastWord = word;
            registryAt = now;
        }
        if (!d) break;
        liveSleep(gLive->sleeping && d>5 ? 5 : d);
    }
    liveSummary(*gLive, line, sizeof line);
    piano_audio_set_property("piano-audio-live", line);
    piano_audio_log("live: %s", line);
}

}  // namespace

extern "C" void piano_audio_producer_trace(uint32_t frames, uint32_t late, uint64_t sample, uint64_t host, uint64_t end)
{
    if (!gShared) return;
    __atomic_add_fetch(&gShared->producer_seq,1u,__ATOMIC_SEQ_CST);
    __atomic_store_n(&gShared->producer_frames,frames,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->producer_late_us,late,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_sample,sample,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_host,host,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->hal_write_end,end,__ATOMIC_RELEASE);
    __atomic_store_n(&gShared->producer_ms,nowMs(),__ATOMIC_RELEASE);
    __atomic_add_fetch(&gShared->producer_seq,1u,__ATOMIC_SEQ_CST);
}

extern "C" int piano_audio_power_request(int sleep)
{
    if (!gLive || __atomic_load_n(&gPmFailed,__ATOMIC_ACQUIRE)) return -1;
    __atomic_store_n(&gPmSleep,sleep?1u:0u,__ATOMIC_RELEASE);
    const uint32_t n=__atomic_add_fetch(&gPmRequest,1u,__ATOMIC_ACQ_REL);
    liveWake();
    const uint32_t start=nowMs();
    while (nowMs()-start<8000) {
        if (__atomic_load_n(&gPmAck,__ATOMIC_ACQUIRE)==n) return __atomic_load_n(&gPmResult,__ATOMIC_ACQUIRE);
        IOSleep(10);
    }
    __atomic_store_n(&gPmFailed,1,__ATOMIC_RELEASE);
    liveWake();
    return -1;
}

extern "C" const PianoPcmTransport *piano_pcm_dsp(void) {return gShared ? &kDsp : nullptr;}

extern "C" int piano_audio_live_prepare(uint32_t volume_percent)
{
    if (gShared) return 0;
    gWakeLock = IOLockAlloc();
    AudioLiveShared *sh = gWakeLock ? IOMallocType(AudioLiveShared) : nullptr;
    int16_t *out = sh ? static_cast<int16_t *>(IOMallocData(AUDIO_LIVE_RING_BYTES)) : nullptr;
    int16_t *in = out ? static_cast<int16_t *>(IOMallocData(AUDIO_LIVE_MIC_RING_BYTES)) : nullptr;
    if (!in) {   // kept for the boot otherwise (the devices' engines may run at any time)
        if (out) IOFreeData(out, AUDIO_LIVE_RING_BYTES);
        if (sh) IOFreeType(sh, AudioLiveShared);
        return -1;
    }
    bzero(out, AUDIO_LIVE_RING_BYTES);
    bzero(in, AUDIO_LIVE_MIC_RING_BYTES);
    audio_live_shared_init(sh, out, in, audio_volume_q15(volume_percent > 100 ? 100 : int(volume_percent)));
    gShared = sh;
    return 0;
}

int piano_audio_levels(const PianoAudioLevelArgs *a, char *summary, size_t size)
{
    AudioSessionConfig c;
    audio_session_defaults(&c);
    c.level = a->level > 7 ? 7 : a->level;
    c.keep = a->level >= 8 && gShared;
    c.dsp = a->dsp && gChannel && gLink.state == AlReady;
    c.ports = gLink.set >= 0 ? gLink.set : AudioPortsUbuntu;
    c.lpass_read = int(a->lpassRead);
    c.preset_any = int(a->presetAny);
    c.mic = int(a->mic);
    c.silence_ms = a->silenceMs;
    c.tone = a->tone == 1 ? 1 : 0;
    c.tone_mask = a->toneMask;
    c.tone_ms = a->toneMs;
    c.gap_ms = a->gapMs;
    c.tone_gain = a->toneGain;
    AudioSessionOps ops = {};
    ops.clock_ms = [](void *) -> uint32_t {return nowMs();};
    ops.log = sessionLog;
    ops.send = sessionSend;
    const pianoaudio::BoardArgs b = {a->level, a->i2cSwap, a->i2cHalfUs, a->smmu, a->micRoute, a->micGain};
    pianoaudio::boardSetup(ops, b);
    audio_session_init(&gSession, &c, &ops, c.dsp ? &gLink.guard : nullptr);
    // A budget well beyond what the levels need (level 6's silence and level 7's walk are seconds; every command is answered or
    // times out in 2 s): past it the session is stopped, with its undo commands first, then without.
    const uint32_t start = nowMs(), budget = 180000u + (c.silence_ms < AUDIO_SILENCE_MAX_MS ? c.silence_ms : AUDIO_SILENCE_MAX_MS);
    uint32_t aborted = 0;
    uint8_t in[AUDIO_INBOX_BYTES];
    PianoAdspHealth h;
    AdgObservation o;
    while (!gSession.finished) {
        const uint32_t now = nowMs();
        if (c.dsp) {
            bzero(&h, sizeof h);
            piano_adsp_health(&h);
            observe(o, h);
            for (uint32_t n; (n = audio_inbox_get(&gInbox, in, sizeof in)) != 0;) audio_session_receive(&gSession, in, n, now);
        }
        const uint32_t d = audio_session_tick(&gSession, c.dsp ? &o : nullptr, now);
        piano_audio_levels_word = levelsWord(gSession);
        if (!d) break;
        if (!aborted && now - start > budget) {
            aborted = now;
            audio_session_abort(&gSession, 0, now);
        } else if (aborted && now - aborted > 30000u) {
            audio_session_abort(&gSession, 1, now);
        }
        IOSleep(d);
    }
    piano_audio_levels_word = levelsWord(gSession);
    char board[400];
    pianoaudio::boardSummary(board, sizeof board);
    size_t n = 0;
    int failed = 0;
    for (unsigned l = 4; l <= c.level && l <= 7 && n < size; ++l) {
        n += size_t(snprintf(summary + n, size - n, "%s%u %s", l > 4 ? ", " : "", l, audio_level_result(gSession.result[l])));
        failed |= gSession.result[l] == AudioLevelFailed;
    }
    piano_audio_log("levels: %s; %s", summary, board);
    if (a->level >= 8) {
        if (gSession.kept) {
            piano_audio_set_property("piano-audio-levels", summary);
            piano_audio_log("levels 8-%u: the macOS devices on the DSP (the hardware of levels 5 and 6 kept)", a->level);
            liveRun(a);
        } else {
            piano_audio_log("levels 8-%u: not run: %s", a->level, !gShared ? "no dsp transport" : "levels 4, 5 and 6 must pass in the same boot");
        }
        if (n < size) n += size_t(snprintf(summary + n, size - n, ", 8-%u %s", a->level, gSession.kept ? "ran" : "not run"));
        failed |= !gSession.kept;
    }
    pianoaudio::boardTeardown();
    return failed ? -1 : 0;
}
