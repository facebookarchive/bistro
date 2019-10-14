# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import os

from facebook.bistro.Args import Args
from facebook.bistro.PluginCollection import PluginCollection

class HostportSources(PluginCollection):
    SEP = '@'

    @classmethod
    def usageException(cls):
        return Exception(
            'You must either pass --hostport, or set the BISTRO_HOSTPORT '
            'environment variable to "prefix{}value". Valid prefixes:\n{}'
            .format(cls.SEP, '\n'.join(
                '  {}{} -- {}'.format(k, cls.SEP, obj.__doc__)
                    for k, obj in cls.key_to_obj.iteritems()  # noqa: B301 T25377293 Grandfathered in
            ))
        )

    @classmethod
    def _getHostportStr(cls, args):
        if hasattr(args, 'hostport') and args.hostport is not None:
            return args.hostport
        hostport = os.environ.get('BISTRO_HOSTPORT')
        if hostport is not None:
            return hostport
        raise cls.usageException()

    @staticmethod
    def literalHostport(val):
        'A literal hostport, with values like: example.com:123'
        return val

    @staticmethod
    def fileHostport(val):
        'Read the hostport from a file (e.g. on NFS); strips whitespace'
        return open(val).read().strip()

    @classmethod
    def get(cls, args):
        s = cls._getHostportStr(args)
        if s.count(cls.SEP) < 1:
            raise cls.usageException()
        key, value = s.split('@', 1)
        if key not in cls.key_to_obj:
            raise cls.usageException()
        return cls.key_to_obj[key](value)

HostportSources.add('', HostportSources.literalHostport)
HostportSources.add('hp', HostportSources.literalHostport)
HostportSources.add('file', HostportSources.fileHostport)

Args.applyToParser(HostportSources, lambda p: p.add_argument('--hostport'))
