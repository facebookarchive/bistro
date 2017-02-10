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
