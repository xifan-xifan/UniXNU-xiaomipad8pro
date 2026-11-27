#!/usr/bin/env python3
"""The pool install's ESP session rehearsed in the real session tool: tooling/persist/maint-session.py of this worktree in execute
mode, its out/persistent (installed.json = the base ESP the pool install starts from, tonight v13.1y10) and the pool package's saved
observations of the tablet (out/pool/install/evidence/v12-observations.json, the layout the pool change does not touch), with only the
device transport replaced (hash, peek, write, shell, fastboot, http, the restore job): every check of the session runs as on the tablet.
Uses the v13.1y10 rehearsal's transport stubs and cases with the pool ESP as the candidate.

The candidate: a pool ESP built by tooling/k273/build-pool-esp.py from the base with a rehearsal state (two made-up UUIDs; the record
is named ...-pool-rehearsal and removed at the end, refused if it exists), written by esp-delta over a host copy of the base ESP, read
back = its sha256, then rolled back (rollback esp: the backup the session took) = the base's sha256 again. Negatives (each must stop
where the session stops): the tablet's ESP not the installed one, a bad GPT, an 8 GiB FD under the 13 GiB ESP, the pool image not its
record's hash, the base image not its record's hash; a control (the base over itself). Not here: v13.1y10's ram4-seed (no RAM image
of the pool exists) and v10-fallback (the archived v10 backup, not the pool's).

  cd WORKTREE && python3 tooling/pool/rehearse-pool-session.py --results out/pool/install/rehearsals/session-HHMMSS [--candidate]
"""
import argparse
import copy
import importlib.util
import json
import os
import subprocess
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

WORK = Path(__file__).resolve().parents[2]
PERSIST = WORK / 'out/persistent'
PKG = WORK / 'out/pool/install'
SEED = '9bd6f902599c60cb89cec504317e124a0065e3f3a203143a41e82335b760076a'
REHEARSAL_UUIDS = ('5E6F7A8B-1111-2222-3333-444455556666', '9A0B0C0D-1111-2222-3333-444455556666')


def load(path):
    return json.loads(path.read_text())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--results', required=True, type=Path)
    ap.add_argument('--candidate', action='store_true', help='the pool ESP written and rolled back on host bytes')
    args = ap.parse_args()
    if Path.cwd().resolve() != WORK.resolve():
        ap.error('enter the real session worktree')
    results = args.results.resolve()
    if not results.is_relative_to((PKG / 'rehearsals').resolve()):
        ap.error(f'results under {PKG / "rehearsals"}')
    results.mkdir(parents=True, exist_ok=False)
    os.environ['PIANO_PERSIST_OUT'] = str(PERSIST)
    os.environ['PIANO_V110'] = str(PKG / 'maintenance')
    ledger = load(PERSIST / 'installed.json')
    OLD, OLD_SHA = ledger['image'].removesuffix('.img'), ledger['sha256']
    old_name = OLD + '.img'
    base = load(PERSIST / (OLD + '.json'))
    assert base['sha256'] == OLD_SHA and base['kind'] == 'k273-only' and base['default'] == 'macos-27.3', 'the base is not a k273-only ESP'
    NEW = OLD + '-pool-rehearsal'
    for suffix in ('.img', '.json'):
        if (PERSIST / (NEW + suffix)).exists():
            sys.exit(f'refused: {NEW}{suffix} exists (a rehearsal left behind?)')
    state = results / 'rehearsal-state.env'
    s = {'container': 'disk3', 'root_systemos_at_preflight': '07AD53B3-7565-4004-B574-F45FAC3753FB', 'fstab': 'yes'}
    for v, u in zip(('os', 'systemos'), REHEARSAL_UUIDS):
        s.update({f'{v}_preflight': 'ok', f'{v}_extracted': 'yes', f'{v}_verified': 'yes', f'{v}_volume_uuid': u})
    state.write_text(''.join(f'{k}={v}\n' for k, v in s.items()))
    try:
        r = subprocess.run([sys.executable, str(WORK / 'tooling/k273/build-pool-esp.py'), '--base', str(PERSIST / (OLD + '.json')),
                            '--state', str(state), '--name', NEW, '--out', str(PERSIST)], capture_output=True, text=True)
        (results / 'build-pool-esp.log').write_text(r.stdout + r.stderr)
        assert r.returncode == 0, r.stdout + r.stderr
        print(f'PASS build-pool-esp.py: {NEW} from {old_name} (rehearsal UUIDs)', flush=True)
        rehearse(results, args, OLD, OLD_SHA, old_name, NEW, ledger)
    finally:
        for suffix in ('.img', '.json'):
            (PERSIST / (NEW + suffix)).unlink(missing_ok=True)
    assert load(PERSIST / 'installed.json') == ledger
    print(f'PASS installed.json unchanged ({old_name}); the rehearsal ESP removed from {PERSIST}', flush=True)


