<?php
// Copyright 2004-present Facebook. All Rights Reserved.

function parse_monitor2_response($request, $hp, $raw_response_str) {
  $keys = array_keys(array_diff_key($request, array('prefs' => 1)));
  $bad_keys = $keys;
  $response = array();
  // XXX make these available to InstanceHealthChecker
  $error_texts = array();

  // json_decode() can only handle UTF-8, while the server can potentially
  // send us non-UTF-8 JSON blob.  Yes, it's a thing.  No, the JSON spec
  // doesn't matter.  Mr. Crockford -- when can I use trailing commas?
  $response_str = iconv('latin1', 'utf-8', $raw_response_str);
  if ($response_str === false) {  // Should never happen? Probably?
    $error_texts[] = 'Could not convert response from latin1 to UTF-8: "'.
      strval($raw_response_str).'"';
  } else {
    $raw_response = json_decode($response_str, /* assoc = */ true);
    if (!is_array($raw_response)) {
      $error_texts[] = 'Invalid JSON in response: "'.strval($response_str).'"';
    } else {
      // TODO(lo-pri): consider checking for keys that weren't requested?
      $bad_keys = array();
      $error_texts = array();
      foreach ($keys as $key) {
        $value = idx($raw_response, $key, array());
        $error = idx($value, 'error');
        $data = idx($value, 'data');

        // Fake a 'running_tasks' handler using 'runtimes'
        // TODO: Delete once all Bistro deployments have running_tasks
        if ($error !== null && $key === 'running_tasks') {
          $value = idx($raw_response, 'runtimes', array());
          $error = idx($value, 'error');
          $data = idx($value, 'data');
          if ($data !== null) {
            $cur_time = time();
            $new_data = array();
            foreach ($data as $job => $node_runtime) {
              foreach ($node_runtime as $node => $runtime) {
                $new_data[$job][$node] = array(
                  'start_time' => $cur_time - $runtime);
              }
            }
            $data = $new_data;
          }
        }

        if (!is_array($value) || !$value ||
            $data === null || $error !== null) {
          $bad_keys[] = $key;
        }
        if ($error !== null) {
          $error_texts[] = $key.': '.$error;
        }
        if ($data !== null) {
          $response[$key] = $data;
        }
      }
    }
  }

  if ($bad_keys) {
    bistro_monitor_log()->error(
      'Hostport '.$hp.' returned no, partial, or bad data for '.
      implode(', ', $bad_keys).'. That is a bug. The displayed '.
      'results are incomplete.',
      $error_texts
        ? "Error texts:\n\n".implode("\n\n", $error_texts)
        : null);
  }

  return array($response, array_keys($response) == $keys);
}

function bistro_check_value($val, $msg) {
  if (!$val) {
    throw new Exception($msg);
  }
}

function fetch_monitor2_via_http(
    $hostport, $query, $send_timeout, $recv_timeout) {

  $h = curl_init();
  bistro_check_value($h !== false, 'Could not make CURL handle');
  bistro_check_value(true === curl_setopt_array($h, array(
    CURLOPT_POST => true,
    CURLOPT_URL => 'http://'.$hostport.'/',
    CURLOPT_POSTFIELDS => $query,
    CURLOPT_HTTPHEADER => array('Content-Length: '.strlen($query)),
    CURLOPT_HEADER => false,  // Don't return the response headers
    CURLOPT_CONNECTTIMEOUT_MS => $send_timeout,
    CURLOPT_TIMEOUT_MS => $recv_timeout,
    CURLOPT_FOLLOWLOCATION => false,  // Do not follow 'Location:' redirects
    CURLOPT_ENCODING => '',  // Accept all known encodings (lol)
    CURLOPT_BINARYTRANSFER => true,  // Not needed after PHP 5.1.3
    CURLOPT_RETURNTRANSFER => true,  // Return data directly from curl_exec
  )), 'Could not set CURL options');

  $response = curl_exec($h);
  bistro_check_value($response !== false, 'CURL error: '.curl_error($h));
  curl_close($h);

  return $response;
}
