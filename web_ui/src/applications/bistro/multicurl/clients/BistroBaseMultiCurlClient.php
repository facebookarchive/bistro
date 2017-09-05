<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Used both for fetching job data and for fetching logs from multiple
 * Bistro instances.
 *
 * Hides the complexity of multiplexing requests across many machines using
 * CurlMultiWorkload + BistroCurlProtection.
 *
 * Acts as an error boundary -- logging non-fatal errors, and throwing
 * exceptions for invariant violations.
 */
abstract class BistroBaseMultiCurlClient {

  /**
   * Unserializes the response from one Bistro instance. Returns the
   * result, or throws BistroMultiCurlQueryError for bad responses.
   */
  abstract protected function unserializeResponse(
    /* string */ $hp, /* string */ $serialized_response);

  abstract protected function getControllerClass();

  abstract protected function logError($hp, BistroMultiCurlQueryError $err);

  /**
   * Returns array(hostport => response), including only the responses that
   * were fetched without error.
   *
   * Hides the errors from the customer:
   *  - For system errors (timeouts, failed connections, server crashes),
   *    makes an bistro_monitor_log() entry.
   *  - For invariant violations (crypto / data format failures), throws an
   *    Exception.
   */
  public function getSuccessfulHostportResponses(
      BistroCommonPrefs $prefs, array $raw_hostports, /* string */ $query) {

    list($fqdn_hostports, $fqdn_to_original_hp) =
      bistro_hostports_validate_and_make_fqdn($prefs, $raw_hostports);

    // Only use multicurl if we are querying many instances
    // To test multicurl, add "false &&" to the condition.
    if (count($fqdn_hostports)
        <= $prefs->get(BistroCommonPrefs::PREF_HOSTPORTS_PER_CURL)) {
      $hp_to_res =
        $this->getResponsesSerially($prefs, $fqdn_hostports, $query);
    } else {
      $hp_to_res =
        $this->fanOutAndAggregateResponses($prefs, $fqdn_hostports, $query);
    }

    // The rest of the front-end doesn't need FQDNs, and might prefer the
    // more compact partial domains.
    $hostport_responses = array();
    foreach ($hp_to_res as $hp => $response) {
      $hp = bistro_hostport_strip_host_suffix(
        $prefs, idx($fqdn_to_original_hp, $hp, $hp));
      $hostport_responses[$hp] = $response;
    }
    return $hostport_responses;
  }

  private function fanOutAndAggregateResponses(
      BistroCommonPrefs $prefs, array $fqdn_hostports, /* string */ $query) {

    $request = $prefs->getRequest();
    $request_hostname = $request->getHost();
    $username = $request->getUser()->getUserName();

    // Prevent replays of responses
    $secret = base64_encode(bistro_read_random_bytes(
      3 * BistroBaseMultiCurlController::SECRET_BYTES / 4
    ));

    $multi_workload = new CurlMultiWorkload();
    $workloads = array();

    // Best not to be too deterministic with the query grouping
    shuffle($fqdn_hostports);
    // Instead of splitting 6 hostports 5:1, split them 3:3, etc.
    $chunk_size = intval(0.5 + count($fqdn_hostports) / min(
      intval(ceil(count($fqdn_hostports)
        / $prefs->get(BistroCommonPrefs::PREF_HOSTPORTS_PER_CURL))),
      $prefs->get(BistroCommonPrefs::PREF_MAX_CURL_COUNT)
    ));
    $hostport_chunks = array_chunk($fqdn_hostports, $chunk_size);
    foreach ($hostport_chunks as $i => $hps) {
      $workloads[$i] = new CurlWorkload(
        $request_hostname,
        get_class($this).'::CURL_ENDPOINT_PREFIX',
        array(
          $username,
          BistroCurlProtection::protectRequest(
            $username,
            json_encode(array(
              BistroBaseMultiCurlController::REQUEST_HOSTPORTS => $hps,
              BistroBaseMultiCurlController::REQUEST_SECRET => $secret,
              BistroBaseMultiCurlController::REQUEST_QUERY => $query,
              BistroBaseMultiCurlController::REQUEST_SEND_TIMEOUT =>
                $prefs->get(BistroCommonPrefs::PREF_SEND_TIMEOUT_MS),
              BistroBaseMultiCurlController::REQUEST_RECV_TIMEOUT =>
                $prefs->get(BistroCommonPrefs::PREF_RECV_TIMEOUT_MS)))
          ))
      );
      $multi_workload->addWorkload($workloads[$i]);
    }
    $multi_workload->exec();

    $hostport_responses = array();
    foreach ($hostport_chunks as $i => $hps) {
      $error = $workloads[$i]->getError();
      if ($error !== null) {
        // Not an exception, because even when some sub-curls time out,
        // we can still expect to show some data.
        bistro_monitor_log()->error(
          'While fetching hostports: '.implode(', ', $hps).', got '.$error);
        continue;
      }
      foreach ($this->extractResults(
        $workloads[$i]->getResult(),
        $username,
        $secret,
        $hps
      ) as $hp => $serialized_response) {
        // Uncomment to debug on-the-wire data size
        // error_log($hp.' '.strlen($serialized_response));
        try {
          $hostport_responses[$hp] = $this->unserializeResponse(
            $hp, base64_decode($serialized_response));
        } catch (BistroMultiCurlQueryError $err) {
          $this->logError($hp, $err);
        }
      }
    }
    return $hostport_responses;
  }

