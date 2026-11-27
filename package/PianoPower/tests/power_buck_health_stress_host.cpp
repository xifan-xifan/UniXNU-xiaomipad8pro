// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -std=c++20 -Wall -Wextra -Werror -Wno-c99-designator -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -D_Static_assert=static_assert -I.. -I../../include -x c++ power_buck_health_stress_host.cpp ../ChargePolicy.c ../PianoMca.c -o /tmp/power_buck_health_stress_host && /tmp/power_buck_health_stress_host
//
// Simulated time and synthetic firmware. Exercise the host policy, MCA wire
// codec and gauge I2C reads with the pump off. UCSI enters at the publication
// boundary of an agreeing connector-status pair.
#include "ChargeLink.h"
#include "gauge_model.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace pianopower;

static const char *scenario;
static unsigned checks;
#define CHECK(expr) do { \
  ++checks; \
  if (!(expr)) {std::fprintf(stderr, "%s (line %d): %s\n", scenario, __LINE__, #expr); std::exit(1);} \
} while (0)

struct Adapter {
  const char *name;
  int type;
  unsigned hv, rpBits, initialMa;
};

constexpr Adapter sdp{"5 V SDP", PIANO_CHG_T_SDP, 0, 8, 500};
constexpr Adapter rp{"5 V 3 A Rp", PIANO_CHG_T_DCP, 0, 2, 3000};
constexpr Adapter fixed{"fixed PD 9 V", PIANO_CHG_T_PD, 1, 2, 2000};
constexpr Adapter pps{"PPS 9 V", PIANO_CHG_T_PPS, 2, 2, 2500};
constexpr Adapter qc2{"QC2 9 V", PIANO_CHG_T_HVDCP, 1, 2, 1200};
constexpr Adapter qc3{"QC3 9 V", PIANO_CHG_T_HVDCP3, 1, 2, 1600};

using Reply = std::array<uint8_t, PIANO_MCA_RESPONSE_BYTES>;
constexpr uint32_t fixed5Rdo = 1u << 28 | 300u << 10 | 300u;
constexpr uint32_t vendor5Rdo = 0x7301f441u;

struct Bench {
  Adapter adapter;
  ChargeLink link;
  PianoMca mca{};
  PianoChargeContract contract{};
  piano_board_temps board{};
  piano_skin_temps skin{};
  PortState port{};
  test::Gauge gauges[2];
  GaugeReading packs[2]{};
  uint32_t pdos[7][3] = {{5000, 5000, 3000}};
  uint64_t now = 1000, packSampleMs = 0;
  uint32_t rdo = fixed5Rdo, bus = 5000, icl = 100, fcc = 500, fv = 4400, enable = 1;
  unsigned ppsWrites = 0, raises = 0, withdrawals = 0, stops = 0, resumes = 0;
  bool connected = true, vendorFallback = false, expectHeld = false;

  explicit Bench(Adapter source) : adapter(source) {
    if (pd()) {
      pdos[1][0] = adapter.hv == 2 ? 3300 : 9000;
      pdos[1][1] = adapter.hv == 2 ? 11000 : 9000;
      pdos[1][2] = adapter.hv == 2 ? 5000 : 2000;
    }
    piano_contract_init(&contract);
    piano_contract_link(&contract, 1);
    piano_contract_allow_firmware_pdos(&contract, 1);
    piano_mca_init(&mca, 1, adapter.hv != 0);
    mca.links = 1;
    mca.board_allowed = 1;
    mca.board_done = 7;
    LinkOptions options;
    options.strictContract = options.health = options.hvCaps = options.fvHot = true;
    options.hv = adapter.hv;
    options.qc = piano_charge_is_qc(adapter.type);
    options.rp = true;
    options.rpMax = 3000;
    options.kick = false;
    link.configure(mca, options);
    CHECK(!link.healthRefused && !link.cfg.qc3);
    CHECK(!piano_mca_write_allowed_for(&mca, PIANO_MCA_QC_PULSE, 0x20, 4));
    CHECK(!piano_mca_write_allowed_for(&mca, PIANO_MCA_PPS, piano_charge_pps_word(9000, 2550), 4));

    board.version = PIANO_BOARD_TEMPS_VERSION;
    board.valid = 15;
    board.state = PIANO_BOARD_TEMPS_RUNNING;
    for (auto &t : board.milli_c) t = 30000;
    skin.version = PIANO_SKIN_TEMPS_VERSION;
    skin.virtual_valid = 1;
    skin.virtual_milli_c = 30000;
    port.ok = true;
    port.rpBits = uint8_t(adapter.rpBits);
    for (auto &g : gauges) test::fillGauge(g, 3900, 300, 60, 3000, 5000, 300, 0);
  }

  bool pd() const {return adapter.type == PIANO_CHG_T_PD || adapter.type == PIANO_CHG_T_PPS;}

  void soc(int a, int b) {
    gauges[0].regs[kGaugeSoc] = uint16_t(a);
    gauges[1].regs[kGaugeSoc] = uint16_t(b);
  }

  void sample() {
    for (unsigned i = 0; i < 2; ++i) {
      I2cBus busIo(gauges[i].io());
      packs[i] = readGauge(busIo);
      CHECK(gauges[i].sda() && gauges[i].scl());
    }
    packSampleMs = now;
  }

  void cable(bool attached) {
    connected = attached;
    bus = attached ? 5000 : 0;
    rdo = attached ? fixed5Rdo : 0;
  }

  void reportVendor5V() {
    // cp-first2: a readable list with vSafe5V at slot 2 and an unmapped object-7 RDO.
    const uint32_t offer[][3] = {{12000, 12000, 3000}, {5000, 5000, 3000}, {15000, 15000, 2000}, {20000, 20000, 1500}};
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned j = 0; j < 3; ++j) pdos[i][j] = offer[i][j];
    rdo = vendor5Rdo;
  }

  void publish() {
    piano_contract_offer_pdos(&contract, mca.pdo, mca.pdos, connected && pd());
    contract.stage_pdos = 0;
    contract.stage_connected = contract.stage_sink = connected;
    contract.stage_pd = connected && pd();
    contract.stage_rdo = contract.stage_pd ? rdo : 0;
    piano_contract_publish(&contract, now);
  }

  LinkOutcome decide() {
    port.sourceSeen = port.attached = connected;
    LinkView view;
    view.nowMs = now;
    view.chargerPdUp = view.safetyFresh = view.cpOff = view.dtptValid = true;
    view.chargerPdUpMs = 1;
    view.contract = &contract;
    view.board = &board;
    view.skin = &skin;
    view.port = &port;
    view.packs = packs;
    view.packsFresh = packSampleMs && now - packSampleMs <= 45000;
    view.packsMs = packSampleMs;
    LinkOutcome outcome;
    link.step(mca, view, outcome);
    CHECK(!outcome.resync && !outcome.refused && !outcome.full);
    return outcome;
  }

  Reply request(uint8_t (&wire)[PIANO_MCA_REQUEST_BYTES], uint32_t opcode) {
    CHECK(piano_mca_next(&mca, wire) == sizeof wire);
    CHECK(piano_contract_u32(wire + 8) == opcode);
    Reply reply{};
    piano_contract_put32(reply.data(), PIANO_MCA_OWNER);
    piano_contract_put32(reply.data() + 4, PIANO_MCA_TYPE_REQUEST);
    piano_contract_put32(reply.data() + 8, opcode);
    piano_contract_put32(reply.data() + 12, piano_contract_u32(wire + 12));
    piano_contract_put32(reply.data() + 20, piano_contract_u32(wire + 16));
    return reply;
  }

  void readCycle() {
    if (mca.cursor >= McaFields) piano_mca_refresh(&mca);
    for (unsigned i = 0; i < McaFields; ++i) {
      uint8_t wire[PIANO_MCA_REQUEST_BYTES];
      auto reply = request(wire, PIANO_MCA_OP_READ);
      uint8_t *payload = reply.data() + 24;
      uint32_t value = 0;
      switch (piano_contract_u32(wire + 12)) {
        case PIANO_MCA_BUS_VOLTAGE: value = bus * 1000; break;
        case PIANO_MCA_BUS_CURRENT: value = icl * 1000; break;
        case PIANO_MCA_REAL_TYPE: value = connected ? adapter.type : PIANO_CHG_T_UNKNOWN; break;
        case PIANO_MCA_ICL: value = icl; break;
        case PIANO_MCA_AICL_STATUS: value = icl; break;
        case PIANO_MCA_CHARGE_ENABLE: value = enable; break;
        case PIANO_MCA_TERM_VOLT: value = piano_charge_fv_raw(fv); break;
        case PIANO_MCA_PD_STATE: value = connected && pd() ? 2 : 0; break;
        case PIANO_MCA_PDOS:
          if (connected && pd()) {
            for (unsigned j = 0; j < 7; ++j)
              for (unsigned k = 0; k < 3; ++k)
                piano_contract_put32(payload + (j * 3 + k) * 4, pdos[j][k]);
            continueRead(reply, i);
            continue;
          }
          break;
        case PIANO_MCA_VERIFY: case PIANO_MCA_LPD_STATUS: break;
        default: piano_contract_put32(reply.data() + 16, 1); break;
      }
      piano_contract_put32(payload, value);
      continueRead(reply, i);
    }
    publish();
  }

  void continueRead(const Reply &reply, unsigned index) {
    CHECK(piano_mca_receive(&mca, reply.data(), reply.size()) ==
          (index + 1 == McaFields ? McaCycleDone : McaAnswer));
  }

  Reply transmit() {
    CHECK(mca.queued && !mca.in_flight);
    const auto write = mca.queue[0];
    CHECK(piano_charge_write_allowed(&link.cfg, write.property, write.value, write.bytes));
    uint8_t wire[PIANO_MCA_REQUEST_BYTES];
    auto reply = request(wire, PIANO_MCA_OP_WRITE);
    link.sent(mca);
    const uint32_t value = piano_contract_u32(wire + 20);
    CHECK(piano_contract_u32(wire + 12) == write.property && value == write.value);
    if (expectHeld && enable) CHECK(write.property == PIANO_MCA_CHARGE_ENABLE && value == 0);
    switch (write.property) {
      case PIANO_MCA_ICL: icl = value; break;
      case PIANO_MCA_FCC: fcc = value; break;
      case PIANO_MCA_TERM_VOLT: fv = value; break;
      case PIANO_MCA_CHARGE_ENABLE:
        CHECK(!value || !expectHeld);
        enable = value;
        if (enable) ++resumes;
        else ++stops;
        break;
      case PIANO_MCA_PPS: {
        const uint32_t ma = (value & 0xffff) * 50;
        CHECK(adapter.hv == 2 && value >> 16 == 450 && ma >= 500 && ma <= 2500 && ma <= pdos[1][2]);
        CHECK(link.plan.charge && !expectHeld && fv == 4400);
        if (bus == 5000) {checkBarrier(); ++raises;}
        ++ppsWrites;
        bus = 9000;
        rdo = 2u << 28 | 450u << 9 | ma / 50;
        break;
      }
      case PIANO_MCA_PD_FIXED: case PIANO_MCA_QC_VOLT:
        CHECK(connected && (value == 5000 || value == 9000));
        checkBarrier();
        if (value == 9000) {
          CHECK(link.plan.charge && !expectHeld && fv == 4400);
          ++raises;
        } else ++withdrawals;
        bus = value;
        rdo = bus == 9000 ? (2u << 28 | 200u << 10 | 200u) : fixed5Rdo;
        if (bus == 5000 && vendorFallback) reportVendor5V();
        break;
      default: CHECK(false);  // The buck has no SC8541, MiPPS or QC pulse writes.
    }
    return reply;
  }

  void checkBarrier() const {
    CHECK(icl <= 500 && fcc <= 500 && link.in.icl_ma <= 500);
  }

  void answer(const Reply &reply, bool late = false) {
    CHECK(piano_mca_receive(&mca, reply.data(), reply.size()) == (late ? McaLateWritten : McaWritten));
    link.answered(mca, now, late);
  }

  void drainWrites() {
    while (mca.queued) answer(transmit());
    if (bus == 9000) CHECK(icl <= 2500);
  }

  void tick(uint64_t elapsed = 5000) {
    now += elapsed;
    sample();
    readCycle();
    CHECK(decide().decided);
    drainWrites();
  }

  void ticks(unsigned count) {
    for (unsigned i = 0; i < count; ++i) tick();
  }

  void startCharging() {
    for (unsigned i = 0; i < 80; ++i) {
      tick(1000);
      if (icl == adapter.initialMa && (!adapter.hv || link.st.hv == PIANO_CHG_HV_ON)) break;
    }
    CHECK(enable && icl == adapter.initialMa && bus == (adapter.hv ? 9000u : 5000u));
  }

  void checkHeld() const {
    CHECK(link.st.hold && link.plan.why_off == PIANO_CHG_OFF_LIMIT && !link.plan.charge && !enable);
  }
};

