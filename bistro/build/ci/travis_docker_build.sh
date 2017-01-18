#!/bin/bash -ex
# .travis.yml in the top-level dir explains why this is a separate script.
# Read the docs: ./make_docker_context.py --help
os_image=${os_image?Must be set by Travis}
gcc_version=${gcc_version?Must be set by Travis}
make_parallelism=${make_parallelism:-4}
docker_context_dir=$(
  "$(dirname "$0")"/make_docker_context.py \
    --os-image "$os_image" \
    --gcc-version "$gcc_version" \
    --make-parallelism "$make_parallelism" \
    --option facebook/bistro:local_repo_dir "$(git rev-parse --show-toplevel)"
)
cd "${docker_context_dir?Failed to make Docker context directory}"
docker build .
