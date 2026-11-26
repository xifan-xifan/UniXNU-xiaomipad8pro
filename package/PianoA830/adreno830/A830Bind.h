/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * gpu/throttle1 (private/host-checks/20261124-gpu-throttle/ANALYSIS.md): which limit sets the GPU's cap, for the LV line and
 * machdep.piano_gpu_state. The installed y12/y12c consoles said "cause=skin25" on 264 of 292 lines while Low Power Mode's 342 MHz
 * ceiling (an external slot) was the cap and the governor sat below it: the guard's own cause ignores every other ceiling. Portable.
 */
#ifndef A830_BIND_H
#define A830_BIND_H
#include <stdint.h>
struct A830BindIn {
    uint8_t voted=0;        /* the acknowledged level */
    uint8_t cap=0;          /* the effective cap (A830Dcvs::ceiling: knob, thermal, external, boost cap, then the guard) */
    uint8_t top=14;         /* the qualified top */
    uint8_t guardCap=0;     /* the guard's ceiling (A830PowerState::qualificationCap) */
    uint8_t guardStep=14;   /* the guard's thermal cap before the permit clamp (its snapshot's cap) */
    uint8_t guardCause=0;   /* A830R3bThermal::Cause */
    bool latched=false;     /* the boot latch (<= 342 for the boot) or the low hold */
    bool opened=false;      /* the permit opened once (daily: armed itself) */
    bool high=false;        /* the permit is open now */
    uint8_t external=0;     /* the external ceiling slots' level (0 none) */
    const char *externalName="ceiling";   /* its holder: "lpm" (the power profile's slot), "thermal" (PianoThermal's passive), ... */
    uint8_t knob=14;        /* the knob / ACD-off ceiling */
    uint8_t boost=0;        /* a boost's cap (0 none) */
};
/* The name of the limit that holds the cap, in this order when several hold the same level: the guard's hard state, the permit
 * not yet open, an external slot, the guard's thermal table, the knob, a boost's cap; "top" when the cap is the qualified top. */
inline const char *a830BindName(const A830BindIn &in)
{
    static const char *const causes[]={"top","ladder","skin25","skin45","xo","skin-unknown","hard","skinband"};
    const uint8_t cap=in.cap;
    if(!cap)return "none";
    if(in.guardCap==cap || in.latched) {
        if(in.guardCause==6 || in.latched)return in.opened?"hard":"startup";
        if(!in.high && in.guardStep>cap)return in.opened?"hard":"permit";
    }
    if(in.external && in.external==cap)return in.externalName?in.externalName:"ceiling";
    if(in.guardCap==cap && in.guardStep==cap && cap<in.top)return causes[in.guardCause<=7?in.guardCause:0];
    if(in.knob==cap && cap<in.top)return "knob";
    if(in.boost && in.boost==cap)return "boostcap";
    return cap>=in.top?"top":"other";
}
/* "demand" when the governor votes below the cap (no limit binds), else the binding limit's name. */
inline const char *a830BindWhy(const A830BindIn &in) {return in.voted && in.voted<in.cap?"demand":a830BindName(in);}
#endif
