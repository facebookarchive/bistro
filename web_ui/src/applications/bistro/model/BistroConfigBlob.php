<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroConfigBlob extends BistroConsensusFinder {

  protected function renderItem(
      /* string */ $config, /* bool */ $is_consensus) {

    // TODO(edit-job): To add editing, these things are needed:
    //  - A backend API that accepts JSON blobs (too lazy to make a form here)
    //  - When $is_consensus is true, make this a fixed-width font textarea
    //  - Enclose this inside a form with a Save button, which
    //    submits to an endpoint which uses fetch_monitor2_via_http()
    //    to save the current JSON blob, and displays the resulting status.
    //  - Change "View Config" to "View / Edit Config" in drop-down.
    return phabricator_tag(
      'pre',
      array('class' => 'config-blob'),
      id(new PhutilJSON())->encodeFormatted(json_decode($config, 1)));
  }

}
