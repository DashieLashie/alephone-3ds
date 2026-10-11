#!/usr/bin/env bash
# Runs inside aleph-host-baseline with a build volume at /build (see build.sh).
# usage: ppcheck-in-container.sh "<changed files>" "<TUs>"  (repo-relative paths)
# /head and /work mirror the repo paths with the HEAD and working-tree versions
# of the changed files. For each TU: automake compile line (make -n) -> g++ -E -P
# with both versions from the same path, cmp, then -fsyntax-only on the working
# version. The volume's own copies are restored at the end.
set -uo pipefail
cd /build/src
CHANGED=($1); TUS=($2)
swap() { for f in "${CHANGED[@]}"; do cp "$1/$f" "$f"; done; }
mkdir -p /tmp/orig; for f in "${CHANGED[@]}"; do mkdir -p /tmp/orig/$(dirname $f); cp $f /tmp/orig/$f; done
for tu in "${TUS[@]}"; do
  dir=$(dirname $tu); base=$(basename $tu .cpp)
  cmd=$(cd $dir && make -n -o Makefile -o Makefile.in -W $(basename $tu) $base.o 2>/dev/null | grep -E '(^| )(g\+\+|c\+\+)' | grep -- "-c" | head -1)
  [ -n "$cmd" ] || { echo "$tu: no compile line"; continue; }
  pp=$(echo "$cmd" | sed -E 's/-MT [^ ]+//; s/-MD//; s/-MP//; s/-MF [^ ]+//; s/-o [^ ]+//; s/ -c / -E -P /; s/[ ]*&&[ ]*\\?$//')
  for v in head work; do swap /$v; (cd $dir && eval "$pp") > /tmp/$base.$v.i 2>/tmp/$base.$v.err; done
  if cmp -s /tmp/$base.head.i /tmp/$base.work.i; then r=IDENTICAL; else r="DIFFER: $(diff /tmp/$base.head.i /tmp/$base.work.i | grep -c '^[<>]') lines"; fi
  swap /work; sx=$(echo "$pp" | sed 's/ -E -P / -fsyntax-only /'); (cd $dir && eval "$sx") > /tmp/sx 2>&1; s=$?
  echo "$tu: $r ($(wc -l < /tmp/$base.work.i) lines, stderr $(cat /tmp/$base.*.err | wc -l)); -fsyntax-only exit $s, $(wc -l < /tmp/sx) diagnostics"
done
swap /tmp/orig
