#!/usr/bin/env python3

'fbcode_builder steps to build & test Bistro'

import specs.fbthrift as fbthrift
import specs.fmt as fmt
import specs.folly as folly
import specs.proxygen as proxygen

from shell_quoting import ShellQuoted

_NUM_TEST_RETRIES = 5


# Since Bistro doesn't presently have an "install" target, there is no
# point in having its spec in the shared spec directory.
def fbcode_builder_spec(builder):
    return {
        'depends_on': [fmt, folly, proxygen, fbthrift],
        'steps': [
            builder.fb_github_project_workdir('bistro/bistro'),
            builder.step('Build bistro', [
                # Future: should this share some code with `cmake_install()`?
                builder.run(ShellQuoted(
                    'PATH="$PATH:"{p}/bin '
                    'TEMPLATES_PATH={p}/include/thrift/templates '
                    './cmake/run-cmake.sh Debug -DCMAKE_INSTALL_PREFIX={p}'
                ).format(p=builder.option('prefix'))),
                builder.workdir('cmake/Debug'),
                builder.parallel_make(),
            ]),
            builder.step('Run bistro tests', [
                # Internally flaky tests have automation to help flag them,
                # so the vast majority of Bistro tests should be pretty
                # good.  Tests flaky on open-source builds can easily go
                # unnoticed and cause noise, so let's just retry liberally.
                builder.run(ShellQuoted(
                    'ctest --output-on-failure || ' + ' || '.join(
                        ['ctest --rerun-failed --output-on-failure']
                            * (_NUM_TEST_RETRIES - 1)
                    ),
                )),
            ]),
        ]
    }


config = {
    'github_project': 'facebook/bistro',
    'fbcode_builder_spec': fbcode_builder_spec,
}
