#!/bin/bash -uex
set -o pipefail
#
# This is normally started via `docker_toy/README.md`. You can also manually
# run it to integrate this `bistro/web_ui/` code checkout into your current
# Phabricator install.  CAVEAT: This code was only tested with the specific
# `github.com/phacility/*` repo hashes in `docker_toy/Dockerfile`.
#

# Make both paths absolute for robustness. However, the bistro UI path may
# include symlink, which we want to preserve, so we avoid `readlink`.
#
# To ensure we exit on error, we do not nest ${} or $() inside $().
web_ui_dir=$(dirname "$0")
web_ui_dir=$(cd "$web_ui_dir" && pwd)
phab_root=${1?First arg -- directory with arcanist/, phabricator/, etc}
phab_root=$(readlink -f "$phab_root")

# Runs $1, which is presumed to be a Phabricator PHP binary with the
# annoying property that it exits with code 0 on some errors.
#
# Only allow $1 to write to stderr, leaving stdout free for plumbing.
wrap_php_errors() {
  exception_bytes=$(
    set -o pipefail
    "$1" 2>&1 | tee /dev/fd/2 | (grep -i exception || :) | wc -c
  )
  [[ "$exception_bytes" == "0" ]]
}

arc_liberate() {
  (
    cd "$web_ui_dir/src"
    "$phab_root/arcanist/bin/arc" liberate --library-name bistro 2>&1
  )
}

set_load_libraries() {
  load_libs_json=$(  # Not inlined so that python errors exit this script.
    python3 -c '
import json, sys
print(json.dumps({"bistro": sys.argv[1]}))
' "$web_ui_dir/src"
  )
  "$phab_root/phabricator/bin/config" set load-libraries "$load_libs_json"
}

celerity_map() {
  # Unfortunately, we cannot just extend CelerityResourcesOnDisk because JS
  # resources from the `bistro` module would not be able to access JS
  # resources like `javelin-dom` from the `phabricator` module.  As of
  # 8/2017, Celerity does not seem to support cross-module resolution.
  for lang in css js ; do 
    ln -snf "$web_ui_dir/rsrc/$lang/application/bistro" \
      "$phab_root/phabricator/webroot/rsrc/$lang/application/bistro"
  done
  "$phab_root/phabricator/bin/celerity" map
}

wrap_php_errors arc_liberate
wrap_php_errors set_load_libraries
wrap_php_errors celerity_map
