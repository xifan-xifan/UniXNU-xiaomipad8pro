/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Levels 8-10, the macOS devices on the DSP: see AudioLive.h. */
#include "AudioLive.h"
#include <sys/errno.h>
#include "PianoGpr.h"

#define LOG(l, ...) do {if ((l)->ops.log) (l)->ops.log((l)->ops.ctx, __VA_ARGS__);} while (0)
/* The kext allocates both with IOMallocType, whose typed zones end at 16 KiB: a host build fails here first. */
_Static_assert(sizeof(AudioLive) <= 16u * 1024u, "AudioLive exceeds IOMallocType's 16 KiB");
_Static_assert(sizeof(AudioLiveSpk) <= 16u * 1024u, "AudioLiveSpk exceeds IOMallocType's 16 KiB");

static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}
static unsigned rd32(const uint8_t *p) {return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;}
static int load(const int *p) {return __atomic_load_n(p, __ATOMIC_ACQUIRE);}
static void store(int *p, int v) {__atomic_store_n(p, v, __ATOMIC_RELEASE);}

static void ledger(AudioLive *l, unsigned event, uint32_t before, uint32_t after, uint32_t want, uint32_t got, int slip)
{
    if (!l->ledger) return;
    const AudioLiveShared *s = l->sh;
    uint64_t w[AUDIO_LEDGER_WORDS] = {0};
    w[0]=event; w[1]=l->now; w[2]=__atomic_load_n(&s->epoch,__ATOMIC_ACQUIRE);
    w[3]=l->spk_mode; w[4]=load(&s->want_out); w[5]=before; w[6]=after; w[7]=want; w[8]=got; w[9]=want-got;
    const uint32_t producer_seq=__atomic_load_n(&s->producer_seq,__ATOMIC_SEQ_CST);
    w[10]=__atomic_load_n(&s->producer_ms,__ATOMIC_ACQUIRE);
    w[11]=__atomic_load_n(&s->producer_frames,__ATOMIC_ACQUIRE);
    w[12]=__atomic_load_n(&s->producer_late_us,__ATOMIC_ACQUIRE);
    w[13]=l->pump.sent; w[14]=l->pump.returned; w[15]=l->pump.in_flight; w[16]=l->done_batch;
    w[17]=l->monitor_start; w[18]=l->monitor_end; w[19]=l->monitor_field;
    w[20]=l->skipped; w[21]=l->drops; w[22]=l->repeats; w[23]=(uint64_t)(int64_t)slip;
    w[24]=__atomic_load_n(&s->hal_sample,__ATOMIC_ACQUIRE); w[25]=__atomic_load_n(&s->hal_host,__ATOMIC_ACQUIRE);
    w[26]=__atomic_load_n(&s->hal_write_end,__ATOMIC_ACQUIRE); w[27]=s->out.underruns;
    if ((producer_seq&1u) || producer_seq!=__atomic_load_n(&s->producer_seq,__ATOMIC_SEQ_CST)) {
        w[10]=w[11]=w[24]=w[25]=w[26]=0; w[12]=UINT32_MAX;
    }
    w[28]=__atomic_load_n(&s->boundary_ms,__ATOMIC_ACQUIRE); w[29]=l->sleep_generation;
    w[30]=l->spk_state; w[31]=l->underruns;
    audio_ledger_put(l->ledger,w);
}

/* ------------------------------------------------------------------------------------------------ levels in dB, as Q15 */
/* The IOAudio2 volume control and the DSP use the same scalar curve: -36 dB
 * at zero, 0 dB at unity, with dB proportional to (1-scalar)^2.  This keeps
 * selector-2 scalar and decibel writes on the same speaker gain path while
 * avoiding the -50 dB floor that made ordinary slider positions too quiet. */
static const int16_t kVolumeQ15[101] = {
    0, 564, 612, 663, 719, 778, 841, 909, 982, 1059, 1141, 1229, 1323, 1422, 1528, 1640, 1759, 1885, 2019, 2160,
    2309, 2466, 2632, 2807, 2991, 3184, 3387, 3599, 3822, 4056, 4300, 4555, 4821, 5098, 5387, 5688, 6000, 6324, 6661, 7009,
    7369, 7742, 8127, 8523, 8932, 9353, 9785, 10229, 10684, 11150, 11626, 12113, 12610, 13116, 13632, 14156, 14688, 15227, 15773, 16325,
    16882, 17444, 18010, 18579, 19149, 19721, 20293, 20865, 21435, 22002, 22565, 23124, 23676, 24222, 24760, 25289, 25808, 26316, 26811, 27293,
    27761, 28213, 28649, 29068, 29468, 29849, 30210, 30550, 30869, 31164, 31437, 31685, 31909, 32108, 32282, 32429, 32550, 32645, 32713, 32753, 32767};

/* The safety-chain ceiling remains a conventional half-dB table, independent
 * of the user-volume curve above. */
static const int16_t kHalfDb[101] = {
    32767, 30934, 29204, 27570, 26028, 24572, 23197, 21900, 20675, 19518, 18426, 17395, 16422, 15504, 14636, 13818, 13045, 12315,
    11626, 10976, 10362, 9782, 9235, 8718, 8231, 7770, 7336, 6925, 6538, 6172, 5827, 5501, 5193, 4903, 4628, 4370, 4125, 3894, 3677,
    3471, 3277, 3093, 2920, 2757, 2603, 2457, 2320, 2190, 2067, 1952, 1843, 1740, 1642, 1550, 1464, 1382, 1304, 1232, 1163, 1098,
    1036, 978, 923, 872, 823, 777, 734, 693, 654, 617, 583, 550, 519, 490, 463, 437, 413, 389, 368, 347, 328, 309, 292, 276, 260,
    246, 232, 219, 207, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 110, 104};

int32_t audio_volume_q15(int percent)
{
    if (percent <= 0) return 0;
    if (percent >= 100) return AUDIO_Q15_ONE;
    return kVolumeQ15[percent];
}

int32_t audio_ceiling_db(unsigned db)
{
    if (db < 1 || db > 40) return AUDIO_CEILING_TEST;
    const int32_t c = kHalfDb[2 * db];
    return c > AUDIO_CEILING_MAX ? AUDIO_CEILING_MAX : c;
}

/* ------------------------------------------------------------------------------------------------ the two clocks */
/* The ring's fill, sampled once a period where its owner reads or writes it, is noisy by up to a device period (480 frames: where
 * the other side's last 10 ms landed) and biased by where in that cycle the sample falls; so its slow average (1/64 a period) is held
 * at the level it settled at over the first 64 periods, give or take AUDIO_LIVE_SLIP_BAND: beyond, one frame slips a period
 * (+1: one too many, drop one; -1: one too few, repeat one). Only a drifting clock moves the average that far. */
