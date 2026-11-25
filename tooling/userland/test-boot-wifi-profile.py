#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Seed-only private fixture safety; no image mount or device."""
import importlib.util
import os
from pathlib import Path
import tempfile
spec=importlib.util.spec_from_file_location('stage',Path(__file__).with_name('stage.py'))
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
with tempfile.TemporaryDirectory() as d:
    path=Path(d)/'profile';path.write_bytes(b'ssid=fixture\nsecurity=open\n');path.chmod(0o600)
    os.environ['PIANO_BOOT_WIFI_PROFILE']=str(path)
    tree={};command=stage.boot_wifi_profile(tree,'sh /private/var/piano/userland/install.sh --boot')
    assert tree['boot-wifi-profile']==(path.read_bytes(),0o600)
    assert command.index('chmod 600')<command.index('install.sh --boot')
    assert 'fixture' not in command
    path.chmod(0o644)
    try: stage.boot_wifi_profile({},'sh /private/var/piano/userland/install.sh --boot')
    except SystemExit: pass
    else: raise AssertionError('public profile accepted')
    path.chmod(0o600);link=path.with_name('link');link.symlink_to(path)
    os.environ['PIANO_BOOT_WIFI_PROFILE']=str(link)
    try: stage.boot_wifi_profile({},'sh /private/var/piano/userland/install.sh --boot')
    except SystemExit: pass
    else: raise AssertionError('symlink accepted')
    del os.environ['PIANO_BOOT_WIFI_PROFILE']
    tree={};assert stage.boot_wifi_profile(tree,'unchanged')=='unchanged' and not tree
print('PASS private seed-only profile: safe fixture, startup ordering, public/symlink rejection, absent no-op')