  private function getResponsesSerially(
      BistroCommonPrefs $prefs, array $fqdn_hostports, /* string */ $query) {

    $get_result_func =
      array($this->getControllerClass(), 'getResultForHostport');
    $send_timeout = $prefs->get(BistroCommonPrefs::PREF_SEND_TIMEOUT_MS);
    $recv_timeout = $prefs->get(BistroCommonPrefs::PREF_RECV_TIMEOUT_MS);

    $hostport_responses = array();
    foreach ($fqdn_hostports as $hp) {
      try {
        $hostport_responses[$hp] = call_user_func(
          $get_result_func, $hp, $query, $send_timeout, $recv_timeout);
      } catch (BistroMultiCurlQueryError $err) {
        $this->logError($hp, $err);
      }
    }
    return $hostport_responses;
  }

  /**
   * Accepts a raw CurlMulti result. Returns
   *
   *   array(hostport => unprotected serialized response),
   *
   * with the replay-protection secret verified, and the hostport keys
   * cross-checked against the initially requested hostports.
   */
  private function extractResults(
    /* string */ $response_protected,
    /* string */ $username,
    /* string */ $secret,
    array $requested_hostports
  ) {
    $response = BistroCurlProtection::unprotectResponse(
      $username,
      $response_protected
    );
    if (!is_string($response) || $response === '') {
      throw new BistroCurlException(
        'Failed to unprotect '.$response_protected
      );
    }

    if (substr($response, 0, 1) !== '{' || substr($response, -1) !== '}') {
      throw new BistroCurlException($response.' is not JSON');
    }

    $response_arr = json_decode($response, /* assoc = */ true);
    if (!is_array($response_arr)) {
      throw new BistroCurlException('json_decode("'.$response.'") failed');
    }

    if (!array_has_exact_keys_set(
      $response_arr,
      array(
        BistroBaseMultiCurlController::RESPONSE_RESULTS,
        BistroBaseMultiCurlController::RESPONSE_SECRET)
    )) {
      throw new BistroCurlException($response.' has unexpected keys');
    }

    if ($response_arr[BistroBaseMultiCurlController::RESPONSE_SECRET] !==
        $secret) {
      throw new BistroCurlException('Wrong secret in '. $response);
    }

    $results =
      $response_arr[BistroBaseMultiCurlController::RESPONSE_RESULTS];
    if (!array_has_exact_keys_set($results, $requested_hostports)) {
      throw new BistroCurlException(
        'Requested hostports ['.
        implode(', ', $requested_hostports).'] but got ['.
        implode(', ', array_keys($results)).']'
      );
    }

    return $results;
  }

}
