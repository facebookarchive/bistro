<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobDependencies
  extends BistroConsensusFinder {

  private $renderJobIDFunc;

  // Not render() because PHP 5.2 is dumb
  public function renderDep(/* func */ $render_job_id_func) {
    $this->renderJobIDFunc = $render_job_id_func;
    return parent::render();
  }

  // Not add() because PHP 5.2 is dumb
  public function addDep(array $deps, /* string */ $hostport) {
    if ($deps !== null) {
      asort($deps);
      $deps = implode(',', $deps);  // TODO(lo-pri): Check for ',' in deps?
    }
    // Easy way to test with multiple variants:
    // if (rand() % 2) { parent::add($deps.'a', $hostport.'a'); }
    return parent::add($deps, $hostport);
  }

  protected function renderItem(/* string */ $item, $_is_consensus) {
    if (!$item) {
      if (count($this->data) > 1) {
        return 'no dependencies';
      } else {
        // Don't show dependencies when the consensus is that they are missing
        return '';
      }
    }
    $deps = array();
    foreach (explode(',', $item) as $dep) {
      $deps[] = call_user_func($this->renderJobIDFunc, $dep);
    }
    return phutil_implode_html(', ', $deps);
  }

}
