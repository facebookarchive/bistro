#!/bin/bash -ex
# .travis.yml in the top-level dir explains why this is a separate script.
# Read the docs: ./make_docker_context.py --help
os_image=${os_image?Must be set by Travis}
gcc_version=${gcc_version?Must be set by Travis}
make_parallelism=${make_parallelism:-4}
cur_dir="$(readlink -f "$(dirname "$0")")"
docker_context_dir=$(
  # The PYTHONPATH will go away in phase 2 of simplifying fbcode_builder usage
  PYTHONPATH="$cur_dir:$PYTHONPATH" \
  "$cur_dir/../bistro/build/ci/make_docker_context.py" \
    --os-image "$os_image" \
    --gcc-version "$gcc_version" \
    --make-parallelism "$make_parallelism" \
    --option facebook/bistro:local_repo_dir "$cur_dir/.."
)
cd "${docker_context_dir?Failed to make Docker context directory}"
docker build .
