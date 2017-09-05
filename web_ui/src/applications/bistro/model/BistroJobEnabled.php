<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobEnabled extends BistroConsensusFinder {

  public function add(/* bool or null */ $enabled, /* string */ $hostport) {
    $enabled = (bool)$enabled;
    // Easy way to test with multiple variants:
    // if (rand() % 2) { parent::add(!$enabled, $hostport.'a'); }
    return parent::add($enabled, $hostport);
  }

  public function renderItem($item, $_is_consensus) {
    return $item ? 'enabled' : 'disabled';
  }

}
