<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * This is outside of CurlWorkload because CURLOPT_WRITEFUNCTION retains a
 * reference to $this until you destroy the curl handle. Since the handle
 * is destroyed by CurlWorkload's destructor, we'd have a circular reference.
 *
 * So, having a separate instance to pass to CURLOPT_WRITEFUNCTION allows
 * the CurlWorkload destructor to run.
 */
final class BistroCurlBodyRecorder {

  // This limit is a bit arbitrary, but keep it under 30MB, which is HPHP's
  // default string buffer limit.
  const MAX_BYTES = 25000000;

  private $data = '';

  public function reset() {
    $this->data = '';
  }

  public function getBody() {
    return $this->data;
  }

  public function curlWriteCallback($_curl_handle, $buf) {
    $l = min(self::MAX_BYTES - strlen($this->data), strlen($buf));
    $this->data .= substr($buf, 0, $l);
    if ($l < strlen($buf)) {
      bistro_monitor_log()->warn(
        'Truncating Curl response to '.self::MAX_BYTES.' bytes'
      );
    }
    return $l;  // The exact number of bytes is required by the curl API
  }

}

