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
                   patches/k27-tree on top (tooling/k27/fetch-k27-tree.sh), checked out as k27-tree.
Commits are fetched by their SHA-1 with depth 1 (GitHub serves any reachable commit that way), so a
pin costs one commit's tree, not the history.

Written by SorasakiHinaHusband (pull request #2); --verify, --outdated, fetching by commit and the k27
tree added for the repository's CI (docs/ci.md).
"""

import argparse
import os
import subprocess
import sys
import tempfile
import tomllib

RENAMES = {
    "Mu-Silicium": "Mu-Silicium-piano",
    "piano-uefi": "piano-uefi-UniXNU-piano",
    "k27": "k27-tree",
}
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def sh(cmd, cwd=None, quiet=False):
    if not quiet:
        print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def out(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True).stdout.strip()


def fetch_commit(dest, url, commit, blobs=True):
    """dest's HEAD at commit: a shallow fetch of exactly that commit (a full fetch when the server refuses one)."""
    os.makedirs(dest, exist_ok=True)
    if not os.path.isdir(os.path.join(dest, ".git")):
        sh(["git", "init", "-q", dest])
        sh(["git", "-C", dest, "remote", "add", "origin", url])
    else:
        sh(["git", "-C", dest, "remote", "set-url", "origin", url])
    extra = [] if blobs else ["--filter=blob:none"]
    try:
        sh(["git", "-C", dest, "fetch", "-q", "--depth", "1", *extra, "origin", commit])
    except subprocess.CalledProcessError:
        sh(["git", "-C", dest, "fetch", "-q", *extra, "origin"])
    if blobs:
        sh(["git", "-C", dest, "checkout", "-q", "--detach", commit])
    else:
        sh(["git", "-C", dest, "cat-file", "-e", commit + "^{commit}"], quiet=True)


def submodule_url(dest, sub):
    url = out(["git", "-C", dest, "config", "-f", ".gitmodules", "--get", f"submodule.{sub}.url"])
    if not url:
        # .gitmodules names the submodule by name, not path
        for line in out(["git", "-C", dest, "config", "-f", ".gitmodules", "--get-regexp", r"submodule\..*\.path"]).splitlines():
            key, path = line.split(" ", 1)
            if path == sub:
                url = out(["git", "-C", dest, "config", "-f", ".gitmodules", "--get", key[:-len(".path")] + ".url"])
    if url.startswith("../"):
        base = out(["git", "-C", dest, "remote", "get-url", "origin"]).rstrip("/")
        url = base.rsplit("/", 1)[0] + "/" + url[3:]
    return url


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verify", action="store_true", help="fetch each pinned commit without a checkout (blobs left out)")
    ap.add_argument("--outdated", action="store_true", help="report pins behind their upstream branch")
    ap.add_argument("--lock", default=os.path.join(REPO, "unixnu.lock"))
    args = ap.parse_args()

    root = os.environ.get("UNIXNU_RESEARCH", os.path.join(os.getcwd(), "unixnu-research"))
    with open(args.lock, "rb") as f:
        lock = tomllib.load(f)
    pins = lock.get("pin", {})
    only = set(args.only.split(",")) if args.only else None
    failed, moved = [], []

    for name, spec in pins.items():
        if only and name not in only:
            continue
        if "git" not in spec:
            print(f"skip {name}: {spec.get('role', '').splitlines()[0][:60] if spec.get('role') else ''} (no git: local tree)")
            continue
        url, commit = spec["git"], spec["commit"]
        if args.outdated:
            ref = spec.get("branch") or "HEAD"
            line = out(["git", "ls-remote", url, ref if ref == "HEAD" else f"refs/heads/{ref}"])
            head = line.split()[0] if line else ""
            if not head:
                failed.append(f"{name}: {ref} not found at {url}")
            elif head != commit:
                moved.append(f"{name}: pinned {commit[:10]}, {ref} is now {head[:10]} ({url})")
            else:
                print(f"ok   {name} @ {commit[:10]} = {ref}")
            continue
        if args.verify:
            with tempfile.TemporaryDirectory() as tmp:
                try:
                    fetch_commit(tmp, url, commit, blobs=False)
                    for sub, sub_commit in (spec.get("submodules") or {}).items():
                        # the submodule's URL comes from .gitmodules of the pinned commit (its blob fetched on demand)
                        gm = out(["git", "-C", tmp, "show", f"{commit}:.gitmodules"])
                        with open(os.path.join(tmp, ".gm"), "w") as g:
                            g.write(gm)
                        key = next((l.split(" ", 1)[0][:-len(".path")] for l in
                                    out(["git", "config", "-f", os.path.join(tmp, ".gm"), "--get-regexp", r"submodule\..*\.path"]).splitlines()
                                    if l.split(" ", 1)[1] == sub), None)
                        sub_url = out(["git", "config", "-f", os.path.join(tmp, ".gm"), "--get", key + ".url"]) if key else ""
                        if sub_url.startswith("../"):
                            sub_url = url.rstrip("/").rsplit("/", 1)[0] + "/" + sub_url[3:]
                        if not sub_url:
                            raise RuntimeError(f"submodule {sub} not in .gitmodules")
                        with tempfile.TemporaryDirectory() as subtmp:
                            fetch_commit(subtmp, sub_url, sub_commit, blobs=False)
                    print(f"ok   {name} @ {commit[:10]}")
                except (subprocess.CalledProcessError, RuntimeError) as e:
                    failed.append(f"{name} @ {commit[:10]}: {e}")
            continue

        dest = os.path.join(root, RENAMES.get(name, name))
        if name == "k27":
            if args.dry_run:
                print(f"would fetch the k27 tree -> {dest} @ {commit[:10]} + patches/k27-tree")
                continue
            sh(["bash", os.path.join(REPO, "tooling/k27/fetch-k27-tree.sh"), dest])
            continue
        if os.path.isdir(os.path.join(dest, ".git")):
            head = out(["git", "-C", dest, "rev-parse", "HEAD"])
            if head == commit:
                print(f"ok   {name} @ {head[:10]}")
                continue
            print(f"stale {name}: {head[:10]} != {commit[:10]}")
        if args.dry_run:
            print(f"would fetch {name} -> {dest} @ {commit[:10]}")
            continue
        fetch_commit(dest, url, commit)
        for sub, sub_commit in (spec.get("submodules") or {}).items():
            sub_dest = os.path.join(dest, sub)
            sub_url = submodule_url(dest, sub)
            sh(["git", "-C", dest, "submodule", "init", "--", sub], quiet=True)
            fetch_commit(sub_dest, sub_url, sub_commit)

    for m in moved:
        print("moved", m)
    for f in failed:
        print("FAIL ", f)
    if args.outdated and os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as s:
            s.write("## Pins behind upstream\n\n" + ("\n".join(f"- {m}" for m in moved) or "none") + "\n")
            if failed:
                s.write("\n## Pins whose branch is gone\n\n" + "\n".join(f"- {f}" for f in failed) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
