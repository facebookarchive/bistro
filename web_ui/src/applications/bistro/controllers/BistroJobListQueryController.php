<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobListQueryController extends BistroCommonQueryController {

  protected function makeBistroPrefs() {
    return new BistroJobListPrefs($this->getRequest());
  }

  protected function getPresetPreferenceKey() {
    return 'bistro-monitor-prefs';
  }

  protected function getResultEndpoint() {
    return '/bistro/jobs';
  }

  protected function getPageTitle() {
    return 'Bistro Job Query';
  }

  protected function getSubmitText() {
    return 'View Jobs';
  }

}