def rehearse(results, args, OLD, OLD_SHA, old_name, NEW, ledger):
    source = WORK / 'tooling/persist/maint-session.py'
    spec = importlib.util.spec_from_file_location('maint_session', source)
    real = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(real)
    assert real.OUT == PERSIST and real.D == WORK
    saved = load(PKG / 'evidence/v12-observations.json')
    assert not saved['esp_store']['header']['self_contained']
    assert saved['esp_store']['header']['identity'] == SEED

    def run(case, target=old_name):
        out = results / case
        out.mkdir()
        (out / 'backups').mkdir()
        session = real.Session(SimpleNamespace(dry_run=False, session=out / 'session', new_backup=True,
                               esp=target, boot_image='boot-piano-macos-persistent-v5-signed.img',
                               steps=['maint-boot', 'map', 'esp-delta', 'check', 'maint-exit'],
                               store='auto', root_part='linux_rootfs', http_port=8000, nc_port=9000))
        session.env['PIANO_BACKUP_DIR'] = str(out / 'backups')
        session.state = {k: copy.deepcopy(saved[k]) for k in ('nodes', 'sizes', 'starts')}
        n = saved['nodes']
        gpt = {'head': saved['gpt_head'], 'tail': saved['gpt_tail']}
        header = saved['esp_store']['header']
        store_block = saved['esp_store']['first_lba'] - saved['starts']['linux_data']
        partition = out / 'host-replay-esp.img'
        fetched = {}
        if case == 'candidate':
            src = PERSIST / old_name
            if subprocess.run(['/bin/cp', '-c', str(src), str(partition)], capture_output=True).returncode:
                shutil.copyfile(src, partition)

        def hash_transport(dev, skip, count, bs=real.B, name='hash'):
            if dev == n['lun0'] and skip == 0 and count == 6:
                return '0' * 64 if case == 'bad-gpt' else gpt['head']
            if dev == n['lun0'] and skip == real.TAIL_LBA and count == 5:
                return gpt['tail']
            if dev == n['rootfs'] and skip == 0 and count == 1:
                return saved['root_manifest']['sha256']
            if dev == n['esp'] or (dev == n['lun0'] and (name == 'esp-check-lun0' or name == 'check-linux_rootfs-tail-esp')):
                if case == 'candidate':
                    return session.sha_nocache(partition)
                return '0' * 64 if case == 'unknown-esp' else ledger['sha256']
            for key in ('boot_a', 'boot_b', 'vendor_boot_a', 'init_boot_a'):
                if dev == n[key]:
                    return '0' * 64 if key == 'boot_a' and case == 'bad-pair' else saved[key + '_sha256']
            raise AssertionError('unhandled hash transport')

        def command_transport(cmd, *argv, **kwargs):
            if cmd == 'map':
                return copy.deepcopy(saved['map'])
            if cmd == 'write' and case == 'candidate':
                piece, dev, seek, bs = argv
                assert dev == n['esp'] and bs == real.MiB
                with partition.open('r+b') as image:
                    image.seek(seek * bs)
                    with fetched[piece].open('rb') as input_stream:
                        shutil.copyfileobj(input_stream, image, 4 << 20)
                return {}
            if cmd != 'peek':
                raise AssertionError('unhandled device command')
            node, block, offset, length = argv
            if node == saved['esp_store']['node'] and int(block) == store_block:
                data = real.common.store_header_block(header['pages'], header['identity'], header['generation'],
                                                      header['flags'], header['flattened'], header['bound_lba'])
            elif node == n['rootfs'] and int(block) == 0:
                data = bytearray(128)
                data[:8] = b'SHNGROOT'
                data[16:24] = saved['root_manifest']['image_bytes'].to_bytes(8, 'little')
            elif node == n['rootfs'] and any(
                saved[key]['state'] == 'blank' and
                int(block) == saved[key]['first_lba'] - saved['starts']['rootfs']
                for key in ('store_old', 'store_new')
            ):
                data = bytes(128)
            else:
                raise AssertionError('unhandled header read')
            return {'hex': bytes(data[int(offset):int(offset) + int(length)]).hex()}

        def shell_transport(command, timeout=120):
            if command.startswith('sgdisk -v '):
                return 'No problems found'
            if command.startswith('. /etc/piano-network'):
                return 'host_ip=REPLAY_LINK'
            if command.startswith('mkdir -p /run/pp'):
                return real.common.sha(real.TABLET_SH)
            if command in ('piano-maint idle 3600', 'piano-maint status'):
                return ''
            raise AssertionError('unhandled shell operation')

        def fastboot_transport(*argv, **kwargs):
            assert argv == ('boot', str(real.MAINT_IMAGE))

        def host_tool_transport(argv, **kwargs):
            assert argv == ['python3', str(real.MAINT_SHELL), 'sync; piano-maint fastboot', '20']
            return subprocess.CompletedProcess(argv, 0, '', '')

        def http_transport(files):
            assert all(path.is_file() for path in files.values())
            fetched.update(files)

        def local_piece(name, path, digest):
            assert path.name == name and real.common.sha(path) == digest
            fetched[name] = path

        def write_boundary(what):
            assert case == 'candidate'
            assert 'esp-delta-' in what or what.startswith('backup linux_rootfs-tail-esp -> ')
            return real.Session.write(session, what)

        def restore_transport(tag, command, piece, dev, seek, bs, port, **kwargs):
            assert case == 'candidate' and command == 'restore' and tag == 'restore-linux_rootfs-tail-esp'
            assert dev == n['lun0'] and seek == real.common.ESP_FIRST and bs == real.B
            shutil.copyfile(fetched[piece], partition)
            return {}

        def ledger_boundary(path, replacement):
            assert path == PERSIST / 'installed.json'
            assert replacement['sha256'] in (OLD_SHA, session.esp['sha256'])
            (out / (replacement['image'] + '.would-publish-ledger.json')).write_text(json.dumps(replacement) + '\n')

        session.region_sha = hash_transport
        session.t = command_transport
        session.shell = shell_transport
        session.fastboot = fastboot_transport
        session.wait = lambda *_: True
        session.wait_shell = lambda: None
        session.host_ip = lambda: 'REPLAY_LINK'
        session.serve = http_transport
        session.fetch = local_piece
        session.write = write_boundary
        session.job = restore_transport
        expected = {'bad-gpt': 'tables: unknown', 'bad-pair': 'that pair never boots', 'unknown-esp': 'the tablet\'s ESP =',
                    'bad-new-hash': 'hashes to its record', 'bad-old-hash': 'image is here and hashes'}
        if case == 'bad-new-hash':
            session.esp = dict(session.esp, sha256='0' * 64)
        if case == 'bad-old-hash':
            session.esp_record = lambda _: dict(load(PERSIST / (OLD + '.json')), sha256='0' * 64)
        with patch.object(real.subprocess, 'run', host_tool_transport), patch.object(real, 'publish_esp_ledger', ledger_boundary):
            try:
                real.maint_boot(session)
                real.map_step(session)
                real.esp_delta(session)
                real.check_step(session)
                real.maint_exit(session)
                if case == 'candidate':
                    assert session.sha_nocache(partition) == session.esp['sha256'], 'the read-back is not the pool ESP'
                    print(f'PASS candidate: esp-delta wrote {NEW}.img over the host copy of {old_name}, read back = its sha256', flush=True)
                    real.maint_boot(session)
                    session.esp = session.esp_record(old_name)
                    real.map_step(session)
                    real.esp_back(session)
                    real.map_step(session)
                    real.check_step(session)
                    real.maint_exit(session)
                    assert session.sha_nocache(partition) == OLD_SHA, 'the rollback did not give the base back'
                    print(f'PASS candidate: rollback esp gave {old_name} back ({OLD_SHA[:16]})', flush=True)
                    (partition.unlink() if partition.resolve().is_relative_to(results) else sys.exit(f'refused: {partition}'))
            except real.Stop as exc:
                assert case in expected and expected[case] in str(exc), str(exc)
            else:
                assert case in {'control', 'candidate'}, f'{case} unexpectedly completed'
        print(f'PASS execute-mode rehearsal {case}', flush=True)

    if args.candidate:
        run('candidate', NEW + '.img')
    else:
        for case in ('control', 'bad-gpt', 'bad-pair', 'unknown-esp', 'bad-new-hash', 'bad-old-hash'):
            run(case, NEW + '.img' if case == 'bad-new-hash' else old_name)


if __name__ == '__main__':
    main()
