<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * This is a slight abuse of ConsensusFinder, in that the same location
 * can report several different errors, which is not done in other
 * consensus finders.
 */
final class BistroErrors extends BistroConsensusFinder {

  protected function renderItem(/* string */ $error, $_is_consensus) {
    $num_errors = count($this->data[$error]);
    return
      phabricator_tag(
        'div',
        array('class' => 'bistro-error'),
        array(
          $num_errors.' instance'.($num_errors > 1 ? 's' : '').' of:',
          phabricator_tag('pre', array(), $error)));
  }

  public function render() {
    $html = parent::render();
    if ($html === '') {
      return '';
    }
    require_celerity_resource('bistro-errors');
    return hsprintf(
      '<div class="bistro-errors"><h1>Job-independent errors:</h1>%s</div>',
      $html);
  }

}
