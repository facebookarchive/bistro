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
        for func in cls.ACTIONS.itervalues():
            func(parser)
        return parser

    @classmethod
    def makeParser(cls, description):
        return cls.enrichParser(argparse.ArgumentParser(
            description=description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        ))
