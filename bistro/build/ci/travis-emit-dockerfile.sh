#!/bin/bash -ex
# .travis.yml in the top-level dir explains why this is a separate script.
bistro_git_hash=$(git rev-parse HEAD) ||
  echo "Not in a git repo? Defaulting to building bistro master."
./emit-dockerfile.py \
  --from-image "$from_image" \
  --gcc-version "$gcc_version" \
  --make-parallelism 4 \
  --substitute bistro_git_hash "$(printf %q "${bistro_git_hash:-master}")"
