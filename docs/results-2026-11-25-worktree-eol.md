# Worktree line endings made every documented `bash` entry point unrunnable (2026-11-25)

Host-side only; the tablet was not touched and no image was rebuilt because of this.

## Symptom

```
$ bash package/NativeUfs/tests/run-tests.sh
package/NativeUfs/tests/run-tests.sh: line 7: set: pipefail: invalid option name
```

The same happens for any of the repository's shell entry points run under WSL or Linux from this Windows
worktree, which includes the commands in `docs/bringup-debugging.md` §4 (`tooling/k273/prepare-k27-tree.sh`,
`tooling/kc/build-kc-k27.sh`, `tooling/persist/build-all.sh`, `tooling/persist/sign.sh`).

## Cause, and why `git status` hid it

68 of the 99 tracked `.sh` files under `tooling/`, `package/` and `scripts/` were CRLF **in the working tree**
while their blobs are LF, and `.gitattributes` declares `*.sh text eol=lf`. With `text` set, git cleans the
worktree bytes (CRLF to LF) before comparing, so every one of those files compares equal to the index: `git status`
reported them clean, `git checkout -- <path>` was a no-op, and nothing hinted that the file on disk differs from
the blob. The conversion also explains the earlier `M` noise: `.py` files carry no attribute here, so their CRLF
*is* a content difference to git.

## Fix and verification

For each tracked `.sh` whose worktree bytes equal the blob after EOL normalisation (68 files; **zero** had any other
local change), the blob bytes were written back to the worktree:

```
bash scripts tracked: 99   rewritten to the LF blob: 68   still CRLF: 0
$ bash package/NativeUfs/tests/run-tests.sh      # 24 PASS lines, exit 0
```

No commit results from it: the files are byte-identical to `HEAD` apart from EOL, which the `text` attribute makes
invisible to git. Re-check with `git show HEAD:<path>`, not with `git status`.

## Note for the next session

`git status` is not a way to detect this class of damage on this worktree. Anything that writes files with Windows
line endings silently breaks the build line for WSL/Linux/Mac. To check quickly:

```sh
python3 -c "import subprocess,pathlib;print([f for f in subprocess.run(['git','ls-files','--','tooling','package','scripts'],capture_output=True,text=True).stdout.split() if f.endswith('.sh') and pathlib.Path(f).exists() and b'\r\n' in pathlib.Path(f).read_bytes()])"
```