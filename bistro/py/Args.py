# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.






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
        for func in cls.ACTIONS.values():
            func(parser)
        return parser

    @classmethod
    def makeParser(cls, description):
        return cls.enrichParser(argparse.ArgumentParser(
            description=description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        ))
