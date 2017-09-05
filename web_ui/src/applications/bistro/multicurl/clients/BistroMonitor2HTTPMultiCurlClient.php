<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2HTTPMultiCurlClient
  extends BistroBaseMultiCurlClient {

  const CURL_ENDPOINT_PREFIX = '/bistro/monitor2_http_multi/';

  protected function getControllerClass() {
    return 'BistroMonitor2HTTPMultiCurlController';
  }

  protected function logError($hp, BistroMultiCurlQueryError $err) {
    bistro_monitor_log()->error('Error connecting to '.$hp, $err->data);
  }

  protected function unserializeResponse(
    /* string */ $hp,
    /* string */ $response) {
    if ($response[0] === BistroMonitor2HTTPMultiCurlController::ERROR) {
      throw new BistroMultiCurlQueryError(substr($response, 1));
    }
    return $response;
  }

}
