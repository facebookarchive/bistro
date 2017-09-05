<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroHTTPTaskLogQueryController
    extends BistroCommonQueryController {

  protected function makeBistroPrefs() {
    return new BistroTaskLogPrefs($this->getRequest());
  }

  protected function getPresetPreferenceKey() {
    return 'bistro-task-log-recent-prefs';
  }

  protected function getResultEndpoint() {
    return '/bistro/logs/view';
  }

  protected function getPageTitle() {
    return 'Bistro Task Log Query';
  }

  protected function getSubmitText() {
    return 'View Logs';
  }

}