static void drift_reset(AudioLiveDrift *d) {d->avg = 0; d->setpoint = 0; d->samples = 0;}
static int drift_update(AudioLiveDrift *d, uint32_t fill)
{
    const int32_t f = (int32_t)fill * 64;
    d->avg = d->samples ? d->avg + (f - d->avg) / 64 : f;
    if (++d->samples < 64) return 0;
    if (d->samples == 64) {d->setpoint = d->avg; return 0;}
    d->samples = 65;
    if (d->avg > d->setpoint + (int32_t)AUDIO_LIVE_SLIP_BAND * 64) {d->avg -= 64; return 1;}
    if (d->avg < d->setpoint - (int32_t)AUDIO_LIVE_SLIP_BAND * 64) {d->avg += 64; return -1;}
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the devices' side */
void audio_live_shared_init(AudioLiveShared *sh, int16_t *out_storage, int16_t *in_storage, int32_t volume_q15)
{
    zero(sh, sizeof *sh);
    (void)audio_ring_init(&sh->out, out_storage, AUDIO_LIVE_RING, 2);
    (void)audio_ring_init(&sh->in, in_storage, AUDIO_LIVE_MIC_RING, 1);
    sh->volume_q15 = volume_q15 < 0 ? 0 : volume_q15 > AUDIO_Q15_ONE ? AUDIO_Q15_ONE : volume_q15;
    sh->input_q15 = AUDIO_Q15_ONE;
}

/* Requested and applied each encode route+1 in three bits and the diagnostic
 * enable in bit 3. HAL StartIO, owner release and control requests share a
 * linearization point. The owner alone writes applied settings and generation. */
enum {
    MicDiag = 1u << 3, MicSetting = 0xfu, MicAppliedShift = 4,
    MicGraphReleased = 1u << 8, MicEngineRunning = 1u << 9,
    MicWindowArmed = 1u << 10, MicWindowReady = 1u << 11, MicWindowStop = 1u << 12, MicGenerationShift = 32,
};

static uint64_t mic_control(const AudioLiveShared *sh)
{
    return __atomic_load_n(&sh->mic_control, __ATOMIC_ACQUIRE);
}

static int mic_control_update(AudioLiveShared *sh, uint64_t *old, uint64_t next)
{
    return __atomic_compare_exchange_n(&sh->mic_control, old, next, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static int mic_stopped(uint64_t control)
{
    return (control & (MicGraphReleased | MicEngineRunning)) == MicGraphReleased;
}

AudioMicRouteStatus audio_live_mic_route_status(const AudioLiveShared *sh)
{
    const uint64_t control = mic_control(sh), active = control >> MicAppliedShift;
    return (AudioMicRouteStatus){
        (int)(control & 7u) - 1, (int)(active & 7u) - 1, !!(control & MicDiag), !!(active & MicDiag),
        mic_stopped(control), !!(control & MicWindowArmed), !!(control & MicWindowReady), (uint32_t)(control >> MicGenerationShift),
    };
}

int audio_live_mic_request(AudioLiveShared *sh, int diagnostic, int value)
{
    if (diagnostic ? (value != 0 && value != 1) : !audio_mic_route_valid(value)) return EINVAL;
    uint64_t old = mic_control(sh);
    do {
        if (!mic_stopped(old)) return EBUSY;
        const uint64_t mask = diagnostic ? MicDiag : 7u;
        const uint64_t setting = diagnostic ? (uint64_t)value * MicDiag : (uint64_t)(value + 1);
        if ((old & mask) == setting) return 0;
        const uint64_t next = (old & ~(mask | MicWindowArmed | MicWindowReady)) | setting;
        if (mic_control_update(sh, &old, next)) return 0;
    } while (1);
}

int audio_live_mic_record(AudioLive *l, int on, AudioMicWindow *backing)
{
    if (on != 0 && on != 1) return EINVAL;
    uint64_t old = mic_control(l->sh);
    do {
        if (!mic_stopped(old)) return EBUSY;
        if (on) {
            const unsigned setting = old & MicSetting;
            if (setting != ((old >> MicAppliedShift) & MicSetting)) return EBUSY;
            if (!(setting & MicDiag)) return EPERM;
            if (!backing) return ENOMEM;
            AudioMicWindow *allocated = __atomic_load_n(&l->mic_window, __ATOMIC_ACQUIRE);
            if (allocated && allocated != backing) return EINVAL;
            __atomic_store_n(&l->mic_window, backing, __ATOMIC_RELEASE);
        }
        const uint64_t next = (old & ~(uint64_t)(MicWindowArmed | MicWindowReady | MicWindowStop)) | (on ? MicWindowArmed : 0);
        if (mic_control_update(l->sh, &old, next)) return 0;
    } while (1);
}

const AudioMicWindow *audio_live_mic_window(const AudioLive *l)
{
    return (mic_control(l->sh) & MicWindowReady) ? __atomic_load_n(&l->mic_window, __ATOMIC_ACQUIRE) : 0;
}

void audio_live_running(AudioLiveShared *sh, int output, int input)
{
    if (input) __atomic_fetch_or(&sh->mic_control, MicEngineRunning, __ATOMIC_ACQ_REL);
    if (input && !load(&sh->want_in)) {
        zero(&sh->input_hal, sizeof sh->input_hal); /* engine stopped */
        store(&sh->input_primed, 0);
        ++sh->input_runs;
    }
    if (output && !load(&sh->want_out)) {
        /* The engine is stopped here. Publish the old/new PCM boundary, never
         * reset its head from the consumer (a new reader may already be writing). */
        __atomic_store_n(&sh->output_head, __atomic_load_n(&sh->out.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        store(&sh->sound, 0);
        store(&sh->have_sound, 0);
        store(&sh->have_pcm, 0);
        __atomic_store_n(&sh->sound_at, 0u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&sh->epoch, 1u, __ATOMIC_RELEASE);
    }
    store(&sh->want_out, output ? 1 : 0);
    if (!input && load(&sh->want_in)) __atomic_fetch_or(&sh->mic_control, MicWindowStop, __ATOMIC_ACQ_REL);
    store(&sh->want_in, input ? 1 : 0);
    if (!input) __atomic_fetch_and(&sh->mic_control, ~(uint64_t)MicEngineRunning, __ATOMIC_ACQ_REL);
}

uint32_t audio_live_play(AudioLiveShared *sh, const int16_t *stereo, uint32_t frames, uint32_t now_ms, int *wake)
{
    __atomic_store_n(&sh->output_frames, frames, __ATOMIC_RELEASE);
    int any = 0;
    for (uint32_t i = 0; i < 2 * frames; ++i) any |= stereo[i];
    *wake = 0;
    sh->offered += frames;
    if (any) {
        __atomic_store_n(&sh->sound_at, now_ms, __ATOMIC_RELEASE);
        store(&sh->have_sound, 1);
    }
    if (!load(&sh->accepting)) {
        if (any && !__atomic_exchange_n(&sh->sound, 1, __ATOMIC_ACQ_REL)) *wake = 1;   /* the first sound while the path is down */
        sh->dropped += frames;
        return frames;
    }
    const uint32_t n = audio_ring_write(&sh->out, stereo, frames);
    if (any && n) {
        int accepted = n == frames ? any : 0;
        if (n != frames) for (uint32_t i = 0; i < 2u * n; ++i) accepted |= stereo[i];
        if (accepted) store(&sh->have_pcm, 1);
    }
    sh->taken += n;
    sh->dropped += frames - n;
    return frames;
}

uint32_t audio_live_capture(AudioLiveShared *sh, int16_t *mono, uint32_t frames)
{
    AudioCaptureHal *h = &sh->input_hal;
    h->requested += frames;
    if (!load(&sh->capturing)) {
        if (load(&sh->input_fault)) h->unavailable += frames; else h->startup += frames;
        return 0;
    }
    /* A queued DSP request is not PCM already in this FIFO. Keep a real
     * target reserve after the first consumer batch, accounting these zeros
     * as startup rather than pretending capture was primed at graph START. */
    if (!load(&sh->input_primed)) {
        if (audio_ring_fill(&sh->in) < AUDIO_LIVE_MIC_TARGET + frames) {
            h->startup += frames;
            return 0;
        }
        store(&sh->input_primed, 1);
    }
    const uint32_t n = audio_ring_read(&sh->in, mono, frames);
    const int muted = load(&sh->input_muted);
    const int32_t gain = __atomic_load_n(&sh->input_q15, __ATOMIC_ACQUIRE);
    if (n && !h->first_delivery) h->first_delivery = h->requested - frames + 1;
    h->delivered += n;
    h->missing += frames - n;
    if (muted) h->muted += n;
    for (uint32_t i = 0; i < n; ++i) {
        mono[i] = muted ? 0 : (int16_t)((int32_t)mono[i] * gain / AUDIO_Q15_ONE);
        if (muted) sh->input_hal.mute_nonzero += mono[i] != 0;
        audio_capture_sample(&h->pcm, mono[i]);
    }
    sh->captured += n;
    return n;
}

void audio_live_control(AudioLiveShared *sh, int volume_percent, int mute)
{
    if (volume_percent >= 0 && volume_percent <= 100) __atomic_store_n(&sh->volume_q15, audio_volume_q15(volume_percent), __ATOMIC_RELEASE);
    if (mute == 0 || mute == 1) store(&sh->muted, mute);
    sh->controls++;
}

int audio_live_gain_select(AudioLiveShared *sh, unsigned gain)
{
    if (gain != 192u && gain != 231u && gain != FS19XX_GAIN_CAP_PROTECTED) return -1;   /* 243: applied only under protection */
    __atomic_store_n(&sh->amp_gain_requested, gain, __ATOMIC_RELEASE);
    return 0;
}

int audio_live_age_select(AudioLiveShared *sh, int signed_age)
{
    if (signed_age != 0 && signed_age != 1) return -1;
    __atomic_store_n(&sh->age_signed, signed_age, __ATOMIC_RELEASE);
    return 0;
}

int audio_live_spk_select(AudioLiveShared *sh, int on)
{
    if (!spk_request_valid(on)) return -1;   /* 0, or an odd mask of SPK_VAR_* (round 2) */
    store(&sh->spk_prot, on);
    __atomic_add_fetch(&sh->spk_prot_gen, 1u, __ATOMIC_ACQ_REL);
    return 0;
}

int audio_live_miplay_select(AudioLiveShared *sh, int request)
{
    if (!spk_miplay_valid(request)) return -1;   /* 0, or AudioSpkProt.h's MIPLAY request */
    store(&sh->miplay, request);
    __atomic_add_fetch(&sh->miplay_gen, 1u, __ATOMIC_ACQ_REL);
    return 0;
}

int audio_live_hold_select(AudioLiveShared *sh, int mask)
{
    if (!ar_hold_valid(mask)) return -1;   /* 0, 1 (the votes) or 3 (the votes and the mclk) */
    store(&sh->duplex_hold, mask);
    __atomic_add_fetch(&sh->duplex_hold_gen, 1u, __ATOMIC_ACQ_REL);
    return 0;
}

void audio_live_input_control(AudioLiveShared *sh, int level_percent, int mute)
{
    const int32_t gain = piano_audio_input_q15(level_percent);
    if (gain >= 0) __atomic_store_n(&sh->input_q15, gain, __ATOMIC_RELEASE);
    if (mute == 0 || mute == 1) store(&sh->input_muted, mute);
    __atomic_add_fetch(&sh->input_controls, 1, __ATOMIC_RELEASE);
}

int audio_live_tap_client(AudioLiveShared *sh, int delta)
{
    int now = __atomic_add_fetch(&sh->tap_open, delta, __ATOMIC_ACQ_REL);
    if (now < 0) {__atomic_store_n(&sh->tap_open, 0, __ATOMIC_RELEASE); now = 0;}   /* an unmatched close: never below zero */
    if (sh->tap) {
        __atomic_store_n(&sh->tap->clients, (uint32_t)now, __ATOMIC_RELEASE);
        if (delta > 0) __atomic_add_fetch(&sh->tap->opens, 1u, __ATOMIC_RELEASE);
    }
    return now;
}

/* The capture plan runs while the Mic device runs or a tap client is open. */
static int mic_wanted(AudioLiveShared *sh) {return load(&sh->want_in) || (sh->tap && load(&sh->tap_open) > 0);}

/* ------------------------------------------------------------------------------------------------ the configuration */
void audio_live_defaults(AudioLiveConfig *c, uint32_t level)
{
    zero(c, sizeof *c);
    c->level = level < 8 ? 8 : level > 10 ? 10 : level;
    c->gain = c->level >= 9 ? FS19XX_GAIN_CAP : AUDIO_AMPS_TEST_GAIN;
    c->ceiling = c->level >= 9 ? audio_ceiling_db(6) : AUDIO_CEILING_TEST;
    c->idle_ms = AUDIO_LIVE_IDLE_MS;
    c->status_ms = AUDIO_LIVE_STATUS_MS;
    c->answer_ms = AUDIO_ANSWER_MS;
    c->mic = c->level >= 10;
    c->mic_route = AUDIO_MIC_ROUTE_LEGACY;
}

const char *audio_live_state(int state)
{
    switch (state) {
    case LiveDown: return "down";
    case LiveStarting: return "starting";
    case LiveRunning: return "running";
    case LiveStopping: return "stopping";
    case LiveOff: return "OFF for this boot";
    default: return "?";
    }
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case AR_OP_GRAPH_OPEN: return "GRAPH_OPEN";
    case AR_OP_GRAPH_PREPARE: return "GRAPH_PREPARE";
    case AR_OP_GRAPH_START: return "GRAPH_START";
    case AR_OP_GRAPH_STOP: return "GRAPH_STOP";
    case AR_OP_GRAPH_CLOSE: return "GRAPH_CLOSE";
    case AR_OP_SET_CFG: return "SET_CFG";
    case AR_OP_GET_CFG: return "GET_CFG";
    case AR_OP_MEM_MAP: return "MEM_MAP";
    case AR_OP_MEM_UNMAP: return "MEM_UNMAP";
    case AR_OP_PRM_REQUEST: return "PRM REQUEST";
    case AR_OP_PRM_RELEASE: return "PRM RELEASE";
    default: return "?";
    }
}

/* ------------------------------------------------------------------------------------------------ the plans (one command at a time) */
static void plan_start(AudioLive *l, AudioLivePlan *p, const char *name, const ArStep *steps, unsigned count, uint32_t token_base,
                       void *context)
{
    ar_seq_init(&p->seq, steps, count, l->ports->apm, l->ports->prm, token_base, context);
    p->name = name;
    p->active = 1;
    p->in_flight = 0;
    p->unsent = 0;
    p->quiet = 0;
    LOG(l, "live: plan %s: %u steps (APM from port %#x, PRM from port %#x)", name, count, l->ports->apm, l->ports->prm);
}

static int plan_settled(const AudioLivePlan *p)
{
    return !p->in_flight && !p->unsent &&
           (p->seq.state == ArSeqUp || p->seq.state == ArSeqDown || p->seq.state == ArSeqFailed);
}

static int plan_busy(const AudioLivePlan *p) {return p->in_flight || p->unsent;}
/* Another plan than p has a command out or waiting to go (one command at a time over the speaker, mic, monitor and hold plans). */
static int others_busy(const AudioLive *l, const AudioLivePlan *p)
{
    return (p != &l->splan && plan_busy(&l->splan)) || (p != &l->mplan && plan_busy(&l->mplan)) || (p != &l->xplan && plan_busy(&l->xplan)) ||
           (p != &l->hplan && plan_busy(&l->hplan));
}

/* The answer timer, then the next packet of plan p, unless another plan has one out (one command at a time over all) or
 * PianoPower asks for quiet. */
static void plan_send(AudioLive *l, AudioLivePlan *p, uint32_t now)
{
    if (!p->active || l->off || l->sleep_failed) return;
    if (p->in_flight && now - p->sent_at >= l->cfg.answer_ms) {
        if (l->sleeping) {l->sleep_failed=1; return;} /* retain uncertain DMA ownership; cancel instead of abort */
        l->timeouts++;
        LOG(l, "live: dsp: no answer to %s%s within %u ms%s", p->pending_undo ? "the undo of " : "", ar_seq_step_name(&p->seq, p->pending_step),
            l->cfg.answer_ms, p->pending_undo ? " (counted; the undoing goes on)" : ": undoing what was done, that step too");
        p->in_flight = 0;
        ar_seq_timeout(&p->seq);
    }
    if (p->in_flight || l->held || others_busy(l, p)) return;
    if (!p->unsent) {
        const uint32_t n = ar_seq_next(&p->seq, p->pkt, sizeof p->pkt);
        if (!n) return;
        p->unsent = n;
        p->pending_undo = p->seq.pending_undo;
        p->pending_step = p->pending_undo ? (int)p->seq.undo_next : (int)p->seq.done;
        if (!p->quiet)
            LOG(l, "live: dsp: -> %s%s (%s, %u bytes, token %#x, from port %#x)", p->pending_undo ? "undo: " : "",
                ar_seq_step_name(&p->seq, p->pending_step), op_name(p->seq.pending_opcode), n, p->seq.pending_token, p->seq.pending_port);
    }
    if (l->ops.send(l->ops.ctx, p->pkt, p->unsent)) {l->queue_full++; return;}
    p->unsent = 0;
    p->in_flight = 1;
    p->sent_at = now;
    l->sends++;
}

static int plan_receive(AudioLive *l, AudioLivePlan *p, const uint8_t *in, uint32_t n, uint32_t now)
{
    if (!p->active || !p->in_flight) return 0;
    const int r = ar_seq_receive(&p->seq, in, n);
    if (r == ArSeqIgnored) return 0;
    p->in_flight = 0;
    const char *name = ar_seq_step_name(&p->seq, p->pending_step);
    if (r == ArSeqAdvanced && p->quiet) return 1;
    if (r == ArSeqAdvanced) LOG(l, "live: dsp: <- %s%s: done in %u ms", p->pending_undo ? "undo: " : "", name, now - p->sent_at);
    else if (p->pending_undo) LOG(l, "live: dsp: <- undo: %s: status %#x (counted; the undoing goes on)", name, p->seq.last_status);
    else LOG(l, "live: dsp: <- %s: status %#x: undoing what was done", name, p->seq.last_status);
    return 1;
}

static void plan_drop(AudioLivePlan *p)
{
    if (p->active && p->seq.state != ArSeqDown && p->seq.state != ArSeqFailed) {
        ar_seq_abort(&p->seq, ArSeqErrDspDown, 1);
        uint8_t scratch[GPR_MAX_PACKET];
        (void)ar_seq_next(&p->seq, scratch, sizeof scratch);   /* without the DSP: only the local undos run */
    }
    p->in_flight = 0;
    p->unsent = 0;
}

static void dma_zero(AudioLive *l, uint32_t offset, uint32_t bytes)
{
    if (!l->ops.dma || offset + bytes > l->ops.dma_bytes) return;
    volatile uint64_t *d = (volatile uint64_t *)(void *)(l->ops.dma + offset);
    for (uint32_t i = 0; i < bytes / 8u; ++i) d[i] = 0;
    if (l->ops.publish) l->ops.publish(l->ops.ctx);
}

/* ------------------------------------------------------------------------------------------------ duplex-hold1 (A/B switch) */
/* The VA clocks (the LPASS macro and digital codec votes, with 3 also the TX core mclk) in a plan of their own: up before the first
 * path begins, down once both paths are down, so a Mic start or stop while the speaker plays requests or releases no PRM resource
 * (private DUPLEX-VOTES.txt). One owner per vote: never brought up while a Mic run holds its own votes. Off (0) by default. */
#define HOLD "duplex-hold1"
static int path_idle(int state) {return state == LiveDown || state == LiveOff;}

/* Before a path begins (who: "speaker" or "Mic"). 1: begin now (the hold is up, not asked for, or not possible); 0: wait for it. */
static int hold_gate(AudioLive *l, const char *who)
{
    if (l->hold_mask) {
        if (l->hplan.seq.state == ArSeqUp && plan_settled(&l->hplan)) return 1;
        l->hold_demand = 1;   /* coming up (or going down): wait; hold_tick keeps it for this path once up */
        return 0;
    }
    if (!l->cfg.mic) return 1;   /* only the Mic's plan requests these clocks */
    const uint32_t gen = __atomic_load_n(&l->sh->duplex_hold_gen, __ATOMIC_ACQUIRE);
    if (gen != l->hold_gen) {
        l->hold_gen = gen;
        if (l->hold_failed == 1) LOG(l, HOLD ": a new request: the failure of this boot is cleared");
        if (l->hold_failed == 1) l->hold_failed = 0;   /* 2 (an undo failed: a vote may still be held) stays for the boot */
    }
    const int want = load(&l->sh->duplex_hold);
    if (!want || l->hold_failed) return 1;
    if (!path_idle(l->mic_state) && !l->mic_held) return 1;   /* this Mic run holds its own votes: no second owner */
    unsigned count;
    const ArStep *steps = ar_va_hold_steps((unsigned)want, &count);
    if (!steps) return 1;
    l->hold_mask = (unsigned)want;
    l->hold_demand = 1;
    l->hold_starts++;
    LOG(l, HOLD ": hold %d up before the %s starts (hold start %u): %s; the speaker %s", want, who, l->hold_starts,
        want == (int)(AR_HOLD_VOTES | AR_HOLD_MCLK) ? "the LPASS macro and digital codec votes, then the VA mclk 19.2 MHz" :
        "the LPASS macro and digital codec votes (the Mic's plan still requests the mclk)",
        path_idle(l->spk_state) ? "is down (the votes come before its TDM clock)" : "plays (requested while it plays, as without the hold)");
    plan_start(l, &l->hplan, "duplex-hold", steps, count, 0xB000u + (l->sleep_generation << 16), 0);
    return 0;
}

/* Every tick after both paths': the hold's next command, its end, and its release once neither path needs it. */
static void hold_tick(AudioLive *l, uint32_t now)
{
    const int demand = l->hold_demand;   /* a path waited for the hold in this tick: it begins at the next */
    l->hold_demand = 0;
    if (!l->hold_mask) return;
    plan_send(l, &l->hplan, now);
    if (!plan_settled(&l->hplan)) return;
    if (l->hplan.seq.state == ArSeqUp) {
        if (demand || !path_idle(l->spk_state) || l->splan.active || !path_idle(l->mic_state) || l->mplan.active) return;
        LOG(l, HOLD ": both paths down: the held clocks released (hold %u)", l->hold_mask);
        ar_seq_stop(&l->hplan.seq);
        plan_send(l, &l->hplan, now);
        return;
    }
    if (l->hplan.seq.state == ArSeqFailed || l->hplan.seq.undo_errors) {
        l->hold_failed = l->hplan.seq.undo_errors ? 2 : 1;
        l->hold_failures++;
        LOG(l, HOLD ": the hold plan failed at %s (status %#x, %u undo errors): no hold %s; the paths start as without it",
            ar_seq_step_name(&l->hplan.seq, l->hplan.seq.failed_step), l->hplan.seq.error_status, l->hplan.seq.undo_errors,
            l->hold_failed == 2 ? "for this boot" : "until a new request");
    }
    l->hplan.active = 0;
    l->hold_mask = 0;
}

/* ------------------------------------------------------------------------------------------------ the speaker path */
/* 231 on the passthrough graph; 243 (Android's) only while this start is the stock protection pair (Fs19xx.h, AudioSpkProt.h) */
static unsigned live_gain_cap(const AudioLive *l)
{
    return l->cfg.level < 9 ? AUDIO_AMPS_TEST_GAIN : l->prot_active ? FS19XX_GAIN_CAP_PROTECTED : FS19XX_GAIN_CAP;
}
static int spk_epoch_lead(const AudioLive *l) {return __atomic_load_n(&l->sh->output_frames, __ATOMIC_ACQUIRE) != 480u;}
static uint32_t spk_prefill(const AudioLive *l) {return spk_epoch_lead(l) ? AUDIO_LIVE_START_FRAMES : AUDIO_LIVE_TARGET;}
static int spk_same_epoch(const AudioLive *l) {return __atomic_load_n(&l->sh->epoch, __ATOMIC_ACQUIRE) == l->ledger_epoch;}
static int spk_has_prefill(const AudioLive *l) {return spk_same_epoch(l) && load(&l->sh->have_pcm) && audio_ring_fill(&l->sh->out) >= spk_prefill(l);}

static int live_amps_on(void *owner)
{
    AudioLive *l = owner;
    l->cfg.gain = __atomic_load_n(&l->sh->amp_gain_requested, __ATOMIC_ACQUIRE);
    if (l->cfg.gain > live_gain_cap(l)) l->cfg.gain = live_gain_cap(l);
    l->gain_changing = 0;
    const int r = audio_amps_start_capped(l->amps, l->cfg.gain, live_gain_cap(l));
    const int db10 = fs19xx_gain_db10(l->amps->amp[0].gain);
    if (r) LOG(l, "live: amplifiers on FAILED (%s): all four muted and powered down again", audio_amps_error(r));
    else LOG(l, "live: amplifiers on: all four powered up MUTED, gain %u (-%d.%d dB)", l->amps->amp[0].gain, -db10 / 10, -db10 % 10);
    return r;
}

static int live_amps_off(void *owner)
{
    AudioLive *l = owner;
    const unsigned bad = audio_amps_off(l->amps);
    LOG(l, "live: amplifiers off: all four muted and powered down%s", bad ? " (a bus error on some: every write was tried)" : "");
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------------------------------------ speaker protection */
#define PROT "PianoSpeakerProtV1"

/* This start's mode: the protection pair when asked for, both files valid and no fallback since the last request. */
static int prot_choose(AudioLive *l)
{
    const uint32_t gen = __atomic_load_n(&l->sh->spk_prot_gen, __ATOMIC_ACQUIRE);
    if (gen != l->prot_gen) {
        l->prot_gen = gen;
        if (l->prot_failed) LOG(l, PROT ": a new request: the fallback of this boot is cleared");
        l->prot_failed = 0;
    }
    if (!load(&l->sh->spk_prot)) return 0;
    if (!l->prot_ready) {
        LOG(l, PROT ": asked for, but %s: the passthrough graph", l->prot_why ? l->prot_why : "the private files are not there");
        return 0;
    }
    if (l->prot_failed) {
        LOG(l, PROT ": the passthrough graph (fallback after %s; a new request clears it)", l->prot_why ? l->prot_why : "an error");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ MIPLAY (AudioSpkProt.h) */
#define MIP "PianoMiplayV1"

/* This start's MIPLAY request (the protection pair is this start's): 0 with *off saying why not. */
static uint32_t mip_choose(AudioLive *l, const char **off)
{
    const uint32_t gen = __atomic_load_n(&l->sh->miplay_gen, __ATOMIC_ACQUIRE);
    if (gen != l->mip_gen) {
        l->mip_gen = gen;
        if (l->mip_failed) LOG(l, MIP ": a new request: the MIPLAY fallback of this boot is cleared");
        l->mip_failed = 0;
    }
    const int r = load(&l->sh->miplay);
    if (!r) {*off = 0; return 0;}
    if (!l->mip_ready) {*off = l->mip_why ? l->mip_why : "no MIPLAY file"; return 0;}
    if (l->mip_failed) {*off = "a MIPLAY fallback of this boot (see above; a new request clears it)"; return 0;}
    return (uint32_t)r;
}

/* The DAP's pregain for this request now: fixed 0 dB, or the master volume (level 9 and up) in 1/16 dB, the floor when muted. */
static int32_t mip_preg(const AudioLive *l, uint32_t request)
{
    if (!request || !(request & SPK_MIP_PREG_TRACKED) || l->cfg.level < 9) return 0;
    if (load(&l->sh->muted)) return SPK_PREG_MUTE;
    return spk_preg_q4(__atomic_load_n(&l->sh->volume_q15, __ATOMIC_ACQUIRE));
}

/* One line per speaker start: MIPLAY's state for it. */
static void mip_report(AudioLive *l, const char *off)
{
    char words[96];
    const int request = load(&l->sh->miplay);
    if (l->mip_active) {
        const SpkPlan *p = &l->spk->plan;
        unsigned params = 0;
        for (unsigned k = 0; k < p->chunks; ++k) params += p->chunk[k].graph == 3 ? p->chunk[k].count : 0u;
        LOG(l, MIP ": speaker start %u: MIPLAY ON (request %u: %s): Dolby DAP %#x in MIPLAY %#x between the stream and MFC %#x; %u "
            "parameters (%u of the DAP) in %u packets; pregain %d/16 dB%s", l->spk_starts, l->mip_request,
            spk_miplay_text(l->mip_request, words, sizeof words), SPK_IID_DAP, SPK_SG_MIP, SPK_IID_MFC, params, l->spk->mip.dap, p->mip_chunks,
            p->preg, l->spk->mip.preg_pid ? "" : " (the file names no pregain parameter: none sent)");
    } else if (!request) {
        LOG(l, MIP ": speaker start %u: MIPLAY off (request 0)", l->spk_starts);
    } else {
        LOG(l, MIP ": speaker start %u: MIPLAY off (request %d asked for: %s)", l->spk_starts, request,
            off ? off : "MIPLAY runs only inside the protection plan, which is not this start's");
    }
}

/* The monitor stops sending: a command in flight completes (it cannot be recalled), nothing more goes out. */
static void mon_stop(AudioLive *l)
{
    l->mon_stopping = 1;
    if (l->xplan.active && !l->xplan.in_flight) {l->xplan.active = 0; l->xplan.unsent = 0;}
}

static void stream_fault(AudioLive *l, int reason, uint32_t now);

/* After the live data's answer: the DSP's header checked, one line per channel. */
static void live_report(AudioLive *l)
{
    SpkLive v;
    const uint8_t *slot = l->ops.dma + AUDIO_DMA_PARAM + l->spk->plan.live_slot;
    if (spk_live_parse(slot, &v)) {
        if (!l->live_bad)
            LOG(l, PROT " live: the answer's header is not the live data's (iid %#x param %#x size %u error %#x)", rd32(slot), rd32(slot + 4),
                rd32(slot + 8), rd32(slot + 12));
        l->live_bad = 1;
        return;
    }
    l->live_data = v;
    l->live_reads++;
    for (unsigned c = 0; c < 4; ++c) {
        const uint32_t mohm = (uint32_t)((uint64_t)v.re[c] * 1000u / 4096u);
        const uint32_t pct = l->spk->prot.re25[c] ? (uint32_t)((uint64_t)v.re[c] * 1000u / l->spk->prot.re25[c]) : 0;
        LOG(l, PROT " live ch%u slot %u r0 %u.%03u ohm (%u.%u%% of Re25) f0 %u Hz q %u t %u w2 %#x w5 %#x read %u vbat %u uV", c, c,
            mohm / 1000u, mohm % 1000u, pct / 10u, pct % 10u, v.f0[c] / 256u, v.q[c], v.temp[c], v.w2[c], v.w5[c], l->live_reads, l->vbat_uv);
    }
}

/* Every SPK_MONITOR_MS while the protected stream runs: the battery voltage (when PianoPower's gauge knows it), then the live data.
 * A refused or unanswered command, or live data under another header: an FSADSP error, so the pair is undone (fallback). */
static void mon_tick(AudioLive *l, uint32_t now)
{
    if (l->xplan.active) {
        if (l->mon_stopping) {
            if (!l->xplan.in_flight) {l->xplan.active = 0; l->xplan.unsent = 0;}
            return;
        }
        plan_send(l, &l->xplan, now);
        if (!plan_settled(&l->xplan)) return;
        l->xplan.active = 0;
        if (l->xplan.seq.state == ArSeqUp && !l->live_bad) {
            if (l->mon_preg) {
                l->preg_sent = l->mon.preg;
                l->preg_updates++;
                LOG(l, MIP ": pregain %d/16 dB (the master volume) to the DAP", l->preg_sent);
            }
            l->mon_preg = 0;
            return;
        }
        l->mon_preg = 0;
        l->prot_why = l->live_bad ? "live data under another header" : "a monitor command refused or unanswered";
        LOG(l, PROT ": monitor: %s (%s, status %#x): FSADSP error", l->prot_why,
            l->live_bad ? "live data from SP_RX" : ar_seq_step_name(&l->xplan.seq, l->xplan.seq.failed_step), l->xplan.seq.error_status);
        l->prot_fallback = 1;
        stream_fault(l, AudioGuardDsp, now);
        return;
    }
    if (!l->prot_active || l->spk_state != LiveRunning || l->spk_ending || l->mon_stopping || l->held || l->sleeping) return;
    if (now - l->mon_at < SPK_MONITOR_MS) return;
    l->mon_at = now;
    const uint32_t uv = l->ops.battery_uv ? l->ops.battery_uv(l->ops.ctx) : 0;
    l->mon.vbat_uv = uv >= 2500000u && uv <= 5000000u ? uv : 0;
    if (l->mon.vbat_uv) {l->vbat_uv = uv; l->vbat_sends++;}
    else if (!l->vbat_noted) {
        l->vbat_noted = 1;
        LOG(l, PROT ": no battery voltage from PianoPower's gauge (read %u uV): not sent; the live data still read", uv);
    }
    spk_live_stage(l->ops.dma + AUDIO_DMA_PARAM + l->spk->plan.live_slot);
    if (l->ops.publish) l->ops.publish(l->ops.ctx);
    l->mon.live_address = spk_plan_address(&l->spk->plan, l->spk->plan.live_slot);
    l->mon.handle = l->splan.seq.handle;
    /* MIPLAY with the pregain following the volume: a changed pregain goes first */
    l->mon_preg = 0;
    if (l->mip_active && (l->mip_request & SPK_MIP_PREG_TRACKED) && l->spk->mip.preg_pid) {
        const int32_t want = mip_preg(l, l->mip_request);
        if (want != l->preg_sent) {l->mon.preg = want; l->mon.preg_pid = l->spk->mip.preg_pid; l->mon_preg = 1;}
    }
    unsigned count;
    const ArStep *steps = spk_monitor_plan(l->mon.vbat_uv != 0, l->mon_preg, &count);
    ar_seq_init(&l->xplan.seq, steps, count, l->ports->apm, l->ports->prm, 0xA000u + (l->sleep_generation << 16), &l->mon);
    l->xplan.name = "speaker-protection monitor";
    l->xplan.active = l->xplan.quiet = 1;
    l->xplan.in_flight = 0;
    l->xplan.unsent = 0;
    plan_send(l, &l->xplan, now);
}

/* The speaker wants to play: the device runs, the master mute is off, and sound came within idle_ms. */
/* duplex-age1 (private DUPLEX-R3.md): the engine thread stamps sound_at with its own clock, so it can be a millisecond AFTER the now
 * this tick read (the inbox drain between them grows with the Mic's READ_DONEs). The old unsigned now - at then wrapped to about
 * 4.29e9: a playing speaker counted as silent and faded out and back in (20 ms dips while the Mic ran). Signed age; the old test
 * stays selectable (machdep.piano_audio_age=0) as the A/B control. */
static int sound_old(const AudioLive *l, uint32_t now, uint32_t from)
{
    if (!__atomic_load_n(&l->sh->age_signed, __ATOMIC_ACQUIRE)) return now - from >= l->cfg.idle_ms;
    return (int32_t)(now - from) >= (int32_t)l->cfg.idle_ms;
}

static int spk_active(AudioLive *l, uint32_t now, int *silent)
{
    const AudioLiveShared *sh = l->sh;
    const int have = load(&sh->have_sound);
    const uint32_t at = __atomic_load_n(&sh->sound_at, __ATOMIC_ACQUIRE);
    if (have && (int32_t)(now - at) < 0) {l->dx_ahead++; l->ahead_total++;}
    *silent = !have || sound_old(l, now, at);
    return !l->sleeping && load(&sh->want_out) && !(l->cfg.level >= 9 && load(&sh->muted)) && !*silent;
}

static void spk_begin(AudioLive *l, uint32_t now)
{
    ar_speaker_defaults(&l->speaker);
    l->speaker.buffer_phys = l->ops.dma_phys + AUDIO_DMA_SPEAKER;
    l->speaker.buffer_bytes = AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES;
    l->speaker.owner = l;
    l->speaker.amps_on = live_amps_on;
    l->speaker.amps_off = live_amps_off;
    dma_zero(l, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
    l->spk_starts++;
    {
        const int requested = load(&l->sh->duplex_hold);
        LOG(l, HOLD ": speaker start %u: hold %u (requested %d, Mic %s%s): %s", l->spk_starts, l->hold_mask, requested,
            audio_live_state(l->mic_state), !path_idle(l->mic_state) && l->mic_held ? " on the hold" : "",
            l->hold_mask ? "the VA clocks are held from before this TDM clock until both paths are down; a Mic start or stop changes no PRM resource" :
            !requested ? "off: the Mic's plan requests and releases its own votes and mclk" :
            !l->cfg.mic ? "not applied: no Mic path at this level" :
            l->hold_failed ? "not applied: a hold plan failed (see above)" :
            "not applied: a Mic run started before the request holds its own votes");
    }
    l->prot_active = prot_choose(l);
    l->mip_active = 0;
    l->mip_request = 0;
    const char *mip_off = 0;
    if (l->prot_active) {
        const char *why = 0;
        uint8_t *area = l->ops.dma + AUDIO_DMA_PARAM;
        const uint32_t area_bytes = l->ops.dma_bytes >= AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES ? AUDIO_DMA_PARAM_BYTES : 0;
        l->spk->plan.base = l->speaker;
        l->spk->plan.variant = (uint32_t)load(&l->sh->spk_prot) & SPK_VAR_ALL & ~1u;
        l->spk->plan.mip = &l->spk->mip;
        l->spk->plan.miplay = mip_choose(l, &mip_off);
        l->spk->plan.preg = mip_preg(l, l->spk->plan.miplay);
        int bad = spk_plan_init(&l->spk->plan, &l->spk->prot, area, area_bytes, &why);
        if (bad && l->spk->plan.miplay) {   /* MIPLAY's layout: the protection plan without it */
            l->mip_why = why;
            l->mip_failed = 1;
            l->mip_fallbacks++;
            LOG(l, MIP ": FALLBACK to the protection plan without MIPLAY: the plan cannot be laid out with it (%s)", why);
            mip_off = "a MIPLAY fallback of this boot (see above; a new request clears it)";
            l->spk->plan.miplay = 0;
            l->spk->plan.preg = 0;
            why = 0;
            bad = spk_plan_init(&l->spk->plan, &l->spk->prot, area, area_bytes, &why);
        }
        if (bad) {
            l->prot_active = 0;
            l->prot_failed = 1;
            l->prot_why = why;
            l->prot_fallbacks++;
            LOG(l, PROT ": FALLBACK to passthrough: the plan cannot be laid out (%s)", why);
        }
    }
    if (l->prot_active) {
        if (l->ops.publish) l->ops.publish(l->ops.ctx);   /* the out-of-band payloads before the first command names them */
        l->prot_starts++;
        l->mip_request = l->spk->plan.miplay;
        l->mip_active = l->mip_request != 0;
        l->mip_starts += (uint32_t)l->mip_active;
        l->preg_sent = l->spk->plan.preg;
        char variant[128];
        LOG(l, PROT ": start %u: the stock protection pair: V/I graph %#x and %#x with SP_RX %#x linked to SP_TX %#x; %u ACDB "
            "parameters (%u not sent) in %u packets, %u out of band; Re25 x4; gain cap %u", l->prot_starts, SPK_SG_VI, SPK_SG_DEV,
            SPK_IID_SP_RX, SPK_IID_SP_TX, l->spk->prot.count - l->spk->plan.skipped, l->spk->plan.skipped, l->spk->plan.chunks,
            l->spk->plan.oob_chunks, live_gain_cap(l));
        LOG(l, PROT ": variant %u: %s (round 2)", l->spk->plan.variant | 1u, spk_variant_text(l->spk->plan.variant, variant, sizeof variant));
        mip_report(l, mip_off);
        LOG(l, "live: speaker: sound from the Speaker device: the speaker-protection plan (start %u)", l->spk_starts);
        plan_start(l, &l->splan, "speaker-protection", l->spk->plan.steps, l->spk->plan.count, 0x8000 + (l->sleep_generation << 16), &l->spk->plan);
    } else {
        unsigned count;
        const ArStep *steps = ar_speaker_steps(&count);
        mip_report(l, 0);
        LOG(l, "live: speaker: sound from the Speaker device: the speaker plan (start %u)", l->spk_starts);
        plan_start(l, &l->splan, "speaker", steps, count, 0x8000 + (l->sleep_generation << 16), &l->speaker);
    }
    l->spk_state = LiveStarting;
    (void)now;
}

static void stream_begin(AudioLive *l, uint32_t now)
{
    uint64_t base = 0;
    (void)ar_dsp_address(l->speaker.buffer_phys, l->speaker.stream_id, &base);
    l->spk_mode = SpkPriming;
    l->spk_primed = 0;
    l->prime_left = AUDIO_LIVE_DEPTH;
    l->spk_ending = l->unmuted = l->dsp_error = l->stream_unsent = 0;
    l->amp_bad = 0;
    l->stream_at = l->next_status = now;
    l->spk_reason = 0;
    l->status_poll_active = 0;
    if (ar_pump_init(&l->pump, base, l->splan.seq.handle, l->ports->data, AUDIO_SPK_SLOTS, AUDIO_SPK_PERIOD_BYTES, AUDIO_LIVE_DEPTH,
                     AUDIO_LATE_MS)) {
        LOG(l, "live: speaker: the period pump refused its geometry");
        l->dsp_error = 1;
    }
    l->pump.token_base += l->sleep_generation << 16;
    (void)audio_chain_init(&l->chain, 4);
    (void)audio_chain_set_ceiling(&l->chain, l->cfg.ceiling);
    l->gain_now = l->cfg.level >= 9 ? __atomic_load_n(&l->sh->volume_q15, __ATOMIC_ACQUIRE) : AUDIO_Q15_ONE;
    audio_guard_start(&l->sguard, now, 0);   /* no time limit: the device's stream */
    audio_ring_reset(&l->sh->out);
    store(&l->sh->have_pcm, 0);
    drift_reset(&l->spk_drift);
    store(&l->sh->accepting, !l->sleeping);
    l->spk_state = LiveRunning;
    l->mon_stopping = l->live_bad = 0;
    l->mon_at = now - SPK_MONITOR_MS;   /* the first battery voltage and live data at once */
    LOG(l, "live: speaker: stream up: 10 ms periods, %u in flight, the ring's target %u frames; ceiling %d, gain %u; %u silent periods "
        "first, the amplifiers muted", AUDIO_LIVE_DEPTH, AUDIO_LIVE_TARGET, l->chain.ceiling, l->amps->amp[0].gain, AUDIO_UNMUTE_PERIODS);
    if (l->prot_active)
        LOG(l, PROT ": up: SP_RX protects the four speakers; battery voltage and live data every %u ms", SPK_MONITOR_MS);
    if (l->mip_active)
        LOG(l, MIP ": up: the Dolby DAP runs in MIPLAY %#x%s", SPK_SG_MIP, (l->mip_request & SPK_MIP_PREG_TRACKED) && l->spk->mip.preg_pid ?
            "; its pregain follows the master volume (checked with the monitor)" : "");
}

/* The ring's frames for one period: the drift held near the target by single frames, a stale backlog skipped. */
static void take_period(AudioLive *l, uint32_t burst)
{
    AudioRing *r = &l->sh->out;
    uint32_t fill = audio_ring_fill(r);
    const uint32_t before = fill, skipped = l->skipped;
    /* Keep this entire burst AND the reserve, including after startup. The
     * former >2400 -> 960 trim destroyed cold-start PCM just before refill. */
    const uint32_t pending = l->prime_left > burst ? l->prime_left : burst;
    const uint32_t keep = AUDIO_LIVE_TARGET + (spk_epoch_lead(l) ? pending * AUDIO_PERIOD_FRAMES : 0u);
    const uint32_t threshold = spk_epoch_lead(l) ? (keep > AUDIO_LIVE_START_FRAMES ? keep : AUDIO_LIVE_START_FRAMES) : AUDIO_LIVE_TARGET + 3u * AUDIO_PERIOD_FRAMES;
    if (fill > threshold) {
        audio_ring_skip(r, fill - keep);
        l->skipped++;
        fill = keep;
        drift_reset(&l->spk_drift);
    }
    uint32_t want = AUDIO_PERIOD_FRAMES;
    const int slip = l->prime_left ? 0 : drift_update(&l->spk_drift, fill);
    if (l->prime_left && !--l->prime_left) drift_reset(&l->spk_drift);
    if (slip > 0 && fill > AUDIO_PERIOD_FRAMES) {
        audio_ring_skip(r, 1);   /* the device runs fast: one frame less */
        l->drops++;
    } else if (slip < 0 && fill >= AUDIO_PERIOD_FRAMES) {
        want = AUDIO_PERIOD_FRAMES - 1u;   /* the DSP runs fast: one frame repeated */
        l->repeats++;
    }
    const uint32_t got = audio_ring_read(r, l->stereo, want);
    if (want < AUDIO_PERIOD_FRAMES) {
        l->stereo[2 * want] = got ? l->stereo[2 * want - 2] : 0;
        l->stereo[2 * want + 1] = got ? l->stereo[2 * want - 1] : 0;
    }
    if (got < want) l->underruns++;
    if (got < want || skipped != l->skipped || slip) ledger(l, AlRefill, before, audio_ring_fill(r), want, got, slip);
}

static void fill_period(AudioLive *l, uint32_t slot, uint32_t burst)
{
    const int listening = l->spk_primed && spk_same_epoch(l) && (l->spk_mode == SpkPlaying || l->spk_mode == SpkFading || l->spk_mode == SpkMuting);
    if (listening) {
        take_period(l, burst);
    } else {
        for (uint32_t i = 0; i < 2u * AUDIO_PERIOD_FRAMES; ++i) l->stereo[i] = 0;
        const uint32_t fill = audio_ring_fill(&l->sh->out);
        const uint32_t keep = spk_prefill(l);
        if (fill > keep) audio_ring_skip(&l->sh->out, fill - keep);
    }
    /* the master volume (level 9), ramped */
    const int32_t target = l->cfg.level >= 9 ? __atomic_load_n(&l->sh->volume_q15, __ATOMIC_ACQUIRE) : AUDIO_Q15_ONE;
    if (l->gain_now < target) l->gain_now = target - l->gain_now > AUDIO_VOLUME_STEP ? l->gain_now + AUDIO_VOLUME_STEP : target;
    else if (l->gain_now > target) l->gain_now = l->gain_now - target > AUDIO_VOLUME_STEP ? l->gain_now - AUDIO_VOLUME_STEP : target;
    /* full scale lands on the ceiling (no clipping at the cap; the clamp stays as the last guard) */
    (void)audio_chain_set_gain(&l->chain, (int32_t)((int64_t)l->gain_now * l->chain.ceiling / AUDIO_Q15_ONE));
    audio_stereo_to_quad(l->stereo, l->quad, AUDIO_PERIOD_FRAMES);
    audio_chain_run(&l->chain, l->quad, AUDIO_PERIOD_FRAMES);
    volatile uint64_t *d = (volatile uint64_t *)(void *)(l->ops.dma + AUDIO_DMA_SPEAKER + slot * AUDIO_SPK_PERIOD_BYTES);
    int sound = 0;
    for (uint32_t i = 0; i < AUDIO_PERIOD_FRAMES; ++i) {
        const int16_t *f = l->quad + 4u * i;
        for (unsigned c = 0; c < 4; ++c) {
            const int32_t v = f[c] < 0 ? -(int32_t)f[c] : f[c];
            if (v > l->peak) l->peak = v;
            sound |= v != 0;
        }
        d[i] = (uint64_t)(uint16_t)f[0] | (uint64_t)(uint16_t)f[1] << 16 | (uint64_t)(uint16_t)f[2] << 32 | (uint64_t)(uint16_t)f[3] << 48;
    }
    if (l->ops.publish) l->ops.publish(l->ops.ctx);
    l->periods++;
    if (sound) l->sound_periods++;
}

static void status_sample(AudioLive *l)
{
    ++l->status_slices;
    l->monitor_start = l->ops.clock_ms ? l->ops.clock_ms(l->ops.ctx) : l->now;
    l->monitor_field = l->status_poll.next;
    const int complete = audio_amps_poll(l->amps, &l->status_poll);
    l->monitor_end = l->ops.clock_ms ? l->ops.clock_ms(l->ops.ctx) : l->now;
    if (complete < 0) {
        l->amp_bad |= l->status_poll.failed;
        LOG(l, "live: amp: a status read failed (slots %#x)", l->status_poll.failed);
        return;
    }
    if (!complete) return;
    l->status_poll_active = 0;
    ++l->status_polls;
    const unsigned bad = audio_amps_check(l->amps);
    if (!bad && l->gain_witness && l->gain_reports < 8) {
        ++l->gain_reports;
        l->gain_witness = 0;
        for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
            const Fs19xxStatus *st = &l->amps->status[i];
            LOG(l, "aud-gain231-v1: active snapshot %u slot %u requested %u gain %u VOLCTRL 0x%04x STATUS 0x%04x DIGSTAT 0x%04x DACCTRL 0x%04x bus-errors %u",
                l->gain_reports, i, l->cfg.gain, st->volctrl >> 8, st->volctrl, st->status, st->digstat, st->dacctrl, l->amps->amp[i].bus_errors);
        }
    }
    if (bad) {
        l->amp_bad |= bad;
        for (unsigned i = 0; i < AUDIO_AMPS; ++i) {
            if (!(bad >> i & 1u)) continue;
            char names[80];
            const Fs19xxStatus *st = &l->amps->status[i];
            fs19xx_status_names(st->status & FS19XX_STATUS_BITS, names, sizeof names);
            LOG(l, "live: amp %s: STATUS 0x%04x (%s) PWRCTRL 0x%04x DIGSTAT 0x%04x VOLCTRL 0x%04x: flagged by the group check", audio_amp_name(i),
                st->status, names[0] ? names : "none", st->pwrctrl, st->digstat, st->volctrl);
        }
    }
}

/* A fault while the stream runs: the amplifiers down at once, the buffer zeroed, the plan undone. */
static void stream_fault(AudioLive *l, int reason, uint32_t now)
{
    store(&l->sh->accepting, 0);
    mon_stop(l);
    /* on the protection pair, a refused or late period is the graph's error too: undone, then the passthrough graph */
    if (l->prot_active && (reason == AudioGuardDsp || reason == AudioGuardLate)) {
        if (!l->prot_fallback) l->prot_why = reason == AudioGuardDsp ? "a refused period or command" : "a late period";
        l->prot_fallback = 1;
    }
    const unsigned bad = audio_amps_off(l->amps);
    dma_zero(l, AUDIO_DMA_SPEAKER, AUDIO_SPK_SLOTS * AUDIO_SPK_PERIOD_BYTES);
    audio_guard_trip(&l->sguard, reason);
    l->spk_reason = reason;
    l->unmuted = 0;
    LOG(l, "live: speaker: STOPPED: %s after %u ms (%u periods sent, %u returned, %u late, %u errors, last status %#x); the amplifiers "
        "muted and powered down at once%s; undoing the plan", audio_guard_reason(reason), now - l->stream_at, l->pump.sent, l->pump.returned,
        l->pump.late, l->pump.errors, l->pump.last_status, bad ? " (a bus error on some)" : "");
    ar_seq_abort(&l->splan.seq, ArSeqErrAbort, 0);
    l->spk_state = LiveStopping;
}

/* duplex-obs1: one line per window, then a new window */
static void dx_report(AudioLive *l, uint32_t now, const char *why)
{
    if (l->dx_ticks)
        LOG(l, "duplex-obs1: %s: %u ms, %u refill ticks (%u with the Mic up), DSP queue depth min %u, %u ticks at 0, %u at <= 1, "
            "largest gap between WRITE sends %u ms, %u queue-full; duplex-age1 %s: sound stamped ahead %u, fades %u (%u as silent)", why,
            now - l->dx_last_log, l->dx_ticks, l->dx_mic_ticks, l->dx_depth_min, l->dx_starved, l->dx_low, l->dx_gap_max,
            l->queue_full - l->dx_qfull_base, __atomic_load_n(&l->sh->age_signed, __ATOMIC_ACQUIRE) ? "signed" : "OLD unsigned",
            l->dx_ahead, l->dx_fades, l->dx_fades_silent);
    l->dx_ticks = l->dx_starved = l->dx_low = l->dx_gap_max = l->dx_mic_ticks = 0;
    l->dx_ahead = l->dx_fades = l->dx_fades_silent = 0;
    l->dx_depth_min = AUDIO_LIVE_DEPTH;
    l->dx_qfull_base = l->queue_full;
    l->dx_last_log = now;
}

static void stream_end(AudioLive *l, uint32_t now)
{
    dx_report(l, now, "speaker down");
    store(&l->sh->accepting, 0);
    mon_stop(l);
    LOG(l, "live: speaker: no sound for %u ms: down after %u ms (%u periods, %u with sound, %u underruns, %u skips, %u frames dropped, %u "
        "repeated; peak %d of ceiling %d): the plan undone, the amplifiers first", l->cfg.idle_ms, now - l->stream_at, l->pump.sent,
        l->sound_periods, l->underruns, l->skipped, l->drops, l->repeats, l->peak, l->chain.ceiling);
    ar_seq_stop(&l->splan.seq);
    l->spk_state = LiveStopping;
}

static void spk_stream_tick(AudioLive *l, uint32_t now)
{
    const int late = ar_pump_check(&l->pump, now);
    int r = audio_guard_check(&l->sguard, now, l->dsp_error, late, l->amp_bad);
    if (!r && l->held) r = AudioGuardQuiet;
    if (r && l->sleeping) {l->sleep_failed=1; (void)audio_amps_off(l->amps); return;}
    if (r) {stream_fault(l, r, now); return;}
    int silent = 0;
    const int active = spk_active(l, now, &silent);
    switch (l->spk_mode) {
    case SpkPriming:
        if (l->pump.returned < AUDIO_UNMUTE_PERIODS) break;
        if (!active) {
            l->spk_mode = SpkQuiet;
            l->quiet_at = now;
            l->hold_from_sound = silent;
            break;
        }
        /* fallthrough: unmute */
    case SpkQuiet:
        if (l->spk_mode == SpkQuiet && !active) {
            const uint32_t from = l->hold_from_sound && load(&l->sh->have_sound) ? __atomic_load_n(&l->sh->sound_at, __ATOMIC_ACQUIRE) : l->quiet_at;
            if (l->sleeping || sound_old(l, now, from) || (!load(&l->sh->want_out) && !load(&l->sh->have_sound))) l->spk_ending = 1;
            break;
        }
        l->spk_ending = 0;
        /* Silent DONE proves only the transport. Both cold and warm starts
         * need this epoch's accepted PCM and the worst four-slot burst budget. */
        if (!spk_has_prefill(l)) break;
        const uint32_t primed_fill = audio_ring_fill(&l->sh->out);
        /* Still quiet: retain the same bounded prefill as silent refills do.
         * Unmute may admit more PCM; take_period preserves the first burst. */
        if (primed_fill > spk_prefill(l)) audio_ring_skip(&l->sh->out, primed_fill - spk_prefill(l));
        if (audio_amps_unmute(l->amps)) {
            LOG(l, "live: speaker: unmuting failed: the amplifiers muted and powered down");
            stream_fault(l, AudioGuardAmplifier, now);
            return;
        }
        /* StartIO/StopIO may race the synchronous bus writes. No PCM has
         * been submitted here yet: a cancelled arm returns safely to quiet. */
        if (!spk_same_epoch(l) || !load(&l->sh->want_out)) {
            if (audio_amps_mute(l->amps)) {stream_fault(l, AudioGuardAmplifier, now); return;}
            l->spk_mode = SpkQuiet;
            l->spk_primed = l->unmuted = 0;
            l->quiet_at = now;
            break;
        }
        audio_chain_restart(&l->chain);
        l->unmuted = 1;
        l->gain_witness = 1;
        l->status_poll_active = 0;   /* discard a snapshot taken before this unmute */
        l->next_status = now + AUDIO_STATUS_MS;
        drift_reset(&l->spk_drift);
        l->spk_primed = 1;
        l->prime_left = spk_epoch_lead(l) ? AUDIO_LIVE_DEPTH : 0;
        l->spk_mode = SpkPlaying;
        ledger(l, AlPrime, primed_fill, audio_ring_fill(&l->sh->out), 0, 0, 0);
        LOG(l, "speaker-prime1:epoch-pcm:burst-reserve epoch %u fill %u budget %u burst %u reserve %u",
            l->ledger_epoch, primed_fill, spk_prefill(l), AUDIO_LIVE_DEPTH - l->pump.in_flight, AUDIO_LIVE_TARGET);
        LOG(l, "live: speaker: %u periods returned: the amplifiers unmuted, the sound fades in", l->pump.returned);
        break;
    case SpkPlaying:
        if (active && l->spk_primed && spk_same_epoch(l)) break;
        l->dx_fades++;
        l->dx_fades_silent += silent != 0;
        l->fades_silent_total += silent != 0;
        audio_chain_fade(&l->chain, -1);
        l->spk_mode = SpkFading;
        l->hold_from_sound = silent;
        break;
    case SpkFading:
        if (active && l->spk_primed && spk_same_epoch(l)) {audio_chain_fade(&l->chain, +1); l->spk_mode = SpkPlaying; break;}
        if (!audio_chain_silent(&l->chain)) break;
        l->mute_after = l->pump.sent;   /* the fade's last periods are still queued at the DSP: muted once they played */
        l->spk_mode = SpkMuting;
        break;
    case SpkMuting:
        if (active && l->spk_primed && spk_same_epoch(l)) {audio_chain_restart(&l->chain); l->spk_mode = SpkPlaying; break;}
        if (l->pump.returned < l->mute_after) break;
        if (audio_amps_mute(l->amps)) {stream_fault(l, AudioGuardAmplifier, now); return;}
        l->unmuted = 0;
        l->spk_primed = 0;
        l->status_poll_active = 0;
        l->spk_mode = SpkQuiet;
        l->quiet_at = now;
        LOG(l, "live: speaker: %s: faded to silence, the amplifiers muted; down after %u ms more without sound", silent ? "silence" :
            !load(&l->sh->want_out) ? "the device stopped" : "muted", l->cfg.idle_ms);
        break;
    }
    if (l->stream_unsent) {
        if (l->ops.send(l->ops.ctx, l->spkt, l->stream_len)) {l->queue_full++; return;}
        l->stream_unsent = 0;
        l->sends++;
    }
    if (l->spk_primed && l->unmuted) {   /* duplex-obs1: the DSP's queue as this refill finds it */
        const uint32_t depth = (uint32_t)l->pump.in_flight;
        if (!l->dx_ticks && !l->dx_last_log) l->dx_last_log = now;
        if (!l->dx_ticks || depth < l->dx_depth_min) l->dx_depth_min = depth;
        l->dx_ticks++;
        l->dx_mic_ticks += l->mic_state == LiveRunning;
        l->dx_starved += depth == 0;
        l->dx_low += depth <= 1;
    }
    uint32_t burst = AUDIO_LIVE_DEPTH - l->pump.in_flight;
    const uint32_t lead_before = audio_ring_fill(&l->sh->out), prime_before = l->prime_left;
    const uint32_t sent_before = l->pump.sent;
    while (!l->spk_ending) {
        const int slot = ar_pump_free_slot(&l->pump);
        if (slot < 0) break;
        fill_period(l, (uint32_t)slot, burst);
        if (burst) --burst;
        l->stream_len = ar_pump_write(&l->pump, l->spkt, sizeof l->spkt, now);
        if (!l->stream_len) {l->dsp_error = 1; break;}
        if (l->ops.send(l->ops.ctx, l->spkt, l->stream_len)) {l->stream_unsent = 1; l->queue_full++; break;}
        l->sends++;
    }
    if (l->spk_primed && prime_before && l->pump.sent != sent_before)
        ledger(l, AlLead, lead_before, audio_ring_fill(&l->sh->out), 0, 0, 0);
    if (l->pump.sent != sent_before) {   /* duplex-obs1: the cadence of WRITE sends */
        if (l->dx_last_send && l->unmuted && now - l->dx_last_send > l->dx_gap_max) l->dx_gap_max = now - l->dx_last_send;
        l->dx_last_send = now;
    }
    if (l->dx_ticks && now - l->dx_last_log >= 2000u) dx_report(l, now, l->mic_state == LiveRunning ? "window, Mic up" : "window");
    if (l->spk_ending && !l->stream_unsent && !l->pump.in_flight) stream_end(l, now);
    /* Refill first. Twenty-four synchronous I2C reads here used to block the
     * four-period queue, then trim its recovered FIFO backlog to two periods.
     * Yield to completion handling between EVERY register, keeping the group
     * snapshot private until all reads succeed. No second bus/DSP owner. */
    if (l->spk_state == LiveRunning && !l->spk_ending && !l->stream_unsent) {
        unsigned requested = __atomic_load_n(&l->sh->amp_gain_requested, __ATOMIC_ACQUIRE);
        if (requested > live_gain_cap(l)) requested = live_gain_cap(l);
        if (!l->gain_changing && requested != l->cfg.gain) {
            l->gain_target = requested;
            l->gain_slot = 0;
            l->gain_changing = 1;
            l->status_poll_active = 0;   /* never publish a mixed-gain snapshot */
        }
        if (l->gain_changing) {
            if (audio_amps_gain_slot(l->amps, l->gain_slot, l->gain_target, live_gain_cap(l))) {
                stream_fault(l, AudioGuardAmplifier, now);
                return;
            }
            if (++l->gain_slot == AUDIO_AMPS) {
                l->cfg.gain = l->gain_target;
                l->gain_changing = 0;
                l->gain_witness = 1;
                l->next_status = now;
                LOG(l, "aud-gain231-v1: applied gain %u cap %u (four slots; refill first)", l->cfg.gain, live_gain_cap(l));
            }
            return;   /* yield to DSP completion handling after each I2C write */
        }
        if (!l->unmuted) return;
        if (!l->status_poll_active && (int32_t)(now - l->next_status) >= 0) {
            zero(&l->status_poll, sizeof l->status_poll);
            l->status_poll_active = 1;
            l->next_status = now + l->cfg.status_ms;
        }
        if (l->status_poll_active) {
            status_sample(l);
            if (l->amp_bad) stream_fault(l, AudioGuardAmplifier, now);
        }
    }
}

static void spk_tick(AudioLive *l, uint32_t now)
{
    switch (l->spk_state) {
    case LiveDown: {
        /* held, muted or no device: the sound flag stays set, so play() wakes nobody until it is taken (the control, the device's
         * start and the end of quiet come with their own wake or the next tick) */
        if (l->sleeping || l->held || l->aborting || !load(&l->sh->want_out) || (l->cfg.level >= 9 && load(&l->sh->muted))) return;
        int silent = 0;
        const int active = spk_active(l, now, &silent);
        if (!active && !load(&l->sh->sound)) return;
        if (!hold_gate(l, "speaker")) return;   /* duplex-hold1: the held clocks first (the sound flag stays for the next tick) */
        const int fresh = __atomic_exchange_n(&l->sh->sound, 0, __ATOMIC_ACQ_REL);
        if (!active && !fresh) return;
        spk_begin(l, now);
        break;
    }
    case LiveRunning:
        spk_stream_tick(l, now);
        break;
    default:
        break;
    }
    mon_tick(l, now);
    plan_send(l, &l->splan, now);
    if (!plan_settled(&l->splan)) return;
    const int up = l->splan.seq.state == ArSeqUp;
    const int down = (l->splan.seq.state == ArSeqDown || l->splan.seq.state == ArSeqFailed) && !l->splan.seq.undo_errors;
    if (l->spk_state == LiveStarting) {
        if (up) {stream_begin(l, now); return;}
        /* the protection pair refused (or left unanswered) a DSP step, and everything it did is undone: the passthrough graph next */
        if (l->prot_active && l->mip_active && l->splan.seq.error != ArSeqErrLocal && !l->splan.seq.undo_errors) {
            l->mip_why = "a step of the protection plan with MIPLAY failed";
            LOG(l, MIP ": FALLBACK to the protection plan without MIPLAY: %s refused or unanswered (status %#x); everything it did is "
                "undone; the speaker comes back on the protection plan", ar_seq_step_name(&l->splan.seq, l->splan.seq.failed_step),
                l->splan.seq.error_status);
            l->mip_active = 0;
            l->mip_failed = 1;
            l->mip_fallbacks++;
            l->prot_active = 0;
            l->splan.active = 0;
            l->spk_state = LiveDown;
            return;
        }
        if (l->prot_active && l->splan.seq.error != ArSeqErrLocal && !l->splan.seq.undo_errors) {
            l->prot_why = "a step of the protection plan failed";
            LOG(l, PROT ": FALLBACK to passthrough: %s refused or unanswered (status %#x); everything it did is undone; the speaker "
                "comes back on the passthrough graph", ar_seq_step_name(&l->splan.seq, l->splan.seq.failed_step), l->splan.seq.error_status);
            l->prot_active = 0;
            l->prot_failed = 1;
            l->prot_fallbacks++;
            l->splan.active = 0;
            l->spk_state = LiveDown;
            return;
        }
        LOG(l, "live: speaker: the plan failed at %s (status %#x): the speaker path is OFF for this boot",
            ar_seq_step_name(&l->splan.seq, l->splan.seq.failed_step), l->splan.seq.error_status);
        l->splan.active = 0;
        l->prot_active = 0;
        l->spk_state = LiveOff;
        audio_amps_hold(l->amps);
        return;
    }
    if (l->spk_state == LiveStopping) {
        l->splan.active = 0;
        l->spk_stops++;
        const int reason = l->spk_reason;
        const int fallback = l->prot_fallback;
        const int mip = l->mip_active;
        l->prot_fallback = 0;
        l->prot_active = 0;   /* this start is over */
        l->mip_active = 0;
        if (!down) {
            LOG(l, "live: speaker: undoing the plan failed (%u undo errors): the speaker path is OFF for this boot", l->splan.seq.undo_errors);
            l->spk_state = LiveOff;
        } else if (fallback && mip) {
            l->mip_why = l->prot_why ? l->prot_why : "an error";
            l->prot_why = 0;
            LOG(l, MIP ": FALLBACK to the protection plan without MIPLAY: %s on the protection pair with MIPLAY; all of it is undone; the "
                "speaker comes back on the protection plan", l->mip_why);
            l->mip_failed = 1;
            l->mip_fallbacks++;
            l->spk_state = LiveDown;
            store(&l->sh->sound, 0);
        } else if (fallback) {
            LOG(l, PROT ": FALLBACK to passthrough: %s on the protection pair; the pair is undone; the speaker comes back on the "
                "passthrough graph", l->prot_why ? l->prot_why : "an FSADSP error");
            l->prot_failed = 1;
            l->prot_fallbacks++;
            l->spk_state = LiveDown;
            store(&l->sh->sound, 0);
        } else if (reason == AudioGuardLate && ++l->late_stops >= AUDIO_LIVE_LATE_STOPS) {
            LOG(l, "live: speaker: %u late stops: the speaker path is OFF for this boot", l->late_stops);
            l->spk_state = LiveOff;
        } else if (reason && reason != AudioGuardLate && reason != AudioGuardQuiet) {
            LOG(l, "live: speaker: stopped by %s: the speaker path is OFF for this boot", audio_guard_reason(reason));
            l->spk_state = LiveOff;
        } else {
            LOG(l, "live: speaker: down%s", reason ? " (it comes back with the next sound)" : "");
            l->spk_state = LiveDown;
            store(&l->sh->sound, 0);
        }
        if (l->spk_state == LiveOff) audio_amps_hold(l->amps);
    }
}

/* ------------------------------------------------------------------------------------------------ the mic path (level 10) */
static void mic_route_applied(AudioLive *l, unsigned setting, uint32_t generation)
{
    l->mic_route = (int)(setting & 7u) - 1;
    l->mic_route_generation = generation;
    if (l->cfg.mic_route_apply) l->cfg.mic_route_apply(l->mic_route, generation);
    LOG(l, "mic-route1: apply request %d applied %d gen %u diag %u mono %s",
        l->mic_route, l->mic_route, generation, !!(setting & MicDiag), audio_mic_mono_name(l->mic_route));
}

static void mic_apply_pending(AudioLive *l)
{
    uint64_t old = mic_control(l->sh);
    while (mic_stopped(old)) {
        const unsigned requested = old & MicSetting;
        if (requested == ((old >> MicAppliedShift) & MicSetting)) return;
        const uint32_t generation = (uint32_t)(old >> MicGenerationShift) + 1;
        const uint64_t next = ((uint32_t)old & ~(MicSetting << MicAppliedShift)) |
                              (requested << MicAppliedShift) | ((uint64_t)generation << MicGenerationShift);
        if (mic_control_update(l->sh, &old, next)) {
            mic_route_applied(l, requested, generation);
            return;
        }
    }
}

static void mic_window_end(AudioLive *l, const char *event, uint32_t now)
{
    AudioMicWindow *w = l->mic_recording;
    if (!w) return;
    w->end_ms = now;
    l->mic_recording = 0;
    uint64_t old = mic_control(l->sh);
    uint64_t next;
    do {
        next = (old & ~(uint64_t)MicWindowArmed) | (w->frames ? MicWindowReady : 0);
    } while (!mic_control_update(l->sh, &old, next));
    LOG(l, "mic-route1: window %s gen %u run %u route %d start-ms %u end-ms %u frames %u periods %u",
        event, w->generation, w->run, w->route, w->start_ms, now, w->frames, w->periods);
}

static int live_va_on(void *owner)
{
    AudioLive *l = owner;
    const int r = l->ops.va_mic ? l->ops.va_mic(l->ops.ctx, 1) : -1;
    LOG(l, "live: mic: the VA macro's DMIC path %s gen %u", r ? "FAILED" : "on", l->mic_route_generation);
    if (!r) l->va_on = 1;
    return r;
}

static int live_va_off(void *owner)
{
    AudioLive *l = owner;
    if (!l->va_on) return 0;
    const int r = l->ops.va_mic ? l->ops.va_mic(l->ops.ctx, 0) : 0;
    l->va_on = 0;
    LOG(l, "live: mic: the VA macro's DMIC path off gen %u result %d", l->mic_route_generation, r);
    return r;
}

static void mic_begin(AudioLive *l)
{
    const uint64_t control = __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)MicGraphReleased, __ATOMIC_ACQ_REL);
    if (control & MicWindowArmed) {
        AudioMicWindow *w = __atomic_load_n(&l->mic_window, __ATOMIC_ACQUIRE);
        w->magic = AUDIO_MIC_DIAG_MAGIC;
        w->generation = l->mic_route_generation;
        w->run = l->mic_starts + 1;
        w->route = l->mic_route;
        w->start_ms = w->end_ms = w->periods = w->frames = 0;
        l->mic_recording = w;
        if (control & MicWindowStop) mic_window_end(l, "input-stop", l->now);
    }
    zero(l->mic_pcm, sizeof l->mic_pcm);
    l->mic_frames = l->mic_settle = l->mic_full_frames = l->mic_tail_frames = 0;
    l->mic_first_read = l->mic_first_pcm = 0;
    l->mic_drop_base = l->mic_drops; l->mic_repeat_base = l->mic_repeats; l->mic_full_base = l->mic_full;
    l->mic_underrun_base = l->sh->in.underruns; l->mic_overrun_base = l->sh->in.overruns;
    ar_mic_defaults(&l->micplan);
    l->micplan.buffer_phys = l->ops.dma_phys + AUDIO_DMA_MIC;
    l->micplan.buffer_bytes = AUDIO_MIC_SLOTS * AUDIO_MIC_PERIOD_BYTES;
    l->micplan.owner = l;
    l->micplan.va_on = live_va_on;
    l->micplan.va_off = live_va_off;
    dma_zero(l, AUDIO_DMA_MIC, AUDIO_MIC_SLOTS * AUDIO_MIC_PERIOD_BYTES);
    unsigned count;
    /* duplex-hold1: on a held prefix (hold_gate let this start through only with the hold up) the plan skips those steps */
    l->mic_held = l->hold_mask && l->hplan.seq.state == ArSeqUp ? l->hold_mask : 0;
    const ArStep *steps = l->mic_held ? ar_mic_live_steps_held(l->mic_held, &count) : ar_mic_live_steps(&count);
    l->mic_starts++;
    const AudioMicRouteStatus route = audio_live_mic_route_status(l->sh);
    LOG(l, "mic-route1: start request %d applied %d gen %u run %u", route.requested, route.applied, route.generation, l->mic_starts);
    LOG(l, "live: mic: the Mic device runs: the capture plan (start %u)", l->mic_starts);
    LOG(l, HOLD ": Mic start %u: hold %u, speaker %s: %s", l->mic_starts, l->mic_held, audio_live_state(l->spk_state),
        l->mic_held == (AR_HOLD_VOTES | AR_HOLD_MCLK) ? "no PRM request (the votes and the mclk are held)" :
        l->mic_held ? "the mclk requested, the votes held" : "the votes and the mclk requested by this plan");
    plan_start(l, &l->mplan, "microphone", steps, count, 0x9000 + (l->sleep_generation << 16), &l->micplan);
    l->mic_state = LiveStarting;
}

static void capture_begin(AudioLive *l, uint32_t now)
{
    uint64_t base = 0;
    (void)ar_dsp_address(l->micplan.buffer_phys, l->micplan.stream_id, &base);
    l->mic_ending = l->cap_unsent = l->cap_error = 0;
    l->mic_reason = 0;
    l->mic_at = now;
    if (l->mic_recording) l->mic_recording->start_ms = now;
    l->mic_diag_at = now;
    l->mic_idle_at = 0;
    if (ar_pump_init_capture(&l->cap, base, l->mplan.seq.handle, l->ports->data, AUDIO_MIC_SLOTS, AUDIO_MIC_PERIOD_BYTES,
                             AUDIO_LIVE_MIC_DEPTH, AUDIO_LATE_MS))
        l->cap_error = 1;
    l->cap.token_base += l->sleep_generation << 16;
    if (l->sh->tap) __atomic_add_fetch(&l->sh->tap->run, 1u, __ATOMIC_RELEASE);   /* a tap reader re-anchors at a new run */
    l->sh->in.head = l->sh->in.tail = 0;   /* nobody reads it while capturing is 0 */
    drift_reset(&l->mic_drift);
    store(&l->sh->input_primed, 0);
    store(&l->sh->capturing, !l->sleeping);
    store(&l->sh->input_fault, 0);
    l->mic_state = LiveRunning;
    LOG(l, "live: mic: capture up: 10 ms periods of two channels, %u in flight, mono %s gen %u; the first %u ms dropped",
        AUDIO_LIVE_MIC_DEPTH, audio_mic_mono_name(l->mic_route), l->mic_route_generation, AUDIO_LIVE_MIC_SETTLE_MS);
    if (l->sh->tap)
        LOG(l, "live: mic: tap1: run %u clients %d mic-device %d written %llu", __atomic_load_n(&l->sh->tap->run, __ATOMIC_ACQUIRE),
            load(&l->sh->tap_open), load(&l->sh->want_in), (unsigned long long)audio_tap_written(l->sh->tap));
    LOG(l, "live: mic: mic-clock2:prime-target:fixed-center reserve %u band %u ring %u",
        AUDIO_LIVE_MIC_TARGET, AUDIO_LIVE_SLIP_BAND, AUDIO_LIVE_MIC_RING);
    LOG(l, "live: mic: mic-clock3:queue-margin center %u trough %u batch-bias %u band %u transport-latency %u",
        AUDIO_LIVE_MIC_TARGET, AUDIO_LIVE_MIC_RESERVE, AUDIO_LIVE_MIC_BATCH_BIAS,
        AUDIO_LIVE_SLIP_BAND, AUDIO_LIVE_INPUT_LATENCY);
    LOG(l, "live: mic: read-v2:56bytes:no-offset request %#x endpoint %#x port %#x base %#llx handle %#x period %u slots %u token %#x",
        AR_OP_READ, AR_IID_CAP_SHMEM, l->cap.port, (unsigned long long)l->cap.base, l->cap.handle,
        l->cap.period_bytes, l->cap.slots, l->cap.token_base);
}

/* One returned capture period into the Mic's ring: two channels to mono, the drift held near the ring's target. */
static void capture_period(AudioLive *l, const ArAnswer *a, uint32_t now)
{
    const uint8_t *src = l->ops.dma + AUDIO_DMA_MIC + l->cap.last_slot * AUDIO_MIC_PERIOD_BYTES + a->offset;
    uint32_t frames = a->data_bytes / 4u;
    if (frames > AUDIO_PERIOD_FRAMES) frames = AUDIO_PERIOD_FRAMES;
    const int settle = now - l->mic_at < AUDIO_LIVE_MIC_SETTLE_MS;
    if (!l->mic_first_read) l->mic_first_read = now - l->mic_at + 1;
    l->mic_frames += frames;
    if (settle) l->mic_settle += frames;
    AudioMicWindow *w = l->mic_recording;
    if (w && (mic_control(l->sh) & MicWindowStop)) {mic_window_end(l, "input-stop", now); w = 0;}
    if (w && now - w->start_ms >= AUDIO_MIC_DIAG_MS) {mic_window_end(l, "limit", now); w = 0;}
    uint32_t raw_frames = 0;
    int16_t *raw = 0;
    if (w && !settle && frames) {
        raw_frames = AUDIO_MIC_DIAG_FRAMES - w->frames;
        if (raw_frames > frames) raw_frames = frames;
        raw = w->pcm + w->frames * 2u;
        w->period[w->periods++] = (AudioMicPeriod){now, raw_frames, (uint32_t)a->timestamp, (uint32_t)(a->timestamp >> 32)};
        w->frames += raw_frames;
    }
    const int legacy = audio_live_mic_route_status(l->sh).applied == AUDIO_MIC_ROUTE_LEGACY;
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t a0 = (int16_t)(src[4 * i] | src[4 * i + 1] << 8), a1 = (int16_t)(src[4 * i + 2] | src[4 * i + 3] << 8);
        const int16_t mono = legacy ? (int16_t)(((int32_t)a0 + a1) / 2) : a0;
        if (i < raw_frames) {raw[2 * i] = a0; raw[2 * i + 1] = a1;}
        audio_capture_sample(&l->mic_pcm[0], a0);
        audio_capture_sample(&l->mic_pcm[1], a1);
        audio_capture_sample(&l->mic_pcm[2], mono);
        if ((a0 || a1) && !l->mic_first_pcm) l->mic_first_pcm = now - l->mic_at + 1;
        l->mono[i] = settle ? 0 : mono;
    }
    if (w && (w->frames == AUDIO_MIC_DIAG_FRAMES || w->periods == AUDIO_MIC_DIAG_PERIODS)) mic_window_end(l, "limit", now);
    /* The tap (AudioTap.h): this period's mono frames as captured (the settle zeros included, before the Mic ring's slip and the
     * Mic's level), with the Mic's level and mute for the reader to apply; only while a tap client is open. */
    if (l->sh->tap && load(&l->sh->tap_open) > 0)
        audio_tap_write(l->sh->tap, l->mono, frames, (uint32_t)__atomic_load_n(&l->sh->input_q15, __ATOMIC_ACQUIRE),
                        load(&l->sh->input_muted) || load(&l->sh->input_fault), now);
    l->mic_periods++;
    AudioRing *r = &l->sh->in;
    const uint32_t fill = audio_ring_fill(r);
    /* A completion batch can raise fill above the controller's mean without
     * exhausting storage. Use the actual room for this return plus a possible
     * repeat; the old target+3-period cutoff discarded still-fitting PCM. */
    if (fill + frames + 1u > AUDIO_LIVE_MIC_RING) {
        l->mic_full++;
        if (load(&l->sh->want_in)) l->mic_full_frames += frames; else l->mic_tail_frames += frames;
        drift_reset(&l->mic_drift); return;
    }   /* not read: dropped */
    /* Empty-start calibration learns zero: later batched completions then
     * look permanently overfull, dropping one frame on every READ even as
     * the consumer underruns. Capture has a declared FIFO target; use that
     * fixed center and do not learn a clock from startup/prefill or idle. */
    int slip = 0;
    if (load(&l->sh->want_in) && load(&l->sh->input_primed)) {
        if (!l->mic_drift.samples) {
            l->mic_drift.avg = l->mic_drift.setpoint = AUDIO_LIVE_MIC_TARGET * 64;
            l->mic_drift.samples = 65;
        }
        slip = drift_update(&l->mic_drift, fill);
    } else drift_reset(&l->mic_drift);
    if (frames && slip > 0) {
        frames--;   /* the Mic's engine runs slow: one frame less */
        l->mic_drops++;
    } else if (frames && slip < 0) {
        l->mono[frames] = l->mono[frames - 1];   /* it runs fast: one frame repeated */
        frames++;
        l->mic_repeats++;
    }
    (void)audio_ring_write(r, l->mono, frames);
}

/* Bounded snapshots, at one-second intervals and stop, rather than one line
 * per 10 ms READ. Totals start at this DSP capture run; HAL totals identify
 * their independent StartIO window. PCM sums/squares permit AC/DC analysis. */
void audio_live_mic_report(AudioLive *l, const char *event, uint32_t now)
{
    const ArPump *p = &l->cap;
    LOG(l, "mic health: run %u event %s ms %u active %d sent %u returned %u flight %u errors %u late %u status %#x bytes %llu min %u max %u offset-min %u offset-max %u short %u zero %u align %u origin %u range %u",
        l->mic_starts, event, now - l->mic_at, load(&l->sh->want_in), p->sent, p->returned, p->in_flight, p->errors, p->late,
        p->last_status, (unsigned long long)p->returned_bytes, p->min_bytes, p->max_bytes, p->min_offset, p->max_offset,
        p->short_returns, p->zero_returns, p->bad_alignment, p->bad_origin, p->bad_range);
    LOG(l, "mic frames: run %u returned %llu settle %llu full-periods %u active-full %llu idle-tail %llu fifo-missing %llu fifo-overrun %llu drop %u repeat %u fill %u first-read-ms+1 %u first-pcm-ms+1 %u primed %d drift-avg %d drift-target %d",
        l->mic_starts, (unsigned long long)l->mic_frames, (unsigned long long)l->mic_settle, l->mic_full - l->mic_full_base,
        (unsigned long long)l->mic_full_frames, (unsigned long long)l->mic_tail_frames,
        (unsigned long long)(l->sh->in.underruns - l->mic_underrun_base), (unsigned long long)(l->sh->in.overruns - l->mic_overrun_base),
        l->mic_drops - l->mic_drop_base, l->mic_repeats - l->mic_repeat_base, audio_ring_fill(&l->sh->in), l->mic_first_read, l->mic_first_pcm,
        load(&l->sh->input_primed), l->mic_drift.avg / 64, l->mic_drift.setpoint / 64);
    for (unsigned i = 0; i < 3; ++i) {
        const AudioCapturePcm *s = &l->mic_pcm[i];
        LOG(l, "mic PCM: run %u layer %s frames %llu nonzero %llu peak %u sum %lld squares %llu clipped %llu",
            l->mic_starts, i == 0 ? "DEC0" : i == 1 ? "DEC1" : "mono", (unsigned long long)s->frames,
            (unsigned long long)s->nonzero, s->peak, (long long)s->sum, (unsigned long long)s->squares, (unsigned long long)s->clipped);
    }
}

static void mic_fault(AudioLive *l, int reason, uint32_t now)
{
    store(&l->sh->input_fault, 1);
    store(&l->sh->capturing, 0);
    mic_window_end(l, "fault", now);
    audio_live_mic_report(l, "fault", now);
    l->mic_reason = reason;
    LOG(l, "live: mic: STOPPED: %s after %u ms (%u periods sent, %u returned, %u late, %u errors, last status %#x): undoing the plan",
        audio_guard_reason(reason), now - l->mic_at, l->cap.sent, l->cap.returned, l->cap.late, l->cap.errors, l->cap.last_status);
    ar_seq_abort(&l->mplan.seq, ArSeqErrAbort, 0);
    l->mic_state = LiveStopping;
}

static void capture_tick(AudioLive *l, uint32_t now)
{
    int r = l->cap_error ? AudioGuardDsp : ar_pump_check(&l->cap, now) ? AudioGuardLate : AudioGuardOk;
    if (!r && l->held) r = AudioGuardQuiet;
    if (r && l->sleeping) {l->sleep_failed=1; return;}
    if (r) {mic_fault(l, r, now); return;}
    if (l->mic_recording && now - l->mic_recording->start_ms >= AUDIO_MIC_DIAG_MS) mic_window_end(l, "limit", now);
    if (l->sleeping) l->mic_ending = 1;
    if (l->mic_recording && (mic_control(l->sh) & MicWindowStop)) mic_window_end(l, "input-stop", now);
    if (now - l->mic_diag_at >= 1000) {audio_live_mic_report(l, "tick", now); l->mic_diag_at = now;}
    if (mic_wanted(l->sh)) l->mic_idle_at = 0;
    else if (!l->mic_idle_at) l->mic_idle_at = now ? now : 1;
    if (l->mic_idle_at && now - l->mic_idle_at >= l->cfg.idle_ms) l->mic_ending = 1;
    if (l->cap_unsent) {
        if (l->ops.send(l->ops.ctx, l->cpkt, l->cap_len)) {l->queue_full++; return;}
        l->cap_unsent = 0;
        l->sends++;
    }
    while (!l->mic_ending && ar_pump_free_slot(&l->cap) >= 0) {
        l->cap_len = ar_pump_write(&l->cap, l->cpkt, sizeof l->cpkt, now);
        if (!l->cap_len) {l->cap_error = 1; break;}
        if (l->ops.send(l->ops.ctx, l->cpkt, l->cap_len)) {l->cap_unsent = 1; l->queue_full++; break;}
        l->sends++;
    }
    if (l->mic_ending && !l->cap_unsent && !l->cap.in_flight) {
        store(&l->sh->capturing, 0);
        audio_live_mic_report(l, "stop", now);
        LOG(l, "live: mic: the Mic device stopped: down after %u ms (%u periods, %u dropped, %u repeated, %u times the ring was full): "
            "the plan undone", now - l->mic_at, l->mic_periods, l->mic_drops, l->mic_repeats, l->mic_full);
        ar_seq_stop(&l->mplan.seq);
        l->mic_state = LiveStopping;
    }
}

static void mic_tick(AudioLive *l, uint32_t now)
{
    if (!l->cfg.mic) return;
    switch (l->mic_state) {
    case LiveDown:
        if (l->sleeping || l->aborting) return;
        mic_apply_pending(l);
        if (l->held || !mic_wanted(l->sh)) return;
        if (!hold_gate(l, "Mic")) return;   /* duplex-hold1: the held clocks first */
        mic_begin(l);
        break;
    case LiveRunning:
        capture_tick(l, now);
        break;
    default:
        break;
    }
    plan_send(l, &l->mplan, now);
    if (!plan_settled(&l->mplan)) return;
    const int up = l->mplan.seq.state == ArSeqUp;
    const int down = (l->mplan.seq.state == ArSeqDown || l->mplan.seq.state == ArSeqFailed) && !l->mplan.seq.undo_errors;
    if (l->mic_state == LiveStarting) {
        if (up) {capture_begin(l, now); return;}
        LOG(l, "live: mic: the capture plan failed at %s (status %#x): the mic path is OFF for this boot",
            ar_seq_step_name(&l->mplan.seq, l->mplan.seq.failed_step), l->mplan.seq.error_status);
        l->mplan.active = 0;
        l->mic_state = LiveOff;
        mic_window_end(l, "setup-failed", now);
        store(&l->sh->input_fault, 1);
        return;
    }
    if (l->mic_state == LiveStopping) {
        l->mplan.active = 0;
        const int reason = l->mic_reason;
        if (!down) l->mic_state = LiveOff;
        else if (reason == AudioGuardLate && ++l->mic_late_stops >= AUDIO_LIVE_LATE_STOPS) l->mic_state = LiveOff;
        else if (reason && reason != AudioGuardLate && reason != AudioGuardQuiet) l->mic_state = LiveOff;
        else l->mic_state = LiveDown;
        LOG(l, "live: mic: %s", l->mic_state == LiveOff ? "the mic path is OFF for this boot" : "down");
        LOG(l, "mic release: run %u down %d va %d retired-handle %#x undo-errors %u route %d gen %u", l->mic_starts, down, l->va_on,
            l->mplan.seq.handle, l->mplan.seq.undo_errors, l->mic_route, l->mic_route_generation);
        if (l->mic_state == LiveDown && !l->va_on && !l->sleeping && !l->aborting)
            __atomic_fetch_or(&l->sh->mic_control, MicGraphReleased, __ATOMIC_RELEASE);
    }
}

/* ------------------------------------------------------------------------------------------------ the whole */
static void finish(AudioLive *l, uint32_t now)
{
    mic_window_end(l, "finish", now);
    __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)(MicGraphReleased | MicWindowArmed), __ATOMIC_ACQ_REL);
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    audio_amps_hold(l->amps);
    if (l->va_on && l->ops.va_mic) {(void)l->ops.va_mic(l->ops.ctx, 0); l->va_on = 0;}
    dma_zero(l, 0, AUDIO_DMA_BYTES);
    if (l->ops.tdm_pins) (void)l->ops.tdm_pins(l->ops.ctx, 0);
    l->splan.active = l->mplan.active = l->xplan.active = l->hplan.active = 0;
    l->hold_mask = 0;
    l->prot_active = 0;
    if (l->spk_state != LiveOff) l->spk_state = LiveDown;
    if (l->mic_state != LiveOff) l->mic_state = LiveDown;
    LOG(l, "live: done after %u ms: the amplifiers held in reset, the TDM pins given back; speaker %s (%u starts, %u periods, %u with "
        "sound, %u underruns, %u skips, %u dropped, %u repeated, peak %d), mic %s (%u starts, %u periods); %u packets sent, %u received, "
        "%u ignored, %u unanswered, %u times the queue was full", now - l->started_at, audio_live_state(l->spk_state), l->spk_starts,
        l->periods, l->sound_periods, l->underruns, l->skipped, l->drops, l->repeats, l->peak, audio_live_state(l->mic_state),
        l->mic_starts, l->mic_periods, l->sends, l->received, l->stray, l->timeouts, l->queue_full);
    l->finished = 1;
}

/* PianoPower's health turned the guard off: no DSP command any more; the amplifiers down and held now. */
static void go_off(AudioLive *l, uint32_t now)
{
    store(&l->sh->input_fault, 1);
    if (l->mic_state == LiveRunning) audio_live_mic_report(l, "guard-off", now);
    l->off = 1;
    LOG(l, "live: dsp: guard OFF: %s: no DSP command goes out any more in this boot; the amplifiers muted, powered down and held in reset",
        adg_reason(l->guard ? l->guard->reason : AdgReasonOwner));
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    audio_amps_hold(l->amps);
    l->va_on = 0;   /* the VA macro and the LPI pins are clocked by the DSP's votes: not touched once it may be gone */
    plan_drop(&l->splan);
    plan_drop(&l->mplan);
    plan_drop(&l->xplan);
    plan_drop(&l->hplan);
    finish(l, now);
}

int audio_live_init(AudioLive *l, const AudioLiveConfig *c, const AudioSession *s, AudioLiveShared *sh)
{
    zero(l, sizeof *l);
    if (!s || !s->kept || !s->guard || !s->ops.dma || !sh || !sh->out.data || !sh->in.data) return -1;
    l->cfg = *c;
    if (l->cfg.level < 8) l->cfg.level = 8;
    if (l->cfg.level > 10) l->cfg.level = 10;
    if (l->cfg.level < 10) l->cfg.mic = 0;
    /* the request may be 243 (applied only under protection, live_gain_cap); 231 is what the passthrough graph plays */
    const unsigned cap = l->cfg.level >= 9 ? FS19XX_GAIN_CAP_PROTECTED : AUDIO_AMPS_TEST_GAIN;
    if (l->cfg.gain > cap) l->cfg.gain = cap;
    const int32_t ceiling = l->cfg.level >= 9 ? AUDIO_CEILING_MAX : AUDIO_CEILING_TEST;
    if (l->cfg.ceiling <= 0 || l->cfg.ceiling > ceiling) l->cfg.ceiling = ceiling;
    if (!l->cfg.idle_ms) l->cfg.idle_ms = AUDIO_LIVE_IDLE_MS;
    if (l->cfg.idle_ms > AUDIO_LIVE_IDLE_MAX_MS) l->cfg.idle_ms = AUDIO_LIVE_IDLE_MAX_MS;
    if (l->cfg.status_ms < 100) l->cfg.status_ms = 100;
    if (!l->cfg.answer_ms) l->cfg.answer_ms = AUDIO_ANSWER_MS;
    l->ops = s->ops;
    l->guard = s->guard;
    l->ports = s->ports;
    l->amps = (AudioAmps *)&s->amps;
    l->sh = sh;
    __atomic_store_n(&sh->amp_gain_requested, l->cfg.gain, __ATOMIC_RELEASE);
    store(&sh->input_fault, !l->cfg.mic);
    if (l->cfg.mic) {
        const int route = audio_mic_route_valid(c->mic_route) ? c->mic_route : AUDIO_MIC_ROUTE_LEGACY;
        const unsigned setting = (unsigned)(route + 1) | (c->mic_diag == 1 ? MicDiag : 0);
        mic_route_applied(l, setting, 1);
        __atomic_fetch_or(&sh->mic_control, MicGraphReleased | setting | (setting << MicAppliedShift) |
                          (1ull << MicGenerationShift), __ATOMIC_RELEASE);
    }
    /* speaker protection: the private files checked once; asked for by the boot argument (the sysctl may ask later) */
    const char *why = l->ops.spk_why;
    l->spk = c->spk_state;
    if (!l->spk) {
        why = "no memory for its state";
    } else if (l->ops.spk_params && l->ops.spk_re25) {
        why = 0;
        if (!spk_params_parse(&l->spk->prot, l->ops.spk_params, l->ops.spk_params_bytes, &why) &&
            !spk_re25_parse(&l->spk->prot, l->ops.spk_re25, l->ops.spk_re25_bytes, &why))
            l->prot_ready = 1;
    } else if (!why) {
        why = "no private files (PianoAudio/spk-prot.bin and spk-re25.bin)";
    }
    l->prot_why = l->prot_ready ? 0 : why;
    store(&sh->spk_prot, spk_request_valid(c->spk_prot) ? c->spk_prot : 0);
    /* MIPLAY: its private file checked once; asked for by the boot argument (the sysctl may ask later) */
    l->mip_why = l->ops.spk_dolby_why;
    if (!l->spk) {
        l->mip_why = "no memory for its state";
    } else if (l->ops.spk_dolby) {
        l->mip_why = 0;
        if (!spk_mip_parse(&l->spk->mip, l->ops.spk_dolby, l->ops.spk_dolby_bytes, &l->mip_why)) l->mip_ready = 1;
    } else if (!l->mip_why) {
        l->mip_why = "no private file (PianoAudio/spk-dolby.bin)";
    }
    store(&sh->miplay, spk_miplay_valid(c->miplay) ? c->miplay : 0);
    l->mip_gen = __atomic_load_n(&sh->miplay_gen, __ATOMIC_ACQUIRE);
    __atomic_store_n(&sh->age_signed, c->age_old ? 0 : 1, __ATOMIC_RELEASE);   /* duplex-age1: signed unless piano-audio-age=0 */
    l->prot_gen = __atomic_load_n(&sh->spk_prot_gen, __ATOMIC_ACQUIRE);
    store(&sh->duplex_hold, ar_hold_valid(c->duplex_hold) ? c->duplex_hold : 0);
    l->hold_gen = __atomic_load_n(&sh->duplex_hold_gen, __ATOMIC_ACQUIRE);
    LOG(l, HOLD ": request %d from the boot argument (0 off, 1 the two LPASS votes, 3 also the VA mclk; machdep.piano_audio_duplex_hold "
        "changes it, taken when both paths are down)%s", load(&sh->duplex_hold), l->cfg.mic ? "" : "; no Mic path at this level: inert");
    if (l->prot_ready)
        LOG(l, PROT ": ready: %u ACDB parameters (%u bytes) of the V/I and speaker graphs, Re25 for four channels; %s", l->spk->prot.count,
            l->spk->prot.bytes, c->spk_prot ? "on from the first start (piano-audio-spk=1)" : "off (piano-audio-spk=0; machdep.piano_audio_spk=1 turns it on)");
    else if (c->spk_prot)
        LOG(l, PROT ": OFF: %s; the speaker plays through the passthrough graph", why);
    {
        char words[96];
        const int request = load(&sh->miplay);
        if (l->mip_ready)
            LOG(l, MIP ": ready: %u parameters (%u bytes) of MIPLAY %#x, %u of them the Dolby DAP's; request %d (%s; piano-audio-miplay, "
                "machdep.piano_audio_miplay changes it, taken at the next start of the protection plan)", l->spk->mip.count, l->spk->mip.bytes,
                SPK_SG_MIP, l->spk->mip.dap, request, spk_miplay_text((uint32_t)request, words, sizeof words));
        else if (request)
            LOG(l, MIP ": OFF: %s; the protection plan runs without MIPLAY", l->mip_why);
    }
    return 0;
}

uint32_t audio_live_tick(AudioLive *l, const AdgObservation *o, uint32_t now_ms)
{
    if (l->finished) return 0;
    if (!l->started_at) {
        l->started_at = now_ms ? now_ms : 1;
        const int db10 = fs19xx_gain_db10(l->cfg.gain);
        LOG(l, "live: level %u: the Speaker%s on the DSP: playing gain %u (-%d.%d dB, cap %u), ceiling %d (max %d), master volume %s; down after "
            "%u ms without sound; ports %s", l->cfg.level, l->cfg.mic ? " and the Mic" : "", l->cfg.gain, -db10 / 10, -db10 % 10,
            live_gain_cap(l), l->cfg.ceiling, l->cfg.level >= 9 ? AUDIO_CEILING_MAX : AUDIO_CEILING_TEST, l->cfg.level >= 9 ? "on" : "off (level 9)",
            l->cfg.idle_ms, l->ports->name);
    }
    l->now = now_ms;
    const uint32_t epoch = __atomic_load_n(&l->sh->epoch,__ATOMIC_ACQUIRE), out = load(&l->sh->want_out);
    if (epoch != l->ledger_epoch || out != l->ledger_out) {
        if (epoch != l->ledger_epoch) {
            AudioRing *r = &l->sh->out;
            const uint32_t old = __atomic_load_n(&l->sh->output_head, __ATOMIC_ACQUIRE) - r->tail;
            if (old <= audio_ring_fill(r)) audio_ring_skip(r, old);
            l->spk_primed = 0;
            l->prime_left = AUDIO_LIVE_DEPTH;
            l->quiet_at = now_ms;
            l->spk_ending = 0;
            drift_reset(&l->spk_drift);
        }
        ledger(l, out ? AlStart : AlStop, audio_ring_fill(&l->sh->out), audio_ring_fill(&l->sh->out), 0, 0, 0);
        l->ledger_epoch=epoch; l->ledger_out=out;
    }
    const int g = o ? adg_observe(l->guard, o, now_ms) : l->guard->state;
    if (g == AdgOff) {go_off(l, now_ms); return 0;}
    l->held = g == AdgWait;
    if (l->held != l->was_held)
        LOG(l, l->held ? "live: dsp: PianoPower asks for quiet: nothing new goes to the DSP until it ends" : "live: dsp: quiet over");
    l->was_held = l->held;
    if (l->sleeping && l->held) {
        /* Shared charger quiet forbids new DSP packets, including undo. Mute
         * locally once, keep receiving DONE, then continue drain when released.
         * The PM owner's 8s deadline cancels an unbounded quiet. */
        if (!l->sleep_quiet) {
            l->sleep_quiet=1;
            if (audio_amps_off(l->amps)) l->sleep_failed=1;
            l->unmuted=0;
            if (l->spk_state==LiveRunning) {l->spk_mode=SpkQuiet; l->spk_ending=1;}
        }
    } else if (!l->sleep_failed) {
        spk_tick(l, now_ms);
        mic_tick(l, now_ms);
        hold_tick(l, now_ms);
    }
    if (l->aborting && l->spk_state != LiveStarting && l->spk_state != LiveRunning && l->spk_state != LiveStopping &&
        l->mic_state != LiveStarting && l->mic_state != LiveRunning && l->mic_state != LiveStopping && !l->hold_mask)
        finish(l, now_ms);
    if (l->finished) return 0;
    if (l->sleep_failed) audio_live_sleep_fail(l);
    if (l->sleeping && !l->parked) (void)audio_live_parked(l);
    l->done_batch = 0;
    if (l->spk_state == LiveRunning || l->mic_state == LiveRunning) return 5;   /* 40 ms queued at the DSP: a refill within 5 ms */
    if (l->spk_state == LiveStarting || l->spk_state == LiveStopping || l->mic_state == LiveStarting || l->mic_state == LiveStopping) return 2;
    if (l->hold_mask) return 2;   /* duplex-hold1: the hold plan moves (or is up with both paths about to start or down) */
    return AUDIO_LIVE_IDLE_TICK_MS;
}

void audio_live_receive(AudioLive *l, const uint8_t *packet, uint32_t length, uint32_t now_ms)
{
    if (l->finished || l->off) return;
    l->received++;
    ArAnswer a;
    const int kind = ar_parse(packet, length, &a);
    if (kind == ArBad && a.opcode == AR_OP_READ_DONE && a.dst_port == l->ports->data && l->mic_state == LiveRunning) {
        l->cap.bad_range++; l->cap.errors++; l->cap_error = 1;
        LOG(l, "live: dsp: READ_DONE refused: opcode %#x malformed payload %u token %#x domains %u->%u ports %#x->%#x",
            a.opcode, a.payload_bytes, a.token, a.src_domain, a.dst_domain, a.src_port, a.dst_port);
        return;
    }
    if (kind == ArWriteDone && l->spk_state == LiveRunning) {
        const int r = ar_pump_done(&l->pump, &a);
        if (r == 1) {++l->done_batch; return;}
        if (r == -1) {
            LOG(l, "live: dsp: WRITE_DONE refused: status %#x for %#llx (token %#x)", a.status, (unsigned long long)a.address, a.token);
            l->dsp_error = 1;
            return;
        }
    }
    if (kind == ArReadDone && l->mic_state == LiveRunning) {
        const int r = ar_pump_done(&l->cap, &a);
        if (r < 0 || (r == 1 && l->cap.returned <= 4)) {
            LOG(l, "live: dsp: READ_DONE wire: opcode %#x status %#x payload %u token %#x domains %u->%u ports %#x->%#x address %#llx handle %#x bytes %u offset %u",
                a.opcode, a.status, a.payload_bytes, a.token, a.src_domain, a.dst_domain, a.src_port, a.dst_port,
                (unsigned long long)a.address, a.handle, a.data_bytes, a.offset);
            LOG(l, "live: dsp: READ_DONE tail: timestamp %#llx flags %#x frames %u md-status %#x md-address %#llx md-handle %#x md-bytes %u result %d",
                (unsigned long long)a.timestamp, a.flags, a.num_frames, a.md_status,
                (unsigned long long)a.md_address, a.md_handle, a.md_bytes, r);
        }
        if (r == 1) {capture_period(l, &a, now_ms); return;}
        if (r == -1) {
            LOG(l, "live: dsp: READ_DONE refused: status %#x for %#llx (token %#x, %u bytes at %u)", a.status, (unsigned long long)a.address,
                a.token, a.data_bytes, a.offset);
            l->cap_error = 1;
            return;
        }
    }
    if (l->xplan.active && l->xplan.in_flight) {
        const int live_step = l->xplan.seq.pending_opcode == AR_OP_GET_CFG;
        if (plan_receive(l, &l->xplan, packet, length, now_ms)) {
            if (live_step && l->xplan.seq.error == ArSeqOk && !l->mon_stopping) live_report(l);
            return;
        }
    }
    if (plan_receive(l, &l->splan, packet, length, now_ms) || plan_receive(l, &l->mplan, packet, length, now_ms) ||
        plan_receive(l, &l->hplan, packet, length, now_ms)) return;
    if ((kind == ArWriteDone || kind == ArReadDone) && (l->spk_state == LiveStopping || l->mic_state == LiveStopping)) return;
    if (++l->stray <= 8)
        LOG(l, "live: dsp: a packet nobody waits for: opcode %#x token %#x to port %#x%s", a.opcode, a.token, a.dst_port, kind == ArBad ? " (malformed)" : "");
}

void audio_live_abort(AudioLive *l, int hard, uint32_t now_ms)
{
    if (l->finished) return;
    LOG(l, "live: stopped by the owner%s", hard ? " (without DSP commands)" : "");
    store(&l->sh->accepting, 0);
    store(&l->sh->capturing, 0);
    (void)audio_amps_off(l->amps);
    mon_stop(l);
    if (hard || l->off) {
        plan_drop(&l->splan);
        plan_drop(&l->mplan);
        plan_drop(&l->xplan);
        plan_drop(&l->hplan);
        finish(l, now_ms);
        return;
    }
    /* soft: each plan's undo commands; the owner ticks on until both are down (or stops hard) */
    __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)MicGraphReleased, __ATOMIC_ACQ_REL);
    l->aborting = 1;
    if (l->splan.active && l->splan.seq.state != ArSeqDown && l->splan.seq.state != ArSeqFailed) {
        ar_seq_abort(&l->splan.seq, ArSeqErrAbort, 0);
        l->spk_state = LiveStopping;
        l->spk_reason = AudioGuardOwner;
    }
    if (l->mplan.active && l->mplan.seq.state != ArSeqDown && l->mplan.seq.state != ArSeqFailed) {
        ar_seq_abort(&l->mplan.seq, ArSeqErrAbort, 0);
        l->mic_state = LiveStopping;
        l->mic_reason = AudioGuardOwner;
    }
    /* duplex-hold1: a hold is released by hold_tick once both paths are down (never before the Mic's own undo touched the VA macro) */
    if (l->spk_state != LiveStopping && l->mic_state != LiveStopping && !l->hold_mask) finish(l, now_ms);
}


