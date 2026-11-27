#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host checks of the tracked DP tables and generated header."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).with_name('dp-phy-tables.py').resolve()
DATA = ROOT / 'data/dp'

CONSUMER = r'''
#include "DpQmpPhy.h"
#include <iterator>

using namespace pianodp;

static_assert(std::size(piano_dp_qmp_serdes_common) == 20);
static_assert(std::size(piano_dp_qmp_tx_shared) == 11);
static_assert(std::size(piano_dp_qmp_aux_dp_phy) == 10);
static_assert(std::size(piano_dp_qmp_rates) == 4);
static_assert(piano_dp_qmp_rates[0].link_mbps == 1620);
static_assert(piano_dp_qmp_rates[1].link_mbps == 2700);
static_assert(piano_dp_qmp_rates[2].link_mbps == 5400);
static_assert(piano_dp_qmp_rates[3].link_mbps == 8100);
static_assert(piano_dp_qmp_piano_max_mbps == piano_dp_qmp_rates[2].link_mbps);
static_assert(piano_dp_qmp_rates[0].serdes == piano_dp_qmp_serdes_rbr);
static_assert(piano_dp_qmp_rates[1].serdes == piano_dp_qmp_serdes_hbr);
static_assert(piano_dp_qmp_rates[2].serdes == piano_dp_qmp_serdes_hbr2);
static_assert(piano_dp_qmp_rates[3].serdes == piano_dp_qmp_serdes_hbr3);
static_assert(piano_dp_qmp_dp_com == 0x0000);
static_assert(piano_dp_qmp_serdes == 0x3000);
static_assert(piano_dp_qmp_tx_a == 0x3400 && piano_dp_qmp_tx_b == 0x3800);
static_assert(piano_dp_qmp_dp_phy == 0x3c00 && piano_dp_qmp_window == 0x4000);
static_assert(piano_dp_qmp_vco_div == 0x70);
static_assert(piano_dp_qmp_tx_swing == 0x14 && piano_dp_qmp_tx_pre_emphasis == 0x0c);

constexpr bool valid_rates() {
    constexpr unsigned selectors[] = {1, 1, 2, 0};
    uint32_t previous_link = 0;
    uint32_t previous_vco = 0;
    unsigned index = 0;
    for (const auto& rate : piano_dp_qmp_rates) {
        if (rate.serdes_count != 9 || rate.vco_div != selectors[index++] ||
            rate.link_clk_hz <= previous_link || rate.vco_div_clk_hz < previous_vco ||
            rate.link_clk_hz != rate.link_mbps * 100000u ||
            rate.vco_div_clk_hz * uint64_t{rate.vco_divisor} != rate.link_mbps * uint64_t{1000000})
            return false;
        previous_link = rate.link_clk_hz;
        previous_vco = rate.vco_div_clk_hz;
    }
    return true;
}

constexpr bool valid_aux() {
    uint16_t offset = 0x20;
    for (const auto& write : piano_dp_qmp_aux_dp_phy) {
        if (write.offset != offset)
            return false;
        offset += 4;
    }
    return offset == 0x48;
}

constexpr bool valid_matrix(const uint8_t (&matrix)[4][4]) {
    for (unsigned swing = 0; swing < 4; ++swing) {
        for (unsigned pre = 0; pre < 4; ++pre) {
            const auto code = matrix[swing][pre];
            if (swing + pre > 3 ? code != 0xff : code > 0x3f)
                return false;
            if (pre > 0 && swing + pre <= 3 && code < matrix[swing][pre - 1])
                return false;
        }
    }
    return true;
}

static_assert(valid_rates());
static_assert(valid_aux());
static_assert(valid_matrix(piano_dp_qmp_swing_hbr_rbr));
static_assert(valid_matrix(piano_dp_qmp_pre_emphasis_hbr_rbr));
static_assert(valid_matrix(piano_dp_qmp_swing_hbr3_hbr2));
static_assert(valid_matrix(piano_dp_qmp_pre_emphasis_hbr3_hbr2));

constexpr bool valid_piano_selection() {
    DpQmpPhySettings settings;
    for (const auto& rate : piano_dp_qmp_rates) {
        const uint32_t clock = rate.link_clk_hz / 1000;
        if (!validDpLink({clock, 4}))
            return false;
        if (clock <= kPianoDpMaxRateKhz) {
            if (!lookupDpQmpPhy(clock, {2, 1}, settings) || settings.rate != &rate)
                return false;
        } else if (lookupDpQmpPhy(clock, {2, 1}, settings) || settings.rate ||
                   settings.swing || settings.preemphasis) {
            return false;
        }
    }
    return true;
}

static_assert(valid_piano_selection());
int main() { return 0; }
'''


def main():
    tool = [sys.executable, str(TOOL)]
    subprocess.run([*tool, 'check', str(DATA)], check=True)
    subprocess.run([*tool, '--self-test'], check=True)
    with tempfile.TemporaryDirectory(prefix='dp-phy-host-') as tmp:
        work = Path(tmp)
        subprocess.run([*tool, 'header', str(DATA), str(work / 'DpQmpTables.h')], check=True)
        consumer = work / 'consumer.cpp'
        consumer.write_text(CONSUMER, encoding='utf-8')
        exe = work / 'consumer'
        subprocess.run([os.environ.get('CXX') or 'c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'package/PianoDp'), '-I', str(work),
                        str(consumer), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('DP PHY tables host test: ok')


if __name__ == '__main__':
    main()
