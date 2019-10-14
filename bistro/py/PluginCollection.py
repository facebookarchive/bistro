# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

class PluginCollection(object):  # abstract
    '''
    A singleton dict for finding plugins -- usage:

      1) Inherit to make a collection:

        class FunkyPlugins(PluginCollection):
            pass

      3) Add plugins to the collection (usually, at module initialization):

        FunkyPlugins.add('groovy', lambda x, y: '{} rocks {}'.format(x, y))

      4) Access plugins via their keys, i.e.

        FunkyPlugins.key_to_obj['groovy']
    '''

    @classmethod
    def add(cls, key, obj):
        # Each child class should have a separate dictionary, so we don't
        # declare one inside this base class at all.
        if 'key_to_obj' not in cls.__dict__:
            cls.key_to_obj = {}
        if key in cls.key_to_obj:
            raise Exception('Key {} is used both by {} and {}'.format(
                key,
                cls.key_to_obj[key],
                obj
            ))
        cls.key_to_obj[key] = obj