void audio_live_sleep(AudioLive *l, uint32_t generation)
{
    if (l->sleeping) return;
    __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)MicGraphReleased, __ATOMIC_ACQ_REL);
    mic_window_end(l, "sleep", l->now);
    __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)MicWindowArmed, __ATOMIC_ACQ_REL);
    l->sleeping=1; l->sleep_quiet=0; l->sleep_generation=generation;
    store(&l->sh->accepting,0); store(&l->sh->capturing,0);
    mon_stop(l);   /* a monitor command in flight completes; nothing more is sent */
    /* Starting plans finish their existing command sequence; only then is the
     * ordinary stream fade/drain/undo used. Never abort a pending MEM_MAP. */
}

int audio_live_parked(AudioLive *l)
{
    if (!l->sleeping || l->sleep_failed || l->finished || l->off || l->spk_state==LiveOff || l->mic_state==LiveOff || l->timeouts ||
        l->splan.seq.undo_errors || l->mplan.seq.undo_errors || l->hold_failed == 2) return -1;
    if (l->parked) return 1;
    /* A prior quiet-stop may have already ACKed STOP/UNMAP. Its old pump
     * bookkeeping is no longer ownership; never clear an active plan. */
    if (l->spk_state==LiveDown && !l->splan.active && (l->splan.seq.state==ArSeqDown || l->splan.seq.state==ArSeqFailed) && !l->splan.in_flight && !l->splan.unsent) {
        l->stream_unsent=0; l->pump.in_flight=0;
    }
    if (l->mic_state==LiveDown && !l->mplan.active && (l->mplan.seq.state==ArSeqDown || l->mplan.seq.state==ArSeqFailed) && !l->mplan.in_flight && !l->mplan.unsent) {
        l->cap_unsent=0; l->cap.in_flight=0;
    }
    if (l->spk_state!=LiveDown || l->mic_state!=LiveDown || l->splan.in_flight || l->mplan.in_flight || plan_busy(&l->xplan) ||
        l->splan.unsent || l->mplan.unsent || l->stream_unsent || l->cap_unsent || l->pump.in_flight || l->cap.in_flight ||
        l->hold_mask || plan_busy(&l->hplan)) return 0;   /* duplex-hold1: the held clocks released first */
    /* Graph stops, UNMAPs, and plan-owned TDM/VA releases have all ACKed.
     * Preserve configured amps in powered-down mute, not terminal reset. */
    if (audio_amps_off(l->amps) || l->amps->reset_errors || l->amps->off_errors) return -1;
    dma_zero(l,0,AUDIO_DMA_BYTES);
    if (l->ops.tdm_pins && l->ops.tdm_pins(l->ops.ctx,0)) return -1;
    audio_ring_reset(&l->sh->out); audio_ring_reset(&l->sh->in);
    l->parked=1;
    ledger(l,AlPark,0,0,0,0,0);
    return 1;
}

