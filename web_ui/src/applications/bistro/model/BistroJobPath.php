<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobPath extends BistroConsensusFinder {

  protected function renderItem(/* string */ $item, $_is_consensus) {
    if (!$item) {  // Bistro has no packages or paths
      return '';
    }

    $listener_id = uniqid('job-path-dots');
    $detail_id = uniqid('job-path-detail');

    Javelin::initBehavior(
      'add-detail-toggle-listener',
      array(
        'detail_html' => hsprintf('%s', dirname($item)),
        'detail_id' => $detail_id,
        'listener_id' => $listener_id));

    return array(
      phabricator_tag(
        'a',
        array('id' => $listener_id),
        array(
          phabricator_tag(
            'span',
              array('class' => 'job-full-path', 'id' => $detail_id),
            ''),
          hsprintf('<b>...</b>'))),
      '/',
      basename($item));
  }

}
