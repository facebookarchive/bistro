# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

# A hook to load non-OSS plugins (e.g. for access via PluginCollection)
try:
    import facebook.bistro.plugins
    (lambda x: facebook.bistro.plugins)  # Make pyflakes think it is used
except ImportError:
    pass
