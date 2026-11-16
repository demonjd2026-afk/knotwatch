#!/bin/sh
# Rebuild viewer/index.html: the template with the example report embedded.
# viewer/example.json is a real report, read from /dev/knotwatch/json on
# QNX OS 8.0.0 (x86_64, QEMU) with both demo scenarios deadlocked.
#
#   sh scripts/make-viewer.sh
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
mkdir -p build
# "</" must not appear inside a script element.
sed 's#</#<\\/#g' viewer/example.json | tr -d '\n' > build/viewer-example.txt
awk -v f=build/viewer-example.txt '
    /__EXAMPLE_JSON__/ { getline json < f; sub(/__EXAMPLE_JSON__/, ""); 
                         i = index($0, "</script>"); print substr($0, 1, i - 1) json substr($0, i); next }
    { print }' viewer/template.html > viewer/index.html
echo "wrote viewer/index.html"
