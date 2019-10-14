#!/bin/bash -e
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

#
# This runs a simple integration test / scheduler benchmark.
#

D=$(dirname `readlink -f "$0"`)

CONFIG_FILE="$1"
FLAG=""
if [[ "$CONFIG_FILE" == "" ]] ; then
  CONFIG_FILE="$D/test_configs/simple"
  FLAG="--benchmark_run"
else
  shift
fi

# Decide if we're using CMake or the FB build system
BISTRO_BINARY="$D/../../../buck-out/gen/bistro/bistro/server/bistro_scheduler"
if [[ -x "$BISTRO_BINARY" ]] ; then
  PHABRICATOR_DOMAIN="phabricator.intern.facebook.com"
else
  PHABRICATOR_DOMAIN="[YOUR PHABRICATOR DOMAIN]"
  # Try the release binary, then default to the debug binary
  BISTRO_BINARY="$D/../cmake/Release/bistro/bistro/server/bistro_scheduler"
  if [[ ! -x "$BISTRO_BINARY" ]] ; then
    BISTRO_BINARY="$D/../cmake/Debug/bistro/bistro/server/bistro_scheduler"
  fi
fi

cat <<EOF
Test instance starting!

You can Ctrl-Z, bg, and watch the logs via:
  tail -f /tmp/bistro_log
The web UI is here:
  https://$PHABRICATOR_DOMAIN/bistro/jobs?fetcher=monitor2_http&hostport_source=hp&hostport_data=$HOSTNAME%3A8080
EOF

echo
set -x
"$BISTRO_BINARY" \
  --config_file="$CONFIG_FILE" \
  $FLAG \
  --monitor_update_ms=1000 \
  --config_update_ms=2000 \
  --nodes_update_ms=5000 \
  --nodes_retry_ms=5000 \
  "$@" \
> /tmp/bistro_log 2>&1
