#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
'''

An fbcode_builder program to install Bistro's dependencies, and then to
build & test Bistro.

'''

from fbcode_builder.fbcode_builder import ShellQuoted


def build_bistro(builder):
    # These APIs should not change, so build a tag instead of building master.
    for p in ('facebook/zstd', 'no1msd/mstch'):
        builder.add_option(
            p + ':git_hash',
            ShellQuoted('$(git describe --abbrev=0 --tags)')
        )

    # Speeds up the build
    builder.add_option('wangle/wangle:cmake_defines', {'BUILD_TESTS': 'OFF'})

    return builder.build([
        builder.fb_github_autoconf_install('folly/folly'),
        builder.fb_github_cmake_install('wangle/wangle'),
        builder.fb_github_autoconf_install('proxygen/proxygen'),
        # Thrift and dependencies
        builder.github_project_workdir('facebook/zstd', '.'),
        builder.zstd_make_and_install(),
        builder.github_project_workdir('no1msd/mstch', '.'),
        builder.cmake_install('no1msd/mstch'),
        builder.fb_github_autoconf_install('fbthrift/thrift'),
        # Bistro
        builder.fb_github_project_workdir('bistro/bistro'),
        builder.step('Build bistro', [
            # Future: should this share some code with `cmake_install()`?
            builder.run(ShellQuoted(
                'PYTHONPATH="$PYTHONPATH:"{p}/lib/python2.7/site-packages '
                './build/run-cmake.sh Debug -DCMAKE_INSTALL_PREFIX={p}'
            ).format(p=builder.option('prefix'))),
            builder.workdir('build/Debug'),
            builder.parallel_make(),
        ]),
        builder.step('Run bistro tests', [
            builder.run(ShellQuoted('ctest')),
        ])
    ])
