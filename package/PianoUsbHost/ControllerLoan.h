// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. A loan's budget starts BEFORE changing roles.
#pragma once
#include <stdint.h>
namespace pianousbhost {
enum class LoanPhase:uint8_t { idle, starting, host, returning, device, held };
enum class LoanEnd:uint8_t { none, timeout, unplug, role, relay, failure };

inline bool loanDomainReady(bool verified, bool requireGadgetLink, bool configured, bool network)
{
  // A diagnostic loan proves the gadget link before interrupting it. A real UFP can arrive after that link has already dropped.
  return verified && (!requireGadgetLink || (configured && network));
}

struct ControllerLoan {
  LoanPhase phase=LoanPhase::idle;
  uint64_t deadline=0;
  bool diagnostic=false,sawDevice=false,latched=false;
  bool canBegin() const { return !latched && (phase==LoanPhase::idle || phase==LoanPhase::device); }
  bool begin(uint64_t now,uint32_t budgetMs,bool test) {
    if(!budgetMs || budgetMs>3600000 || !canBegin())return false;
    diagnostic=test;deadline=now+budgetMs;sawDevice=false;latched=true;phase=LoanPhase::starting;return true;
  }
  void started() {if(phase==LoanPhase::starting)phase=LoanPhase::host;}
  LoanEnd end(uint64_t now,bool relay,bool connected,bool hostRole,bool devicePresent,bool fault=false) {
    if(phase!=LoanPhase::starting && phase!=LoanPhase::host)return LoanEnd::none;
    if(fault)return LoanEnd::failure;
    if(now>=deadline)return LoanEnd::timeout;
    if(!relay && !diagnostic)return LoanEnd::relay;
    if(diagnostic) {if(devicePresent)sawDevice=true;return sawDevice&&!devicePresent?LoanEnd::unplug:LoanEnd::none;}
    if(!connected)return LoanEnd::unplug;
    if(!hostRole)return LoanEnd::role;
    return LoanEnd::none;
  }
  bool returning() {
    if(phase!=LoanPhase::starting && phase!=LoanPhase::host)return false;
    phase=LoanPhase::returning;return true;
  }
  void returned(bool stopped,bool deviceRole,bool reclaimed) {
    phase=stopped&&deviceRole&&reclaimed?LoanPhase::device:LoanPhase::held;deadline=0;
  }
  void disconnected() {if(phase!=LoanPhase::held)latched=false;}
};
inline const char *loanEndName(LoanEnd e) {
  switch(e) {case LoanEnd::timeout:return "deadline";case LoanEnd::unplug:return "unplug";
    case LoanEnd::role:return "role";case LoanEnd::relay:return "relay";case LoanEnd::failure:return "failure";default:return "none";}
}
} // namespace pianousbhost
