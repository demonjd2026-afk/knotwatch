#!/bin/sh
# End-to-end checks of the command line: exit codes and output formats.
#   sh tests/test_cli.sh build/knotwatch
bin=$1
fix=tests/fixtures
fails=0

expect() { # expect <wanted-exit> <description> <command...>
    want=$1; what=$2; shift 2
    "$@" >/tmp/kw_cli_out.$$ 2>/tmp/kw_cli_err.$$
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL: $what: exit $got, wanted $want"
        fails=$((fails + 1))
    fi
}

expect 0  "healthy system is clean"        "$bin" analyze $fix/healthy.kws
expect 2  "deadlock exits 2"               "$bin" analyze $fix/deadlock.kws
expect 2  "blocking cycle exits 2"         "$bin" analyze $fix/pool.kws
expect 1  "mutual send exits 1"            "$bin" analyze $fix/transient.kws
expect 0  "potential cycle is a notice"    "$bin" analyze $fix/latent.kws
expect 1  "contract violation exits 1"     "$bin" analyze $fix/latent.kws --contract demo/demo.contract
expect 2  "lock held across a send"       "$bin" analyze $fix/mutex.kws
expect 0  "threshold above block time"     "$bin" analyze $fix/join.kws --min-block-ms 60000
expect 64 "no command"                     "$bin"
expect 64 "unknown command"                "$bin" frobnicate
expect 64 "unknown option"                 "$bin" analyze $fix/healthy.kws --nope 1
expect 64 "bad number"                     "$bin" analyze $fix/healthy.kws --min-block-ms 12x
expect 64 "bad format"                     "$bin" analyze $fix/healthy.kws --format xml
expect 64 "missing file argument"          "$bin" analyze
expect 66 "missing file"                   "$bin" analyze /nonexistent.kws
expect 66 "missing contract"               "$bin" analyze $fix/healthy.kws --contract /nonexistent

# stdin input
"$bin" analyze - <$fix/deadlock.kws >/dev/null 2>&1
[ $? -eq 2 ] || { echo "FAIL: stdin input"; fails=$((fails + 1)); }

# JSON must parse, when a JSON parser is available on this host.
if command -v python3 >/dev/null 2>&1; then
    for f in healthy deadlock pool transient latent join mutex abba; do
        "$bin" analyze $fix/$f.kws --format json |
            python3 -c 'import json,sys; json.load(sys.stdin)' ||
            { echo "FAIL: invalid JSON for $f"; fails=$((fails + 1)); }
    done
fi

rm -f /tmp/kw_cli_out.$$ /tmp/kw_cli_err.$$
if [ "$fails" -ne 0 ]; then
    echo "$fails CLI check(s) failed"
    exit 1
fi
echo "all CLI tests passed"
