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
