<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2HTTPFetcher extends BistroBaseMonitor2Fetcher {

  public static function getName() {
    return 'monitor2_http';
  }

  public static function getTitle() {
    return 'monitor2-http';
  }

  public static function getDescription() {
    return 'Connect to each of the Bistro instances at a given host-port '.
           ' via the Monitor2-HTTP protocol.';
  }

  protected function getResponses($hostports, $query) {
    $multi_curl_fetcher = new BistroMonitor2HTTPMultiCurlClient();
    return $multi_curl_fetcher->getSuccessfulHostportResponses(
      $this->prefs,
      $hostports,
      $query);
  }

}
