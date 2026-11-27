#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Extract, check and emit piano's SM8750 DisplayPort PHY data.

The source revision, input files and table format are in data/dp/README.md.
The extractor handles the initializers and literal settings selected by the
pinned SM8750 configuration. It is not a C or device tree parser.
"""
import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PINNED_COMMIT = '7f8d6bedde479a01f4a05d6d0581b43c7fd20a01'
PHY = 'qmp-sm8750-dp'
FILENAME = f'{PHY}.tbl'
COMPATIBLE = 'qcom,sm8750-qmp-usb3-dp-phy'
QMP = 'drivers/phy/qualcomm/phy-qcom-qmp-'
DRIVER = QMP + 'combo.c'
DTS = 'arch/arm64/boot/dts/qcom/'
BOARD = DTS + 'sm8750-xiaomi-piano.dts'
SOC = DTS + 'sm8750.dtsi'
HEADERS = tuple(QMP + suffix + '.h' for suffix in (
    'dp-phy', 'dp-phy-v6', 'qserdes-com-v6', 'qserdes-txrx-v6', 'qserdes-txrx-v8',
))
SOURCES = tuple(sorted((DRIVER, BOARD, SOC, *HEADERS)))

BLOCK_FIELDS = {
    'dp_com': 'com', 'serdes': 'dp_serdes', 'tx_a': 'dp_txa', 'tx_b': 'dp_txb', 'dp_phy': 'dp_dp_phy',
}
REGISTER_FIELDS = {
    'vco_div': 'QPHY_DP_PHY_VCO_DIV',
    'tx_swing': 'QPHY_TX_TX_DRV_LVL',
    'tx_pre_emphasis': 'QPHY_TX_TX_EMP_POST1_LVL',
}
RATES = {'rbr': 1620, 'hbr': 2700, 'hbr2': 5400, 'hbr3': 8100}
WRITE_FIELDS = {
    'serdes common': 'dp_serdes_tbl',
    **{f'serdes {name}': f'serdes_tbl_{name}' for name in RATES},
    'tx shared': 'dp_tx_tbl',
}
MATRIX_FIELDS = {
    'matrix swing-hbr-rbr': 'swing_hbr_rbr',
    'matrix pre-emphasis-hbr-rbr': 'pre_emphasis_hbr_rbr',
    'matrix swing-hbr3-hbr2': 'swing_hbr3_hbr2',
    'matrix pre-emphasis-hbr3-hbr2': 'pre_emphasis_hbr3_hbr2',
}
SECTION_TAGS = {
    'layout qmp': 'op', 'layout tuning': 'o', 'rates link': 'l',
    **{section: 'r' for section in WRITE_FIELDS}, 'aux dp-phy': 'r',
    **{section: 'm' for section in MATRIX_FIELDS},
}
# The row schema supplies both the input syntax and the canonical output.
NAME = (r'[a-z][a-z0-9_]*', str, '{}')
DEC = (r'[0-9]+', int, '{}')
HEX4 = (r'0x[0-9a-fA-F]{4}', lambda s: int(s, 16), '0x{:04x}')
HEX2 = (r'0x[0-9a-fA-F]{2}', lambda s: int(s, 16), '0x{:02x}')
ROW_FIELDS = {
    'o': (NAME, HEX4), 'p': (NAME, DEC), 'r': (HEX4, HEX2),
    'l': (NAME, DEC, DEC, DEC, DEC, DEC), 'm': (HEX2,) * 4,
}
IDENT = r'[A-Za-z_]\w*'
NUMBER = r'(?:0[xX][0-9a-fA-F]+|[0-9]+)'
BLOCK_BYTES = 0x400
AUX_OFFSETS = range(0x20, 0x48, 4)


class Bad(ValueError):
    pass


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def match_one(pattern, source, description):
    matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise Bad(f'expected one {description}, found {len(matches)}')
    return matches[0]


def body(source, declaration, description):
    return match_one(declaration + r'\s*\{(.*?)^\}', source, description)[1]


def field(source, name, value):
    return match_one(r'\.' + name + r'\s*=\s*(' + value + r')\s*,', source, f'.{name}')[1]


def register_macros(sources):
    offsets = {}
    for path in HEADERS:
        for name, value in re.findall(r'^\s*#\s*define[ \t]+(' + IDENT + r')[ \t]+(' + NUMBER + r')[ \t]*$',
                                      sources[path], re.MULTILINE):
            if name in offsets:
                raise Bad(f'duplicate register macro {name}')
            offsets[name] = int(value, 0)
    return offsets


def register_writes(source, cfg, field_name, offsets):
    array = field(cfg, field_name, IDENT)
    field(cfg, field_name + '_num', rf'ARRAY_SIZE\(\s*{array}\s*\)')
    init = body(source, rf'\b{array}\s*\[\s*\]\s*=', array)
    entry = rf'\s*QMP_PHY_INIT_CFG\(\s*({IDENT})\s*,\s*({NUMBER})\s*\)\s*,'
    if not re.fullmatch(rf'(?:{entry})+\s*', init):
        raise Bad(f'{array}: expected only QMP_PHY_INIT_CFG entries')
    return [('r', offsets[reg], int(value, 0)) for reg, value in re.findall(entry, init)]


def aux_writes(source, cfg, offsets):
    name = field(cfg, 'dp_aux_init', IDENT)
    init = body(source, rf'\b{name}\([^;{{}}]*\)', name)
    writes = re.findall(rf'\bwritel\(\s*({NUMBER})\s*,\s*qmp->dp_dp_phy\s*\+\s*'
                        r'(QSERDES_DP_PHY_AUX_CFG[0-9])\s*\);', init)
    expected = [f'QSERDES_DP_PHY_AUX_CFG{i}' for i in range(10)]
    if [reg for _, reg in writes] != expected or re.findall(r'\bQSERDES_DP_PHY_AUX_CFG[0-9]\b', init) != expected:
        raise Bad(f'{name}: expected one literal write for each AUX_CFG0..9 in order')
    return [('r', offsets[reg], int(value, 0)) for value, reg in writes]


def matrix(source, cfg, field_name):
    array = field(cfg, field_name, r'&\s*' + IDENT).lstrip('&').strip()
    init = body(source, rf'\b{array}\s*\[4\]\[4\]\s*=', array)
    row = r'\s*\{([^{}]+)\}\s*,?'
    if not re.fullmatch(rf'(?:{row}){{4}}\s*', init):
        raise Bad(f'{array}: expected four rows')
    return [('m', *(int(value.strip(), 0) for value in values.split(','))) for values in re.findall(row, init)]


def link_rates(source, cfg):
    name = field(cfg, 'configure_dp_clocks', IDENT)
    clocks = body(source, rf'\b{name}\([^;{{}}]*\)', name)
    serdes = body(source, r'\bqmp_combo_dp_serdes_init\([^;{}]*\)', 'serdes initialization')
    multiplier = match_one(r'clk_set_rate\(qmp->dp_link_hw\.clk,\s*dp_opts->link_rate\s*\*\s*([0-9]+)\);',
                           clocks, 'DP link clock multiplier')
    match_one(r'writel\(phy_vco_div,\s*qmp->dp_dp_phy\s*\+\s*cfg->regs\[QPHY_DP_PHY_VCO_DIV\]\);',
              clocks, 'DP VCO divider register write')
    rows = []
    for rate, mbps in RATES.items():
        table = f'serdes_tbl_{rate}'
        match_one(rf'case {mbps}:\s*qmp_configure\(qmp->dev,\s*serdes,\s*cfg->{table},\s*cfg->{table}_num\);',
                  serdes, f'{rate} serdes selection')
        selector, hz, divisor = match_one(
            rf'case {mbps}:\s*phy_vco_div\s*=\s*({NUMBER});\s*'
            r'pixel_freq\s*=\s*([0-9]+)UL\s*/\s*([0-9]+);\s*break;',
            clocks, f'{rate} VCO divider').groups()
        divisor = int(divisor)
        if divisor == 0 or int(hz) != mbps * 1_000_000 or int(hz) % divisor:
            raise Bad(f'{rate}: invalid VCO clock expression')
        rows.append(('l', rate, mbps, mbps * int(multiplier[1]), int(selector, 0), divisor, int(hz) // divisor))
    return rows


def extract(raw, commit):
    sources = {path: re.sub(r'/\*.*?\*/|//[^\n]*', ' ', data.decode(), flags=re.DOTALL)
               for path, data in raw.items()}
    offsets = register_macros(sources)
    source = sources[DRIVER]
    name = match_one(rf'\.compatible\s*=\s*"{COMPATIBLE}",\s*\.data\s*=\s*&({IDENT}),',
                     source, COMPATIBLE)[1]
    if name != 'sm8750_usb3dpphy_cfg':
        raise Bad(f'{COMPATIBLE}: unexpected configuration {name}')
    cfg = body(source, rf'\b{name}\s*=', name)
    layout_name = field(cfg, 'offsets', r'&\s*' + IDENT).lstrip('&').strip()
    layout = body(source, rf'\b{layout_name}\s*=', layout_name)
    regs_name = field(cfg, 'regs', IDENT)
    regs = body(source, rf'\b{regs_name}\s*\[[^\]]*\]\s*=', regs_name)

    window = int(match_one(
        rf'\busb_dp_qmpphy:\s*phy@\w+\s*\{{\s*compatible\s*=\s*"{COMPATIBLE}";\s*'
        rf'reg\s*=\s*<\s*{NUMBER}\s+{NUMBER}\s+0x0\s+({NUMBER})\s*>;',
        sources[SOC], 'QMP DT region')[1], 0)
    frequencies = match_one(r'&mdss_dp0_out\s*\{[^{}]*?link-frequencies\s*=\s*/bits/\s*64\s*<([0-9\s]+)>;',
                            sources[BOARD], 'piano DP endpoint frequencies')[1]
    board_rates = tuple(int(freq) for freq in frequencies.split())
    if board_rates != tuple(RATES[name] * 1_000_000 for name in ('rbr', 'hbr', 'hbr2')):
        raise Bad(f'piano DP endpoint: unexpected rates {board_rates}')

    sections = {
        'layout qmp': [('o', 'window', window),
                       *(('o', key, int(field(layout, member, NUMBER), 0)) for key, member in BLOCK_FIELDS.items()),
                       ('p', 'piano_max_mbps', max(board_rates) // 1_000_000)],
        'layout tuning': [('o', key, offsets[match_one(rf'\[{member}\]\s*=\s*({IDENT})\s*,', regs, member)[1]])
                          for key, member in REGISTER_FIELDS.items()],
        'rates link': link_rates(source, cfg),
        **{section: register_writes(source, cfg, field_name, offsets) for section, field_name in WRITE_FIELDS.items()},
        'aux dp-phy': aux_writes(source, cfg, offsets),
        **{section: matrix(source, cfg, field_name) for section, field_name in MATRIX_FIELDS.items()},
    }
    validate(sections)
    lines = ['# piano SM8750 QMP DisplayPort PHY tuning data.',
             '# Generated by tooling/dp/dp-phy-tables.py extract. See data/dp/README.md.',
             f'phy {PHY}', f'commit {commit}', *(f'source {path} {sha256(raw[path])}' for path in SOURCES)]
    for section, rows in sections.items():
        lines.extend(('', f'[{section}]'))
        for tag, *values in rows:
            fields = (fmt.format(value) for (_, _, fmt), value in zip(ROW_FIELDS[tag], values))
            lines.append(' '.join((tag, *fields)))
    return ('\n'.join(lines) + '\n').encode()


def git_sources(src, commit):
    raw = {}
    for path in SOURCES:
        result = subprocess.run(['git', '-C', str(src), 'show', f'{commit}:{path}'], capture_output=True)
        if result.returncode:
            raise Bad(f'{src}: cannot read {commit}:{path}\n{result.stderr.decode(errors="replace").strip()}')
        raw[path] = result.stdout
    return raw


def validate(sections):
    layout = sections['layout qmp']
    keys = [('o', key) for key in ('window', *BLOCK_FIELDS)] + [('p', 'piano_max_mbps')]
    if [(tag, key) for tag, key, _ in layout] != keys:
        raise Bad('layout qmp: invalid fields or order')
    values = {key: value for _, key, value in layout}
    window = values['window']
    blocks = [values[key] for key in BLOCK_FIELDS]
    if (blocks[0] != 0 or not blocks[-1] + BLOCK_BYTES <= window <= 0xffff or window % BLOCK_BYTES
            or any(base % BLOCK_BYTES for base in blocks)
            or any(b - a < BLOCK_BYTES for a, b in zip(blocks, blocks[1:]))):
        raise Bad('layout qmp: blocks must be aligned, ordered and within the QMP window')
    if values['piano_max_mbps'] != RATES['hbr2']:
        raise Bad('layout qmp: piano must be capped at HBR2')

    regs = sections['layout tuning']
    if [key for _, key, _ in regs] != list(REGISTER_FIELDS):
        raise Bad('layout tuning: invalid fields or order')
    if any(not 0 <= offset < BLOCK_BYTES or offset % 4 for _, _, offset in regs):
        raise Bad('layout tuning: invalid register offset')

    rates = sections['rates link']
    if [(name, mbps) for _, name, mbps, *_ in rates] != list(RATES.items()):
        raise Bad('rates link: expected ordered RBR through HBR3')
    for _, name, mbps, link_hz, selector, divisor, vco_hz in rates:
        if (link_hz != mbps * 100_000 or not 0 <= selector <= 3 or divisor not in (2, 4, 6)
                or vco_hz * divisor != mbps * 1_000_000):
            raise Bad(f'rates link: invalid {name} clock parameters')

    write_names = (*WRITE_FIELDS, 'aux dp-phy')
    for section in write_names:
        writes = sections[section]
        if not writes:
            raise Bad(f'{section}: empty register table')
        if any(not 0 <= off < BLOCK_BYTES or off % 4 or not 0 <= val <= 0xff for _, off, val in writes):
            raise Bad(f'{section}: invalid register offset or value')
    if [off for _, off, _ in sections['aux dp-phy']] != list(AUX_OFFSETS):
        raise Bad('aux dp-phy: expected AUX_CFG0..9 in order')

    for section in MATRIX_FIELDS:
        rows = sections[section]
        if len(rows) != 4 or any(len(row) != 5 for row in rows):
            raise Bad(f'{section}: expected a 4x4 matrix')
        for swing, (_, *values) in enumerate(rows):
            for pre, value in enumerate(values):
                valid = value == 0xff if swing + pre > 3 else 0 <= value <= 0x3f
                if not valid:
                    raise Bad(f'{section}: invalid code for swing {swing}, pre-emphasis {pre}')


def parse(raw, commit=PINNED_COMMIT):
    lines = [line.partition('#')[0].strip() for line in raw.decode().splitlines()]
    lines = [line for line in lines if line]
    if lines[:2] != [f'phy {PHY}', f'commit {commit}']:
        raise Bad(f'{FILENAME}: expected {PHY} at commit {commit}')
    if len(lines) < 2 + len(SOURCES):
        raise Bad(f'{FILENAME}: missing source provenance')
    for path, line in zip(SOURCES, lines[2:2 + len(SOURCES)]):
        if not re.fullmatch(rf'source {re.escape(path)} [0-9a-f]{{64}}', line):
            raise Bad(f'{FILENAME}: expected provenance for {path}')
    sections = {}
    current = None
    for line in lines[2 + len(SOURCES):]:
        if line.startswith('['):
            current = line[1:-1]
            if not line.endswith(']') or current not in SECTION_TAGS or current in sections:
                raise Bad(f'{FILENAME}: unexpected or duplicate section {line}')
            sections[current] = []
            continue
        tag, *values = line.split()
        if current is None or tag not in SECTION_TAGS[current]:
            raise Bad(f'{FILENAME}: unexpected row {line}')
        fields = ROW_FIELDS[tag]
        if len(values) != len(fields) or any(not re.fullmatch(pat, val) for (pat, _, _), val in zip(fields, values)):
            raise Bad(f'{FILENAME}: malformed row {line}')
        sections[current].append((tag, *(convert(value) for (_, convert, _), value in zip(fields, values))))
    if list(sections) != list(SECTION_TAGS):
        raise Bad(f'{FILENAME}: missing or out-of-order sections')
    validate(sections)
    return sections


def manifest_record(raw, sections):
    counts = ' '.join(f'{section.replace(" ", ":")}={len(rows)}' for section, rows in sections.items())
    return f'{FILENAME} {sha256(raw)} {counts}'


def manifest(raw, sections):
    return ('# The DP PHY table, its SHA-256 and every section\'s entry count.\n'
            '# Regenerate with tooling/dp/dp-phy-tables.py manifest data/dp after a deliberate extraction.\n'
            + manifest_record(raw, sections) + '\n')


def read_table(directory, commit=PINNED_COMMIT):
    if {p.name for p in directory.glob('*.tbl')} != {FILENAME}:
        raise Bad(f'{directory}: expected only {FILENAME}')
    raw = (directory / FILENAME).read_bytes()
    return raw, parse(raw, commit)


def check(directory, src=None, commit=PINNED_COMMIT):
    raw, sections = read_table(directory, commit)
    pinned = (directory / 'MANIFEST').read_text(encoding='utf-8').splitlines()
    pinned = [line for line in pinned if line.strip() and not line.startswith('#')]
    record = manifest_record(raw, sections)
    if pinned != [record]:
        raise Bad(f'MANIFEST mismatch for {FILENAME}\n  pinned: {pinned}\n  actual: {record}')
    if src is not None and extract(git_sources(src, commit), commit) != raw:
        raise Bad(f'{FILENAME}: does not equal a fresh extraction at {commit}')
    return sections


def header(sections):
    lines = [
        '// Generated by tooling/dp/dp-phy-tables.py header. See data/dp/README.md.',
        '#pragma once', '#include <stdint.h>', '',
        '#ifdef __cplusplus', '#define PIANO_DP_QMP_CONST inline constexpr',
        '#else', '#define PIANO_DP_QMP_CONST static const', '#endif', '',
        'typedef struct { uint16_t offset; uint8_t value; } PianoDpQmpWrite;',
        'typedef struct {',
        '    uint32_t link_mbps, link_clk_hz, vco_div_clk_hz;',
        '    uint8_t vco_div, vco_divisor;',
        '    const PianoDpQmpWrite *serdes;',
        '    uint16_t serdes_count;',
        '} PianoDpQmpRate;', '',
        '// Combo PHY window size, block offsets, board rate cap and block-relative tuning registers.',
    ]
    for tag, key, value in sections['layout qmp'] + sections['layout tuning']:
        literal = f'0x{value:04x}' if tag == 'o' else str(value)
        lines.append(f'PIANO_DP_QMP_CONST uint16_t piano_dp_qmp_{key} = {literal};')
    lines.extend(('', '// Register writes in source order. The TX table applies to both TX blocks.'))
    for section in (*WRITE_FIELDS, 'aux dp-phy'):
        name = section.replace(' ', '_').replace('-', '_')
        lines.append(f'PIANO_DP_QMP_CONST PianoDpQmpWrite piano_dp_qmp_{name}[] = {{')
        lines.extend(f'    {{0x{offset:04x}, 0x{value:02x}}},' for _, offset, value in sections[section])
        lines.extend(('};', ''))
    lines.append('PIANO_DP_QMP_CONST PianoDpQmpRate piano_dp_qmp_rates[] = {')
    for _, name, mbps, link_hz, selector, divisor, vco_hz in sections['rates link']:
        count = len(sections[f'serdes {name}'])
        lines.append(f'    {{{mbps}, {link_hz}, {vco_hz}, {selector}, {divisor}, piano_dp_qmp_serdes_{name}, {count}}},')
    lines.extend(('};', '', '// Rows are swing levels and columns are pre-emphasis levels. 0xff is invalid.'))
    for section in MATRIX_FIELDS:
        name = section.split()[1].replace('-', '_')
        lines.append(f'PIANO_DP_QMP_CONST uint8_t piano_dp_qmp_{name}[4][4] = {{')
        for _, *row in sections[section]:
            values = ', '.join(f'0x{value:02x}' for value in row)
            lines.append(f'    {{{values}}},')
        lines.extend(('};', ''))
    return '\n'.join((*lines, '#undef PIANO_DP_QMP_CONST', ''))


def synthetic_sources():
    # Keep an unrelated table in the driver to exercise reference selection.
    cfg = ['.offsets = &test_layout,', '.regs = test_regs,',
           '.dp_aux_init = test_aux,', '.configure_dp_clocks = test_clocks,']
    driver = ['static const int decoy[] = { 1, 2, 3 };',
              'static const struct layout test_layout = {',
              '.com = 0, .dp_serdes = 0x3000, .dp_txa = 0x3400, .dp_txb = 0x3800, .dp_dp_phy = 0x3c00,',
              '};', 'static const int test_regs[] = {']
    driver.extend(f'[{member}] = MOCK_REG_{i},' for i, member in enumerate(REGISTER_FIELDS.values()))
    driver.append('};')
    for i, field_name in enumerate(WRITE_FIELDS.values()):
        array = f'test_table_{i}'
        cfg.extend((f'.{field_name} = {array},', f'.{field_name}_num = ARRAY_SIZE({array}),'))
        driver.extend((f'static const int {array}[] = {{', '/* Source order is significant. */',
                       f'QMP_PHY_INIT_CFG(MOCK_B, {i + 1}), QMP_PHY_INIT_CFG(MOCK_A, 0x00),', '};'))
    for i, field_name in enumerate(MATRIX_FIELDS.values()):
        array = f'test_matrix_{i}'
        cfg.append(f'.{field_name} = &{array},')
        driver.append(f'static const u8 {array}[4][4] = {{')
        for swing in range(4):
            codes = ', '.join(str(i + pre) if swing + pre <= 3 else '0xff' for pre in range(4))
            driver.append(f'{{{codes}}},')
        driver.append('};')
    driver.extend(('static const struct cfg sm8750_usb3dpphy_cfg = {', *cfg, '};',
                   'static const struct match matches[] = {',
                   f'{{ .compatible = "{COMPATIBLE}", .data = &sm8750_usb3dpphy_cfg, }},', '};',
                   'static void test_aux(void *qmp)', '{'))
    driver.extend(f'writel({i}, qmp->dp_dp_phy + QSERDES_DP_PHY_AUX_CFG{i});' for i in range(10))
    driver.extend(('}', 'static int qmp_combo_dp_serdes_init(void *qmp)', '{', 'switch (dp_opts->link_rate) {'))
    for name, mbps in RATES.items():
        driver.append(f'case {mbps}: qmp_configure(qmp->dev, serdes, cfg->serdes_tbl_{name}, cfg->serdes_tbl_{name}_num);')
    driver.extend(('    }', '}', 'static int test_clocks(void *qmp)', '{', 'switch (dp_opts->link_rate) {'))
    for mbps, divisor in zip(RATES.values(), (2, 2, 4, 6)):
        driver.append(f'case {mbps}: phy_vco_div = 0x1; pixel_freq = {mbps * 1_000_000}UL / {divisor}; break;')
    driver.extend(('    }', 'writel(phy_vco_div, qmp->dp_dp_phy + cfg->regs[QPHY_DP_PHY_VCO_DIV]);',
                   'clk_set_rate(qmp->dp_link_hw.clk, dp_opts->link_rate * 100000);', '}'))
    macros = ['#define MOCK_A 0x04', '#define MOCK_B 0x08']
    macros.extend(f'#define MOCK_REG_{i} {i * 4}' for i in range(3))
    macros.extend(f'#define QSERDES_DP_PHY_AUX_CFG{i} {offset:#x}' for i, offset in enumerate(AUX_OFFSETS))
    sources = dict.fromkeys(SOURCES, b'')
    sources[DRIVER] = '\n'.join(driver).encode()
    sources[QMP + 'dp-phy.h'] = '\n'.join(macros).encode()
    sources[SOC] = (f'usb_dp_qmpphy: phy@1000 {{ compatible = "{COMPATIBLE}";'
                    'reg = <0x0 0x1000 0x0 0x4000>; };').encode()
    sources[BOARD] = b'&mdss_dp0_out { link-frequencies = /bits/ 64 <1620000000 2700000000 5400000000>; };'
    return sources


def expect_bad(call, message):
    try:
        call()
    except Bad as error:
        if message in str(error):
            return
        raise AssertionError(f'expected {message!r}, got {error!s}') from error
    raise AssertionError(f'expected rejection for {message}')


def self_test():
    sources = synthetic_sources()
    with tempfile.TemporaryDirectory(prefix='dp-phy-self-test-') as tmp:
        root = Path(tmp)
        src = root / 'src'
        src.mkdir()
        for path, blob in sources.items():
            dest = src / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(blob)
        git = ['git', '-C', str(src), '-c', 'core.hooksPath=/dev/null']
        subprocess.run([*git, 'init', '-q'], check=True)
        subprocess.run([*git, 'add', '--', *SOURCES], check=True)
        subprocess.run([*git, '-c', 'user.name=DP Test', '-c', 'user.email=dp@example.test',
                        'commit', '--no-gpg-sign', '-qm', 'Synthetic DP fixtures'], check=True)
        commit = subprocess.check_output([*git, 'rev-parse', 'HEAD'], text=True).strip()

        raw = extract(git_sources(src, commit), commit)
        sections = parse(raw, commit)
        assert sections['serdes common'] == [('r', 8, 1), ('r', 4, 0)]
        assert sections['serdes hbr3'][0] == ('r', 8, 5)
        assert sections['tx shared'][0] == ('r', 8, 6)
        assert sections['aux dp-phy'][-1] == ('r', 0x44, 9)
        assert sections['matrix pre-emphasis-hbr3-hbr2'][1] == ('m', 3, 4, 5, 0xff)
        assert sections['rates link'][-1] == ('l', 'hbr3', 8100, 810_000_000, 1, 6, 1_350_000_000)
        assert f'source {DRIVER} {sha256(sources[DRIVER])}'.encode() in raw
        (src / DRIVER).write_text('uncommitted changes\n', encoding='utf-8')
        assert extract(git_sources(src, commit), commit) == raw
        malformed = dict(sources)
        malformed[DRIVER] = sources[DRIVER].replace(b'QMP_PHY_INIT_CFG(MOCK_B, 1)', b'UNSUPPORTED(MOCK_B, 1)')
        expect_bad(lambda: extract(malformed, commit), 'expected only QMP_PHY_INIT_CFG')
        malformed[DRIVER] = sources[DRIVER].replace(b'writel(9,', b'writel(computed_value,')
        expect_bad(lambda: extract(malformed, commit), 'expected one literal write')

        table = root / FILENAME
        table.write_bytes(raw)
        man = root / 'MANIFEST'
        canonical = manifest(raw, sections)
        man.write_text(canonical, encoding='utf-8')
        assert check(root, src, commit) == sections
        for before, after, message in (
            (b'r 0x0008 0x01', b'r 0x0400 0x01', 'invalid register offset'),
            (b'r 0x0008 0x01', b'r 0x0009 0x01', 'invalid register offset'),
            (b'r 0x0044 0x09', b'r 0x0048 0x09', 'expected AUX_CFG0..9'),
            (b'[tx shared]', b'[serdes common]', 'duplicate section'),
            (b'm 0x00 0x01 0x02 0x03', b'm 0x00 0x01 0x02', 'malformed row'),
            (b'm 0x00 0x01 0x02 0x03\n', b'', 'expected a 4x4 matrix'),
            (b'm 0x00 0xff 0xff 0xff', b'm 0x00 0x01 0xff 0xff', 'invalid code'),
            (b'm 0x00 0x01 0x02 0x03', b'm 0x40 0x01 0x02 0x03', 'invalid code'),
            (b'162000000 1 2', b'162000001 1 2', 'invalid rbr clock parameters'),
        ):
            assert before in raw
            expect_bad(lambda: parse(raw.replace(before, after, 1), commit), message)
        man.write_text(canonical.replace('serdes:common=2', 'serdes:common=3'), encoding='utf-8')
        expect_bad(lambda: check(root, commit=commit), 'MANIFEST mismatch')
        man.write_text(canonical, encoding='utf-8')
        tampered = raw.replace(b'r 0x0008 0x01', b'r 0x0008 0x02', 1)
        table.write_bytes(tampered)
        expect_bad(lambda: check(root, commit=commit), 'MANIFEST mismatch')
        man.write_text(manifest(tampered, parse(tampered, commit)), encoding='utf-8')
        expect_bad(lambda: check(root, src, commit), 'does not equal a fresh extraction')

        (root / 'tables.h').write_text(header(sections), encoding='utf-8')
        consumer = root / 'consumer.cpp'
        consumer.write_text('''#include "tables.h"
#include "tables.h"
static_assert(piano_dp_qmp_serdes_common[0].offset == 8);
static_assert(piano_dp_qmp_rates[3].serdes[0].value == 5);
static_assert(piano_dp_qmp_rates[3].vco_div_clk_hz == 1350000000);
static_assert(piano_dp_qmp_aux_dp_phy[9].offset == 0x44);
static_assert(piano_dp_qmp_swing_hbr_rbr[3][1] == 0xff);
int main() { return 0; }
''', encoding='utf-8')
        subprocess.run(['clang++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(consumer), '-o', str(root / 'consumer')], check=True)
        subprocess.run([str(root / 'consumer')], check=True)
    print('DP PHY tables self-test: ok')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    commands = parser.add_subparsers(dest='command')
    extract_cmd = commands.add_parser('extract')
    extract_cmd.add_argument('src', type=Path)
    extract_cmd.add_argument('out', type=Path)
    check_cmd = commands.add_parser('check')
    check_cmd.add_argument('directory', type=Path)
    check_cmd.add_argument('--src', type=Path)
    header_cmd = commands.add_parser('header')
    header_cmd.add_argument('directory', type=Path)
    header_cmd.add_argument('out', type=Path)
    commands.add_parser('manifest').add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.self_test and args.command:
        parser.error('--self-test takes no command')
    try:
        if args.self_test:
            self_test()
        elif args.command == 'extract':
            raw = extract(git_sources(args.src, PINNED_COMMIT), PINNED_COMMIT)
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / FILENAME).write_bytes(raw)
        elif args.command == 'check':
            check(args.directory, args.src)
            detail = f', reproduced at {PINNED_COMMIT}' if args.src is not None else ''
            print(f'{FILENAME}: ok (format, MANIFEST{detail})')
        elif args.command == 'header':
            output = header(check(args.directory))
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(output, encoding='utf-8')
        elif args.command == 'manifest':
            sys.stdout.write(manifest(*read_table(args.directory)))
        else:
            parser.error('choose a command or --self-test')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'dp-phy-tables.py: {error}', file=sys.stderr)
        return 1
    except KeyError as error:
        print(f'dp-phy-tables.py: unresolved source macro {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
