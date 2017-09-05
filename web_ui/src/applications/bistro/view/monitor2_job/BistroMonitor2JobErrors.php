<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2JobErrors extends BistroJobResponseView {
  public function render() {
    require_celerity_resource('bistro-job');
    // TODO: Try ConsensusFinder-style rendering here?

    $job_id = $this->job->jobId;
    $error_htmls = array();
    foreach ($this->hostportToResponse as $hp => $response) {
      $why_invalid = idx(adx(adx($response, 'jobs'), $job_id), 'why_invalid');
      if ($why_invalid !== null) {
        $error_htmls[] = hsprintf(
          '<div>Invalid on %s:<pre>%s</pre></div>', $hp, $why_invalid);
      }
    }
    if (!$error_htmls) {
      return null;
    }

    return phabricator_tag('div', array('class' => 'bistro-why-invalid'), array(
      phabricator_tag(
        'h2', array(),
        'Incomplete data. Errors on '.count($error_htmls).' hostports:'),
      $error_htmls));
  }
}
