# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.






import json
import requests
import logging
log = logging.getLogger(__name__)

from facebook.bistro.HostportSources import HostportSources

def query_bistro(args, req):
    hostport = HostportSources.get(args)
    log.debug('Querying Bistro at %s', hostport)
    r = requests.post('http://{}'.format(hostport), data=json.dumps(req),
                      timeout=60)
    response = json.loads(r.text)
    assert 'error' not in response, response['error']
    for k in req.keys():
        assert 'error' not in response[k], \
            '{}: {}'.format(k, response[k]['error'])
    return response
