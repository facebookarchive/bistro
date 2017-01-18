#!/usr/bin/env python2.7
'''

Converts Facebook's internal TARGETS files into CMakeLists.txt, used by
`run-cmake.sh`.

Using Python 2.7 until all environments come with 3.x out of the box.

'''
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import os
import re
import sys
import textwrap


def parse_targets(dirpath, s):
    cmake_lines = []

    #
    # Implementation details
    #

    def _dep_name(s):
        if not re.match('^(:|@/bistro/bistro[:/])', s):
            return None
        if s.endswith('-cpp2'):
            # Handled specially by run-cmake.sh, has a special CMakeLists.txt
            return 'lib_bistro_if'
        return s.split(':', 1)[1]

    def _parse_deps(kwargs):
        return [
            d for d in (
                _dep_name(s) for s in kwargs.get('deps', {})
            ) if d is not None
        ]

    def _one_per_line(l):
        return '\n    '.join(l)

    def _validate_keys(kwargs, name, valid_keys):
        for bad_key in set(kwargs.keys()).difference(valid_keys):
            raise Exception('Unknown {} key: {}'.format(name, bad_key))

    def _add_library(name, srcs):
        assert srcs
        cmake_lines.append(textwrap.dedent('''
            add_library(
                {name} STATIC
                {srcs}
            )
        ''').format(name=name, srcs=_one_per_line(srcs)))

    def _bistro_link_libraries(name, deps):
        cmake_lines.append(textwrap.dedent('''
            bistro_link_libraries(
                {name}{maybe_deps}
            )
        ''').format(
            name=name,
            maybe_deps='\n    {}'.format(_one_per_line(deps)) if deps else '',
        ))

    def _add_executable_with_libraries(name, srcs, deps):
        cmake_lines.append(textwrap.dedent('''
            add_executable(
                {name}
                {srcs}
            )
            target_link_libraries(
                {name}
                {deps}
            )
        ''').format(
            name=name, srcs=_one_per_line(srcs), deps=_one_per_line(deps)
        ))

    def _add_gtest(name, deps):
        cmake_lines.append(textwrap.dedent('''
            add_gtest(
                {name}
                {deps}
            )
        ''').format(name=name, deps=_one_per_line(deps)))

    #
    # TARGETS file constructs
    #

    def cpp_benchmark(**kwargs):
        pass  # TODO: actually build the benchmark

    def cpp_binary(name, **kwargs):
        _add_executable_with_libraries(
            name, kwargs.get('srcs', ()), _parse_deps(kwargs)
        )
        _validate_keys(kwargs, 'cpp_library', {
            'srcs',  # Handled above
            'deps',  # Handled above
            'headers',  # CMake handles headers automatically
        })

    def cpp_library(name, **kwargs):
        _add_library(name, srcs=kwargs.get('srcs', ()))
        _bistro_link_libraries(name, deps=_parse_deps(kwargs))
        _validate_keys(kwargs, 'cpp_library', {
            'srcs',  # Handled above
            'deps',  # Handled above
            'external_deps',  # Handled in build/CMakeLists.txt
            'headers',  # CMake handles headers automatically
        })

    def cpp_unittest(name, **kwargs):
        _add_gtest(name, deps=_parse_deps(kwargs))

    def python_library(**kwargs):
        pass  # I don't think that OSS platforms need to build the Python libs

    def thrift_library(**kwargs):
        pass  # Thrift is currently built by run-cmake.sh. TODO: Improve?

    class util(object):
        @classmethod
        def files(cls, pattern):
            return None  # Not currently used in Bistro?
            # Py3.5-specific return glob.glob(pattern, recursive=True)

    fn_locals = locals()
    exec s in {l: fn_locals[l] for l in (
        'cpp_benchmark',
        'cpp_binary',
        'cpp_library',
        'cpp_unittest',
        'python_library',
        'thrift_library',
        'util',
    )}

    return cmake_lines


class CmakeDir(object):
    def __init__(self, name):
        self.name = os.path.normpath(name)
        self.dirs = {}
        self.cmake_lines = []

    def write_cmake_lists(self, path=None, setup_lines=''):
        if path is None:
            path = self.name
        else:
            path = os.path.join(path, self.name)

        print(path)
        with open(os.path.join(path, 'CMakeLists.txt'), 'w') as f:
            print(setup_lines, end='', file=f)

            for l in self.cmake_lines:
                print(l, end='', file=f)

            for d in self.dirs:
                print('add_subdirectory({})'.format(d), file=f)

            # Until this supports thrift_library, add this directory manually.
            if path is self.name:
                print('add_subdirectory(if)', file=f)

        for d in self.dirs.values():
            d.write_cmake_lists(path)


def make_cmake_dir_recursive(root_path):
    TARGETS = 'TARGETS'
    root = CmakeDir(root_path)
    for dirpath, _dirnames, filenames in os.walk(root.name):
        if TARGETS in filenames:
            # run-cmake.sh has its own hacky way of building Thrift. TODO: Fix.
            if dirpath == os.path.join(root.name, 'if'):
                continue
            with open(os.path.join(dirpath, TARGETS)) as f:
                curdir = root
                assert dirpath.startswith(root.name)
                for part in dirpath[len(root.name):].split('/'):
                    if part != '':
                        curdir = curdir.dirs.setdefault(part, CmakeDir(part))
                for cmake_lines in parse_targets(dirpath, f.read()):
                    curdir.cmake_lines.append(cmake_lines)
    return root


make_cmake_dir_recursive(sys.argv[1]).write_cmake_lists(
    setup_lines='cmake_minimum_required(VERSION 2.8)\n'
        'include("build/setup.cmake")\n',
)
