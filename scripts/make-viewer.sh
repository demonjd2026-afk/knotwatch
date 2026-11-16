#!/bin/sh
# Rebuild viewer/index.html: the template with the example report embedded.
# The example is a real knotwatch report of viewer/example.kws.
#
#   make && sh scripts/make-viewer.sh
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
build/knotwatch analyze viewer/example.kws --contract demo/demo.contract \
    --format json > build/viewer-example.json || [ $? -le 2 ]
# "</" must not appear inside a script element.
sed 's#</#<\\/#g' build/viewer-example.json | tr -d '\n' > build/viewer-example.txt
awk -v f=build/viewer-example.txt '
    /__EXAMPLE_JSON__/ { getline json < f; sub(/__EXAMPLE_JSON__/, ""); 
                         i = index($0, "</script>"); print substr($0, 1, i - 1) json substr($0, i); next }
    { print }' viewer/template.html > viewer/index.html
echo "wrote viewer/index.html"
