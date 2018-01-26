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

class TaskBits(object):

    # Use the Base64-URL alphabet because it survives JSON and URLs unescaped.
    CODE = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
    INVERSE_CODE = dict((c, i) for i, c in enumerate(CODE))

    @classmethod
    def encode(cls, b):
        # This requires updates as we add bits, but it's ~25x faster than
        # "urlsafe_b64encode + struct.pack + rstrip 0s and =s"
        return cls.CODE[b & 63] + (cls.CODE[b >> 6] if b >> 6 else '')

    @classmethod
    def decode(cls, s):
        bits = cls.INVERSE_CODE[s[0]]
        if len(s) > 1:
            bits |= cls.INVERSE_CODE[s[1]] << 6
        return bits
