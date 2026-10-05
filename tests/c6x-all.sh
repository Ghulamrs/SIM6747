#!/bin/sh
# tests/c6x-all.sh - every test of the machine-code path and the oracle kit, against VM (default ./vm6747.exe).
set -u
D=$(dirname "$0")
VM=${VM:-./vm6747.exe}; export VM
fail=0
for t in "$D"/c6x-*.sh "$D"/oracle-*.sh; do
    case "$t" in */c6x-all.sh) continue ;; esac
    sh "$t" || fail=$((fail + 1))
done
for t in "$D"/c6x-*.py; do
    case "$t" in */c6x-words.py) continue ;; esac
    python3 "$t" "$VM" || fail=$((fail + 1))
done
[ $fail -eq 0 ] && echo "c6x-all: every test passes" || { echo "c6x-all: $fail failed"; exit 1; }
