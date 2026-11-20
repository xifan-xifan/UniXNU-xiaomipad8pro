# The k27 tree: the local XNU 27.0.1 kernel tree (xnu-12377 + the baseline series 0001-0052) that patches/k27 applies to.
# The native build reads it and never writes there: the kernel and its objects' compile recipes (out/k27, out/x12377/build), the
# generated headers (out/x12377: storage ABI, xnu, IOKit hid/net/gfx), the KC base (out/kc/kc27.bin, Apple's payload, never
# published) and the KC builder (k27/, scripts/, configs/kc-keep.txt). docs/bringup-debugging.md §4 has how it is built.
#
# Its place is PIANO_K27_TREE, from the environment or else from ~/.config/piano/env (a line PIANO_K27_TREE=/path); no
# committed file names it. Sourced by the build scripts (not by an interactive shell: it exits the script when the tree is unknown):
#
#   . "$D/tooling/k27/k27-tree.sh"        sets and exports PIANO_K27_TREE, or stops with what to set
#
# tooling/k27/k27_tree.py is the same for Python.
if [ -z "${PIANO_K27_TREE:-}" ] && [ -f "$HOME/.config/piano/env" ]; then
  # Only this one line is read: sourcing the file would also set its other variables over the caller's.
  PIANO_K27_TREE=$(sed -n 's/^[[:space:]]*\(export[[:space:]][[:space:]]*\)\{0,1\}PIANO_K27_TREE=//p' "$HOME/.config/piano/env" | tail -1)
  PIANO_K27_TREE=${PIANO_K27_TREE%\"}; PIANO_K27_TREE=${PIANO_K27_TREE#\"}
  PIANO_K27_TREE=${PIANO_K27_TREE%\'}; PIANO_K27_TREE=${PIANO_K27_TREE#\'}
  case $PIANO_K27_TREE in "~/"*) PIANO_K27_TREE=$HOME/${PIANO_K27_TREE#\~/} ;; "\$HOME/"*) PIANO_K27_TREE=$HOME/${PIANO_K27_TREE#\$HOME/} ;; esac
fi
if [ -z "${PIANO_K27_TREE:-}" ]; then
  echo "PIANO_K27_TREE is not set: the k27 tree (xnu-12377 + the baseline series 0001-0052, XNU 27.0.1) the native build reads." >&2
  echo "Set it in the environment or as PIANO_K27_TREE=/path in ~/.config/piano/env (docs/bringup-debugging.md §4)." >&2
  exit 1
fi
K27_LOCKED_COMMIT=11ac316b789d81fc9f734e15645456f536c05141
if [ -d "$PIANO_K27_TREE/.git" ]; then
  K27_HEAD=$(git -C "$PIANO_K27_TREE" rev-parse --verify HEAD 2>/dev/null || true)
  if [ "$K27_HEAD" != "$K27_LOCKED_COMMIT" ]; then
    echo "PIANO_K27_TREE=$PIANO_K27_TREE is at ${K27_HEAD:-no commit}; locked native k27 source is $K27_LOCKED_COMMIT" >&2
    exit 1
  fi
else
  echo "PIANO_K27_TREE=$PIANO_K27_TREE has no Git metadata; refusing an untraceable native k27 source" >&2
  exit 1
fi
if [ ! -d "$PIANO_K27_TREE" ]; then
  echo "PIANO_K27_TREE=$PIANO_K27_TREE is not a directory (the k27 tree, docs/bringup-debugging.md §4)" >&2
  exit 1
fi
export PIANO_K27_TREE