int audio_live_rearm(AudioLive *l)
{
    if (!l->parked || audio_live_parked(l)!=1 || !l->guard || l->guard->state!=AdgRun) return -1;
    if (l->ops.tdm_pins && l->ops.tdm_pins(l->ops.ctx,1)) return -1;
    audio_ring_reset(&l->sh->out); audio_ring_reset(&l->sh->in);
    store(&l->sh->sound,0); store(&l->sh->have_sound,0);
    zero(&l->pump,sizeof l->pump); zero(&l->cap,sizeof l->cap);
    drift_reset(&l->spk_drift); drift_reset(&l->mic_drift);
    l->status_poll_active=0; l->stream_unsent=l->cap_unsent=0;
    l->sleeping=l->parked=0;
    if (l->cfg.mic) __atomic_fetch_or(&l->sh->mic_control, MicGraphReleased, __ATOMIC_RELEASE);
    ledger(l,AlRearm,0,0,0,0,0);
    return 0;
}

void audio_live_sleep_fail(AudioLive *l)
{
    __atomic_fetch_and(&l->sh->mic_control, ~(uint64_t)MicGraphReleased, __ATOMIC_ACQ_REL);
    mic_window_end(l, "sleep-failed", l->now);
    l->sleeping=1; l->sleep_failed=1;
    store(&l->sh->accepting,0); store(&l->sh->capturing,0);
    if (!l->sleep_safe) {
        l->sleep_safe=1;
        if (audio_amps_off(l->amps)) audio_amps_hold(l->amps);
        l->unmuted=0;
        LOG(l,"sleep-contract1: owner fail-safe mute; uncertain DSP mappings/backing retained");
    }
    /* No DSP abort, unmap, DMA zero, VA access or new acquisition. CPU ready
     * gate + cancellation keep the host alive; a health-fatal still wins. */
}
