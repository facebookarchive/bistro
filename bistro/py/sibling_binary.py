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

import os
import sys


def sibling_binary(name):
    candidate = os.path.join(os.path.dirname(sys.argv[0]), name)
    return candidate if os.path.exists(candidate) \
        else os.path.join("/usr/local/bin", name)
