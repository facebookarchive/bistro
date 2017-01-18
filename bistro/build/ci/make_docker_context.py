#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import textwrap

from build_steps import build_bistro
from fbcode_builder.make_docker_context import make_docker_context

# Try --help for more info.
print(make_docker_context(
    build_bistro,
    help=textwrap.dedent('''

    Creates a Docker context directory for building Bistro and its
    dependencies, and prints its path to stdout.

    Pass --option facebook/bistro:git_hash SHA1 to build something other
    than the master branch from Github.

    Or, pass --option facebook/bistro:local_repo_dir LOCAL_PATH to build
    from a local repo instead of cloning from Github.

    Usage:
        (cd $(./make_docker_context.py) && docker build . 2>&1 | tee log)

    '''),
))