static void healthStress(Adapter adapter) {
  scenario = adapter.name;
  Bench b(adapter);
  b.soc(79, 78);
  b.startCharging();

  // The higher pack is the one at 80%, even though the pack average is below it.
  b.soc(78, 80);
  b.expectHeld = true;
  b.tick(adapter.hv == 2 ? b.link.st.hv_asked_ms + 4000 - b.now : 1000);
  b.checkHeld();
  CHECK(b.stops == 1 && !b.resumes);

  // Over two hours of ordinary 5 s cycles for each source. Include the full
  // hold band, temperature transitions, and cable loss with a live inhibit.
  const unsigned raises = b.raises, ppsWrites = b.ppsWrites;
  for (unsigned i = 0; i < 1500; ++i) {
    b.soc(75, 79 - int(i % 4));
    if (i == 100 || i == 900) b.cable(false);
    if (i == 103 || i == 907) b.cable(true);
    if (i == 200) b.gauges[0].regs[kGaugeTemperature] = 2732 + 480;
    if (i == 300) b.gauges[0].regs[kGaugeTemperature] = 2732 + 550;
    if (i == 500) b.gauges[0].regs[kGaugeTemperature] = 2732 + 459;
    b.tick();
    b.checkHeld();
    CHECK(b.raises == raises && b.ppsWrites == ppsWrites && !b.resumes);
    if (i >= 10) CHECK(b.bus == (b.connected ? 5000u : 0u));
    if (i == 250 || i == 350) CHECK(b.fv == 4060);
    if (i == 501) CHECK(b.fv == 4400);
  }
  CHECK(b.withdrawals == (adapter.hv ? 1u : 0u));
  b.expectHeld = false;
  b.soc(74, 75);
  b.tick();
  CHECK(b.enable && !b.link.st.hold && b.resumes == 1);
  b.startCharging();

  b.soc(81, 76);
  b.expectHeld = true;
  b.tick(1000);
  b.checkHeld();
  CHECK(b.stops == 2);
}

