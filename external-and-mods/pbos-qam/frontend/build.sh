#!/usr/bin/env bash
# Build dist/pbos-qam.js (run on the build Mac: needs node and npm). The
# result is committed, as the Decky plugins' dist/index.js were: image
# builds do not need node.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
npm ci --no-audit --no-fund 2>/dev/null || npm install --no-audit --no-fund
npm run build
