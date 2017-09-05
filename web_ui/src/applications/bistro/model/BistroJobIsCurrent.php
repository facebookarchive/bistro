<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobIsCurrent extends BistroConsensusFinder {

  public function add(/* bool */ $is_current, /* string */ $hostport) {
    // Easy way to test with multiple variants:
    // parent::add(rand() % 2, $hostport.'a');
    return parent::add($is_current, $hostport);
  }

  protected function renderItem(/* string */ $is_current, $_is_consensus) {
    if (!$is_current) {
      return phabricator_tag(
        'span',
        array(
          'title' =>
            'Job is no longer in the Bistro configuration. Data is likely '.
            'to be incomplete, except via the central DB connection. '.
            "Hostsports reporting:\n\n".
            implode("\n", array_keys($this->data[$is_current]))),
        '[historical]'
      );
    } else if (count($this->data) > 1) {
      return '[current]';
    } else {
      return '';  // This is fine when the job is current for all hostports
    }
  }

  public function render() {
    if (!$this->data) {
      return '[job not found]';
    }
    return parent::render();
  }

}