static void detachedHealth() {
  scenario = "health inhibit without a cable";
  Bench b(sdp);
  b.cable(false);
  b.soc(79, 80);
  b.expectHeld = true;
  b.tick();
  b.checkHeld();
  CHECK(b.stops == 1);

  // Firmware can restore the enable bit while disconnected. Reassert the hold
  // after the normal readback threshold, before the next cable attach.
  b.enable = 1;
  b.soc(78, 77);
  b.ticks(2);
  b.checkHeld();
  CHECK(b.stops == 2);
  b.cable(true);
  b.tick();
  b.checkHeld();
  CHECK(!b.resumes);
}

static void ppsCadence() {
  scenario = "PPS 4 s cadence";
  Bench b(pps);
  b.startCharging();
  for (unsigned i = 0; i < 1800; ++i) {
    const uint64_t last = b.link.st.hv_asked_ms;
    const unsigned writes = b.ppsWrites;
    b.now = last + 3999;
    if (b.now - b.packSampleMs >= 30000) b.sample();
    b.readCycle();
    CHECK(b.decide().decided);
    b.drainWrites();
    CHECK(b.ppsWrites == writes);
    ++b.now;
    CHECK(b.decide().decided);
    b.drainWrites();
    CHECK(b.ppsWrites == writes + 1 && b.link.st.hv_asked_ms == b.now);
    CHECK(b.contract.protocol == PIANO_CONTRACT_PPS && b.contract.mv == 9000 && b.contract.ma == 2500);
    CHECK(b.bus == 9000 && b.icl == 2500 && b.enable && b.raises == 1 && !b.withdrawals);
  }
}

