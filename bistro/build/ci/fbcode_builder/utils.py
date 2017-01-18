#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
'Miscellaneous utility functions.'

import itertools
import logging
import subprocess
import sys


def recursively_flatten_list(l):
    return itertools.chain.from_iterable(
        (recursively_flatten_list(i) if type(i) is list else (i,))
            for i in l
    )


def run_command(*cmd, **kwargs):
    'The stdout of most fbcode_builder utilities is meant to be parsed.'
    logging.debug('Running: {0} with {1}'.format(cmd, kwargs))
    kwargs['stdout'] = sys.stderr
    subprocess.check_call(cmd, **kwargs)
