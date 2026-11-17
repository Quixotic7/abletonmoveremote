#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# dist/moveremote-module.tar.gz: one moveremote/ folder, as Schwung Manager expects (schwung docs/MODULES.md).
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"
for f in build/move/moveremote-host build/move/moveremote-heal; do
    [[ -x "$f" ]] || { echo "ERROR: $f is missing: run make move" >&2; exit 1; }
done
rm -rf dist/moveremote
mkdir -p dist/moveremote
cp module/module.json module/standalone build/move/moveremote-host build/move/moveremote-heal LICENSE \
    THIRD_PARTY_LICENSES.md dist/moveremote/
chmod 755 dist/moveremote/{standalone,moveremote-host,moveremote-heal}
# COPYFILE_DISABLE: no macOS ._ files in the archive
COPYFILE_DISABLE=1 tar -C dist -czf dist/moveremote-module.tar.gz moveremote
ls -l dist/moveremote-module.tar.gz