enum class PpsFault { silent, late, expired, apdoDrop };

static void ppsWithdrawal(PpsFault fault, const char *name) {
  scenario = name;
  Bench b(pps);
  b.startCharging();
  b.tick(0);  // Finish the current-limit readback before advancing to the worker deadline.
  const auto last = b.link.st.hv_asked_ms;
  b.now = last + 4000;
  if (fault == PpsFault::apdoDrop) {
    for (auto &field : b.pdos[1]) field = 0;
    b.vendorFallback = true;
    b.tick(0);
    CHECK(!b.contract.valid && b.ppsWrites == 1);
  } else {
    CHECK(b.decide().decided && b.mca.queued == 1 && b.mca.queue[0].property == PIANO_MCA_PPS);
    if (fault == PpsFault::expired) {
      // Stall the worker with an unsent refresh; the watchdog revokes it at 8 s.
      b.now = last + 7999;
      CHECK(!b.decide().decided && b.mca.queued == 1);
      ++b.now;
      CHECK(b.decide().decided);
      b.drainWrites();
      CHECK(b.ppsWrites == 1);
    } else {
      const auto reply = b.transmit();
      b.now += 3000;  // The owner's default MCA write timeout.
      b.link.timeout(b.mca);
      CHECK(b.mca.last_result == -60 && b.link.st.pps_failed);
      if (fault == PpsFault::late) {
        b.now = last + 8000;
        b.answer(reply, true);
        CHECK(b.mca.late_answers == 1 && b.link.st.pps_failed && b.link.st.hv_asked_ms == last);
      }
    }
  }
  const unsigned writes = b.ppsWrites;
  for (unsigned i = 0; i < 15; ++i) {
    b.tick(1000);
    CHECK(b.ppsWrites == writes && b.raises == 1);
    if (i >= 4) CHECK(b.bus == 5000 && b.icl > 100);
  }
  CHECK(b.withdrawals == 1 && b.link.st.hv == PIANO_CHG_HV_BLOCKED);
  CHECK(b.link.st.volt_set_prop == PIANO_CHG_PD_FIXED && b.link.st.volt_want_prop == PIANO_CHG_PD_FIXED);
  CHECK(b.enable);
  if (b.vendorFallback) {
    CHECK(b.contract.rdo == vendor5Rdo && b.mca.pdos == 4);
    CHECK((b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.link.in.rdo_object == 7);
  }
  b.ticks(60);
  CHECK(b.bus == 5000 && b.icl == (b.vendorFallback ? 1500u : 3000u));
  CHECK(b.ppsWrites == writes && b.withdrawals == 1);
}

static void gaugeRecovery() {
  scenario = "dual gauge glitch and NACK";
  Bench b(pps);
  b.soc(74, 76);
  auto &a = b.gauges[0];
  auto &other = b.gauges[1];
  // Corrupt voltage accompanied by an apparently good SOC from that pack.
  // Leave it broken past the 9 V entry delay to prove the gauges gate entry.
  a.regs[kGaugeVoltage] = 0xffff;
  b.ticks(5);
  CHECK(!b.packs[0].ok && b.packs[1].ok && !(b.link.in.valid & PIANO_CHG_IN_GAUGES));
  CHECK(!b.ppsWrites && b.bus == 5000);
  a.regs[kGaugeVoltage] = 3900;
  a.regs[kGaugeCurrent] = 0x7fff;
  b.tick();
  CHECK(!b.packs[0].ok && b.packs[1].ok && !(b.link.in.valid & PIANO_CHG_IN_GAUGES));
  CHECK(!b.ppsWrites && b.bus == 5000);
  a.replies[kGaugeVoltage] = {0xffff, 3900};
  a.replies[kGaugeCurrent] = {0x7fff, 300};
  other.replies[kGaugeSoc] = {0xffff, 76};
  other.replies[kGaugeAverageCurrent] = {0x8000, 300};
  other.nackReadNumber = other.reads + 2;
  other.nackOnce = true;
  b.tick();
  CHECK(b.packs[0].ok && b.packs[1].ok && !other.nackReadNumber);
  CHECK(b.packs[0].wordRereads == 2 && b.packs[1].wordRereads == 2);
  CHECK(b.link.in.vbat_max_mv == 3900 && b.link.in.ibat_ma == 600 && b.link.in.soc_max_pct == 76);
  CHECK(b.packs[1].averageMa == 300);
  b.startCharging();

  const unsigned resumes = b.resumes;
  b.soc(80, 79);
  b.expectHeld = true;
  b.tick(1000);
  b.checkHeld();
  const unsigned writes = b.ppsWrites;
  b.soc(76, 75);
  a.regs[kGaugeCurrent] = 0x7fff;
  other.present = false;
  b.tick();
  CHECK(!b.packs[0].ok && b.packs[1].error == I2cResult::nackAddress);
  CHECK(!(b.link.in.valid & PIANO_CHG_IN_GAUGES));
  b.checkHeld();
  CHECK(b.ppsWrites == writes);
  a.regs[kGaugeCurrent] = 300;
  other.present = true;
  b.tick();
  CHECK(b.link.in.soc_max_pct == 76 && b.link.in.ibat_ma == 600);
  b.checkHeld();
  b.expectHeld = false;
  b.soc(75, 74);
  b.tick();
  CHECK(b.enable && !b.link.st.hold && b.resumes == resumes + 1);
}

static void hotFloat() {
  scenario = "hot float hysteresis";
  Bench b(pps);
  b.startCharging();
  auto &warm = b.gauges[1];
  warm.regs[kGaugeVoltage] = 4055;
  const auto temperature = [&](int deciC) {
    warm.regs[kGaugeTemperature] = uint16_t(2732 + deciC);
    b.tick(1000);
  };
  temperature(479);
  CHECK(b.fv == 4400 && b.enable && !b.link.st.term_hot);
  temperature(480);
  CHECK(b.fv == 4060 && !b.enable && b.link.st.term_hot && b.link.st.fv_hold);
  CHECK(b.link.plan.why_off == PIANO_CHG_OFF_WARM_FV);
  warm.regs[kGaugeVoltage] = 3910;
  temperature(460);
  CHECK(b.fv == 4060 && !b.enable && b.link.st.fv_hold && b.bus == 5000);
  warm.regs[kGaugeVoltage] = 3909;
  temperature(460);
  CHECK(b.fv == 4060 && b.enable && !b.link.st.fv_hold);
  warm.regs[kGaugeVoltage] = 4055;
  temperature(460);
  CHECK(b.fv == 4060 && !b.enable && b.link.st.fv_hold);
  temperature(459);
  CHECK(b.fv == 4400 && b.enable && !b.link.st.term_hot && !b.link.st.fv_hold && b.bus == 5000);
  b.startCharging();
}

static void defaults() {
  scenario = "daily defaults";
  const LinkOptions options;
  CHECK(options.stopPct == 80 && options.resumePct == 75 && !options.qc3);
  PianoChargeConfig cfg{};
  piano_charge_config_default(&cfg);
  CHECK(cfg.stop_pct == 80 && cfg.resume_pct == 75);
  // Boot argument defaults belong to the kext translation unit.
  std::ifstream source("../PianoPower.cpp");
  CHECK(source.is_open());
  const std::string code{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
  for (const char *setting : {"gArgs.b4pd = bootArg(\"piano-power-b4pd\", 0) == 1;",
                              "gArgs.cp = bootArg(\"piano-power-cp\", 0) == 1;",
                              "gArgs.healthStop = bootArg(\"piano-power-health-stop\", 80);",
                              "gArgs.healthResume = bootArg(\"piano-power-health-resume\", 75);"})
    CHECK(code.find(setting) != std::string::npos);
}

int main() {
  defaults();
  for (const auto &adapter : {sdp, rp, fixed, pps, qc2, qc3}) healthStress(adapter);
  detachedHealth();
  ppsCadence();
  ppsWithdrawal(PpsFault::silent, "MCA refresh reply missing");
  ppsWithdrawal(PpsFault::late, "MCA refresh reply late");
  ppsWithdrawal(PpsFault::expired, "MCA refresh queued past expiry");
  ppsWithdrawal(PpsFault::apdoDrop, "APDO loss and cp-first2 5 V recovery");
  gaugeRecovery();
  hotFloat();
  std::printf("power_buck_health_stress_host: %u checks passed\n", checks);
}
