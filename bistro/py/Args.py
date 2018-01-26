# Copyright (c) 2015-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import argparse

class Args(object):
    ''' Let modules add argparse flags to your parser, gflags-style. '''

    PARSERS = []
    ACTIONS = {}

    @classmethod
    def applyToParser(cls, key, func):
        if key in cls.ACTIONS:
            raise Exception('Action with key {} already applied'.format(key))
        cls.ACTIONS[key] = func
        for parser in cls.PARSERS:
            func(parser)

    @classmethod
    def enrichParser(cls, parser):
        cls.PARSERS.append(parser)
        for func in cls.ACTIONS.itervalues():  # noqa: B301 T25377293 Grandfathered in
            func(parser)
        return parser

    @classmethod
    def makeParser(cls, description):
        return cls.enrichParser(argparse.ArgumentParser(
            description=description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        ))
