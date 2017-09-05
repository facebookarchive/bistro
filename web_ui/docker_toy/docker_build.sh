#!/bin/bash -uex
set -o pipefail
#
# See docker_toy/README.md before running this script!
#

# Assume that `docker_toy` is not a symlink, so we can `dirname` twice.
docker_toy_dir=$(dirname "$0")
web_ui_dir=$(dirname "$docker_toy_dir")

tar -C "$web_ui_dir" -czf "$docker_toy_dir/web_ui.tgz" .
cd "$docker_toy_dir"
docker build . 
