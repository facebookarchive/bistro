#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
'fbcode_builder steps to build & test Bistro'

import specs.fbthrift as fbthrift
import specs.folly as folly
import specs.proxygen as proxygen

from shell_quoting import ShellQuoted


# Since Bistro doesn't presently have an "install" target, there is no
# point in having its spec in the shared spec directory.
def fbcode_builder_spec(builder):
    return {
        'depends_on': [folly, proxygen, fbthrift],
        'steps': [
            builder.fb_github_project_workdir('bistro/bistro'),
            builder.step('Build bistro', [
                # Future: should this share some code with `cmake_install()`?
                builder.run(ShellQuoted(
                    'PATH="$PATH:{p}/bin" '
                    'TEMPLATES_PATH="{p}/include/thrift/templates" '
                    './cmake/run-cmake.sh Debug -DCMAKE_INSTALL_PREFIX={p}'
                ).format(p=builder.option('prefix'))),
                builder.workdir('cmake/Debug'),
                builder.parallel_make(),
            ]),
            builder.step('Run bistro tests', [
                builder.run(ShellQuoted('ctest')),
            ]),
        ]
    }


config = {
    'github_project': 'facebook/bistro',
    'fbcode_builder_spec': fbcode_builder_spec,
}
