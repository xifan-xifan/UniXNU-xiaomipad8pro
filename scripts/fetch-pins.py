#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fetch the checkouts UniXNU-piano pins, into $UNIXNU_RESEARCH (default ./unixnu-research).

Reads unixnu.lock's [pin."name"] tables — the same list tooling/unixnu.sh expects under
~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research — and checks each one out at its commit.

    python3 scripts/fetch-pins.py [--only NAME[,NAME]] [--dry-run]
    python3 scripts/fetch-pins.py --verify [--only ...]     # every pinned commit (and submodule commit) still exists upstream
    python3 scripts/fetch-pins.py --outdated [--only ...]   # pins whose upstream branch has moved on (report only)

Why this exists: every build path in the repository assumes those checkouts already exist locally
(the author's Mac). A CI runner starts empty, so the pins have to be materialised from the lockfile.
Nothing here is a fork: the deltas stay in the device repository (firmware/patches, patches/, dts/).

Special cases, matching tooling/unixnu.sh:
  Mu-Silicium   -> checked out as Mu-Silicium-piano (the checkout name the script and piano-uefi's
                   setup-deps.sh expect); its submodules are pinned in the lockfile too.
  piano-uefi    -> checked out as piano-uefi-UniXNU-piano, branch "unixnu" (the lockfile records it).
  k27           -> the k27 tree: upstream Droid-VM/macOS-DroidVM at the pinned commit with this repository's
                   patches/k27-tree on top, checked out as k27-tree by the helper in tooling/k27/.
Commits are fetched by their SHA-1 with depth 1 (GitHub serves any reachable commit that way), so a
pin costs one commit's tree, not the history.

Written by SorasakiHinaHusband (pull request #2); --verify, --outdated, fetching by commit and the k27
tree added for the repository's CI (docs/ci.md).
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile

from repo_manifest import load_manifest

RENAMES = {
    "Mu-Silicium": "Mu-Silicium-piano",
    "piano-uefi": "piano-uefi-UniXNU-piano",
    "k27": "k27-tree",
}
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SHA = re.compile(r"[0-9a-f]{40}")
CONTROL = re.compile(r"[\x00-\x1f\x7f]")


def sh(cmd, cwd=None, quiet=False):
    if not quiet:
        print("+", shlex.join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def out(cmd, *, check=True):
    return subprocess.run(cmd, capture_output=True, text=True, check=check).stdout.rstrip("\n")


def valid_path(path):
    return (not CONTROL.search(path) and "\\" not in path
            and all(part.casefold() not in ("", ".", "..", ".git") for part in path.split("/")))


def read_pins(lock):
    pins = load_manifest(lock).get("pin")
    if not isinstance(pins, dict) or not pins:
        raise ValueError("expected [pin.NAME] tables")
    for name, spec in pins.items():
        label = f"pin {name!r}"
        if "/" in name or not valid_path(name):
            raise ValueError(f"{label}: invalid pin name")
        if not isinstance(spec, dict):
            raise ValueError(f"{label}: expected a table")
        for field in ("git", "role", "branch", "patches"):
            if field in spec and not isinstance(spec[field], str):
                raise ValueError(f"{label}.{field}: expected a string")
        if "git" not in spec:
            continue
        url = spec["git"]
        if not url.strip() or url.startswith("-") or CONTROL.search(url):
            raise ValueError(f"{label}.git: expected a repository URL or path")
        branch = spec.get("branch")
        if branch and (not branch.strip() or CONTROL.search(branch)):
            raise ValueError(f"{label}.branch: invalid branch name")
        if not isinstance(spec.get("commit"), str) or not SHA.fullmatch(spec["commit"]):
            raise ValueError(f"{label}.commit: expected a 40-character lowercase hexadecimal SHA")
        submodules = spec.get("submodules", {})
        if not isinstance(submodules, dict):
            raise ValueError(f"{label}.submodules: expected a table of paths and commits")
        for path, commit in submodules.items():
            if not valid_path(path):
                raise ValueError(f"{label}: invalid submodule path {path!r}")
            if not isinstance(commit, str) or not SHA.fullmatch(commit):
                raise ValueError(f"{label} submodule {path!r}: expected a 40-character lowercase hexadecimal SHA")
        if name == "k27":
            if not spec.get("patches") or not valid_path(spec["patches"]):
                raise ValueError(f"{label}.patches: expected a repository-relative path")
            if not isinstance(spec.get("tree"), str) or not SHA.fullmatch(spec["tree"]):
                raise ValueError(f"{label}.tree: expected a 40-character lowercase hexadecimal SHA")
    return pins


def checkout_head(dest):
    if os.path.exists(os.path.join(dest, ".git")):
        return out(["git", "-C", dest, "rev-parse", "--verify", "--quiet", "HEAD"], check=False)
    return ""


def fetch_commit(dest, url, commit, blobs=True):
    """dest's HEAD at commit: a shallow fetch of exactly that commit (a full fetch when the server refuses one)."""
    os.makedirs(dest, exist_ok=True)
    if not os.path.exists(os.path.join(dest, ".git")):
        sh(["git", "init", "-q", dest])
    if "origin" not in out(["git", "-C", dest, "remote"]).splitlines():
        sh(["git", "-C", dest, "remote", "add", "origin", url])
    else:
        sh(["git", "-C", dest, "remote", "set-url", "origin", url])
    extra = [] if blobs else ["--filter=blob:none"]
    try:
        sh(["git", "-C", dest, "fetch", "-q", "--depth", "1", *extra, "origin", commit])
    except subprocess.CalledProcessError:
        shallow = out(["git", "-C", dest, "rev-parse", "--is-shallow-repository"]) == "true"
        sh(["git", "-C", dest, "fetch", "-q", *(["--unshallow"] if shallow else []), *extra, "origin"])
    if blobs:
        sh(["git", "-C", dest, "checkout", "-q", "--detach", commit])
    else:
        sh(["git", "-C", dest, "cat-file", "-e", commit + "^{commit}"], quiet=True)


def submodule_url(dest, sub, commit=None):
    git = ["git", "-C", dest]
    source = ["--blob", f"{commit}:.gitmodules"] if commit else ["-f", ".gitmodules"]
    entries = out([*git, "config", *source, "--null", "--get-regexp", r"^submodule\..*\.path$"], check=False)
    for entry in entries.split("\0"):
        key, _, path = entry.partition("\n")
        if path == sub:
            # Git knows how to resolve relative URLs for every supported remote transport.
            sh([*git, "submodule", "init", "--", sub], quiet=True)
            url = out([*git, "config", "--get", key.removesuffix(".path") + ".url"], check=False)
            if url:
                return url
            break
    raise RuntimeError(f"no URL for submodule {sub!r} in .gitmodules")


def verify_pin(url, commit, submodules):
    with tempfile.TemporaryDirectory() as tmp:
        fetch_commit(tmp, url, commit, blobs=False)
        if submodules:
            # Populate the index for submodule init without checking out files.
            sh(["git", "-C", tmp, "read-tree", commit], quiet=True)
        for sub, sub_commit in submodules.items():
            sub_url = submodule_url(tmp, sub, commit)
            with tempfile.TemporaryDirectory() as subtmp:
                fetch_commit(subtmp, sub_url, sub_commit, blobs=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--verify", action="store_true", help="fetch each pinned commit without a checkout (blobs left out)")
    mode.add_argument("--outdated", action="store_true", help="report pins behind their upstream branch")
    ap.add_argument("--lock", default=os.path.join(REPO, "unixnu.lock"))
    args = ap.parse_args()
    root = os.environ.get("UNIXNU_RESEARCH", os.path.join(os.getcwd(), "unixnu-research"))
    try:
        pins = read_pins(args.lock)
    except (OSError, ValueError) as e:
        ap.error(f"invalid lockfile: {e}")
    only = {name.strip() for name in args.only.split(",")} if args.only is not None else None
    if only is not None and (unknown := only - pins.keys()):
        ap.error("unknown pin(s): " + ", ".join(repr(name) for name in sorted(unknown)))
    failed, moved = [], []

    for name, spec in pins.items():
        if only and name not in only:
            continue
        if "git" not in spec:
            print(f"skip {name}: {spec.get('role', '').splitlines()[0][:60] if spec.get('role') else ''} (no git: local tree)")
            continue
        url, commit = spec["git"], spec["commit"]
        submodules = spec.get("submodules", {})
        if args.outdated:
            ref = spec.get("branch") or "HEAD"
            full_ref = ref if ref == "HEAD" else f"refs/heads/{ref}"
            query = subprocess.run(["git", "ls-remote", "--exit-code", "--", url, full_ref],
                                   capture_output=True, text=True)
            head = next((line.partition("\t")[0] for line in query.stdout.splitlines()
                         if line.partition("\t")[2] == full_ref), "")
            if query.returncode not in (0, 2):
                failed.append(f"{name}: could not query {ref} at {url} (git exited {query.returncode})")
                if query.stderr:
                    print(query.stderr.rstrip(), file=sys.stderr)
            elif not head:
                failed.append(f"{name}: {ref} not found at {url}")
            elif head != commit:
                moved.append(f"{name}: pinned {commit[:10]}, {ref} is now {head[:10]} ({url})")
            else:
                print(f"ok   {name} @ {commit[:10]} = {ref}")
            continue
        if args.verify:
            try:
                verify_pin(url, commit, submodules)
                print(f"ok   {name} @ {commit[:10]}")
            except (subprocess.CalledProcessError, RuntimeError) as e:
                failed.append(f"{name} @ {commit[:10]}: {e}")
            continue

        dest = os.path.join(root, RENAMES.get(name, name))
        if name == "k27":
            if args.dry_run:
                print(f"would fetch the k27 tree -> {dest} @ {commit[:10]} + {spec['patches']}")
                continue
            sh(["bash", os.path.join(REPO, "tooling/k27/fetch-k27-tree.sh"), dest, args.lock])
            continue
        head = checkout_head(dest)
        if head == commit:
            print(f"ok   {name} @ {head[:10]}")
        elif head:
            print(f"stale {name}: {head[:10]} != {commit[:10]}")
        if args.dry_run and head != commit:
            print(f"would fetch {name} -> {dest} @ {commit[:10]}")
        elif head != commit:
            fetch_commit(dest, url, commit)
        for sub, sub_commit in submodules.items():
            sub_dest = os.path.join(dest, sub)
            if checkout_head(sub_dest) == sub_commit:
                continue
            if args.dry_run:
                print(f"would fetch {name}/{sub} -> {sub_dest} @ {sub_commit[:10]}")
                continue
            try:
                sub_url = submodule_url(dest, sub)
            except (subprocess.CalledProcessError, RuntimeError) as e:
                sys.exit(f"{name}: {e}")
            fetch_commit(sub_dest, sub_url, sub_commit)

    for m in moved:
        print("moved", m)
    for f in failed:
        print("FAIL ", f)
    if args.outdated and os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as s:
            s.write("## Pins behind upstream\n\n" + ("\n".join(f"- {m}" for m in moved) or "none") + "\n")
            if failed:
                s.write("\n## Pins whose upstream could not be checked\n\n" + "\n".join(f"- {f}" for f in failed) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
