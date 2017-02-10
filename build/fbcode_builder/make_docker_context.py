#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
'''

Helper for making a command-line utility that writes your project's
Dockerfile and associated data into a (temporary) directory.  Your main
program will look something like this:

    print(make_docker_context(
        lambda builder: [builder.step(...), ...],
        help='Creates a Docker context and writes its path to stdout.',
    ))

'''

import argparse
import logging
import os
import tempfile

from fbcode_builder.docker_builder import DockerFBCodeBuilder
from .parse_args import parse_args_to_fbcode_builder_opts
from fbcode_builder.shell_quoting import raw_shell, ShellQuoted


def make_docker_context(
    get_steps_fn, opts=None, help='', default_context_dir=None
):
    'Returns a path to the Docker context directory. See parse_args.py.'

    if opts is None:
        opts = {}

    valid_versions = (
        ('ubuntu:14.04', '4.9'), ('ubuntu:16.04', '5'), ('debian:8.6', '4.9')
    )

    def add_args(parser):
        parser.add_argument(
            '--docker-context-dir', metavar='DIR',
            default=default_context_dir,
            help='Write the Dockerfile and its context into this directory. '
                'If empty, make a temporary directory. Default: %(default)s.',
        )
        parser.add_argument(
            '--user', metavar='NAME', default=opts.get('user', 'nobody'),
            help='Build and install as this user. Default: %(default)s.',
        )
        parser.add_argument(
            '--prefix', metavar='DIR',
            default=opts.get('prefix', '/home/install'),
            help='Install all libraries in this prefix. Default: %(default)s.',
        )
        parser.add_argument(
            '--projects-dir', metavar='DIR',
            default=opts.get('projects_dir', '/home'),
            help='Place project code directories here. Default: %(default)s.',
        )
        parser.add_argument(
            '--os-image', metavar='IMG', choices=zip(*valid_versions)[0],
            default=opts.get('os_image', 'debian:8.6'),
            help='Docker OS image -- be sure to use only ones you trust (See '
                'README.docker). Choices: %(choices)s. Default: %(default)s.',
        )
        parser.add_argument(
            '--gcc-version', metavar='VER',
            choices=set(zip(*valid_versions)[1]),
            default=opts.get('gcc_version', '4.9'),
            help='Choices: %(choices)s. Default: %(default)s.',
        )
        parser.add_argument(
            '--make-parallelism', metavar='NUM', type=int,
            default=opts.get('make_parallelism', 1),
            help='Use `make -j` on multi-CPU systems with lots of RAM. '
                'Default: %(default)s.',
        )

    opts = parse_args_to_fbcode_builder_opts(
        add_args,
        # These have add_argument() calls, others are set via --option.
        (
            'docker_context_dir',
            'user',
            'prefix',
            'projects_dir',
            'os_image',
            'gcc_version',
            'make_parallelism',
        ),
        opts,
        help
    )

    if (opts.get('os_image'), opts.get('gcc_version')) not in valid_versions:
        raise Exception(
            'Due to 4/5 ABI changes (std::string), we can only use {0}'.format(
                ' / '.join('GCC {1} on {0}'.format(*p) for p in valid_versions)
            )
        )

    if opts.get('docker_context_dir') is None:
        opts['docker_context_dir'] = tempfile.mkdtemp(prefix='docker-context-')
    elif not os.path.exists(opts.get('docker_context_dir')):
        os.makedirs(opts.get('docker_context_dir'))

    builder = DockerFBCodeBuilder(**opts)
    context_dir = builder.option('docker_context_dir')  # Mark option "in-use"
    # The renderer may also populate some files into the context_dir.
    dockerfile = builder.render(get_steps_fn(builder))

    with os.fdopen(os.open(
        os.path.join(context_dir, 'Dockerfile'),
        os.O_RDWR | os.O_CREAT | os.O_EXCL,  # Do not overwrite existing files
        0o644,
    ), 'w') as f:
        f.write(dockerfile)

    return context_dir
