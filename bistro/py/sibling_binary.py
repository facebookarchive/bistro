# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.






import os
import sys


def sibling_binary(name):
    candidate = os.path.join(os.path.dirname(sys.argv[0]), name)
    return candidate if os.path.exists(candidate) \
        else os.path.join("/usr/local/bin", name)
