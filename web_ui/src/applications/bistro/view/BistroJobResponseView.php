<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @concrete-extensible
 */
class BistroJobResponseView {
  protected $job;
  protected $hostportToResponse;

  public function __construct(BistroJob $job, array $hostport_to_response) {
    $this->job = $job;
    $this->hostportToResponse = $hostport_to_response;
  }

  protected function makeChild($class_name) {
    return newv($class_name, array($this->job, $this->hostportToResponse));
  }
}
