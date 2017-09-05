<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2HTTPMultiCurlController
  extends BistroBaseMultiCurlController {

  // The first byte of the response decides if it's a response or an error.
  // We are only querying JSON, and valid JSON can never start with an 'e',
  // so we only prepend 'e' for errors.  This saves us a large-string copy
  // when the query succeeds.
  const ERROR = 'e';

  public static function getResultForHostport(
      $hp, /* string */ $query, $send_timeout, $recv_timeout) {

    $start_time = microtime(true);
    try {
      $r = fetch_monitor2_via_http($hp, $query, $send_timeout, $recv_timeout);
      self::profileRequest('ok', $hp, $start_time);
      return $r;
    } catch (Exception $e) {
      self::profileRequest('dead', $hp, $start_time);
      throw new BistroMultiCurlQueryError($e);
    }
  }

  protected static function serializeResult($res) {
    return $res;
  }

  protected static function serializeError(BistroMultiCurlQueryError $err) {
    return self::ERROR.$err->data->getMessage();
  }

}
