<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * WARNING WARNING WARNING: this is an extremely security-sensitive
 * endpoint, because it accepts *all* Phabricator web requests and
 * pontentially makes internal requests (if your webserver has internal
 * network access, tut tut) using their input.
 *
 * Do NOT copy or modify this endpoint without careful security scrutiny.
 *
 * This is very deliberately not a PhabricatorController, since this might
 * possibly reduce the attack area.
 *
 * The current login-free solution is only barely okay for three reasons:
 *   a) We only accept connections from the local machine.
 *   b) The input is cryptographically signed to ensure that its
 *      input is coming from an authenticated user of Bistro Monitor.
 *   c) This endpoint does very-well constrained work with a deliberately
 *      tiny API, tight error checking, and hence small attack area.
 *
 * TODO: It would be ideal to return a 400 on fatals, so that the attacker
 * cannot easily distinguish fatals from other errors.
 */
abstract class BistroBaseMultiCurlController extends AphrontController {

  // Must be false in production
  const DEBUG = false;

  // Usage: tail -f error.log | grep ^PROFILE_REQUEST
  //
  // Must be false in production
  const PROFILE_REQUEST = false;

  const REQUEST_HOSTPORTS = 1;
  const REQUEST_SECRET = 2;
  const REQUEST_QUERY = 3;
  const REQUEST_SEND_TIMEOUT = 4;
  const REQUEST_RECV_TIMEOUT = 5;

  const SECRET_BYTES = 64;  // Must be divisible by 4

  const RESPONSE_RESULTS = 1;
  const RESPONSE_SECRET = 2;

  // Keep these high -- these are intended as last-ditch sanity checks
  const MAX_REQUEST_BYTES = 1000000;
  const MAX_HOSTPORTS = 10000;

  private $data;
  private $username;  // Only safe to access if data is set

  /**
   * Returns the unserialized result.
   * Throw BistroMultiCurlControllerError on any error => returns HTTP 400.
   */
  abstract public static function getResultForHostport(
    $hp, /* string */ $query, $send_timeout, $recv_timeout);

  /**
   * Serializes the output of getResultForHostport().
   */
  abstract protected static function serializeResult($result);

  /**
   * Serializes a BistroMultiCurlQueryError thrown by getResultForHostport().
   */
  abstract protected static function serializeError(
    BistroMultiCurlQueryError $err);



  public function willProcessRequest(array $data) {
    // Sheer paranoia: make timing attacks harder by wasting up to 25ms
    usleep(mt_rand(0, 25000));

    // Request must come from this server's IP
    $source_ip = bistro_get_remote_address($this->getRequest());
    $expected_ip = $_SERVER['SERVER_ADDR'];
    // Without inet_pton, IPv6 addresses are hard to compare correctly.
    if (inet_pton($expected_ip) !== inet_pton($source_ip)) {
      self::debugLog('Source IP '.$source_ip.' != '.$expected_ip);
      return;
    }

    // Data not too long
    $input = idx($data, 'data');
    if (strlen($input) > self::MAX_REQUEST_BYTES) {
      self::debugLog('Input too long: '.strlen($input));
      return;
    }

    // Checks that the username is valid and has security keys, and that
    // the request key decrypts the data
    $this->username = idx($data, 'user');
    $input = BistroCurlProtection::unprotectRequest($this->username, $input);
    if ($input === null) {
      self::debugLog('Failed to unprotect input for '.$this->username);
      return;
    }

    // Expect a non-empty JSON array
    $input = json_decode($input, /* assoc = */ true);
    if (!$input || !is_array($input)) {
      self::debugLog('Failed to JSON-decode input');
      return;
    }

    $this->data = $input;
  }

  protected static function profileRequest($event, $hostport, $start_time) {
    if (!self::PROFILE_REQUEST) {
      return;
    }
    error_log(
      'PROFILE_REQUEST '.$event.' '.$hostport.' '.
      (microtime(true) - $start_time));
  }

  private static function debugLog($message) {
    if (!self::DEBUG) {
      return;
    }
    error_log(__CLASS__.'::debugLog(): '.$message);
  }

  private static function getFields(array $data, array $fields_in_order) {
    if (!array_has_exact_keys_set($data, $fields_in_order)) {
      throw new BistroMultiCurlControllerError(
        'Unexpected fields in '.print_r($data, 1));
    }
    return array_values(array_select_keys($data, $fields_in_order));
  }

  private function validateAndMakeResponse() {
    // Check that the request had precisely the requested fields
    list($hostports, $secret, $query, $send_timeout, $recv_timeout) =
      self::getFields($this->data, array(
        self::REQUEST_HOSTPORTS,
        self::REQUEST_SECRET,
        self::REQUEST_QUERY,
        self::REQUEST_SEND_TIMEOUT,
        self::REQUEST_RECV_TIMEOUT));
    // Shallow validation on the input fields
    if (!is_array($hostports) || count($hostports) > self::MAX_HOSTPORTS) {
      throw new BistroMultiCurlControllerError(
        'Bad hostport list '.print_r($hostports, 1));
    }
    if (!is_string($secret) || strlen($secret) !== self::SECRET_BYTES) {
      throw new BistroMultiCurlControllerError(
        'Bad secret '.print_r($secret, 1));
    }
    if (!is_string($query)) {
      throw new BistroMultiCurlControllerError(
        'Non-string query '.print_r($query, 1));
    }

    // Check each hostport value
    foreach ($hostports as $hostport) {
      if (!is_string($hostport) || !bistro_hostport_is_valid_fqdn($hostport)) {
        throw new BistroMultiCurlControllerError(
          'Bad hostport '.print_r($hostport, 1));
      }
    }

    // Run the queries and encode results / errors
    $results = array();
    foreach ($hostports as $hp) {
      try {
        // Have to use static:: to appease HackLang :/
        $ser_result = static::serializeResult(static::getResultForHostport(
          $hp, $query, $send_timeout, $recv_timeout));
      } catch (BistroMultiCurlQueryError $err) {
        // Have to use static:: to appease HackLang :/
        $ser_result = static::serializeError($err);
      }
      // Base64 is needed because my multicurl wire format is JSON, which
      // can't handle binary data.
      //
      // TODO(speed): A binary wire format would likely make multicurls
      // faster, since we'd be copying and munging less data.  But for best
      // performance we'd also stop encrypting the response.  Here's the
      // fastest wire format I can think of:
      //   <length of JSON string><JSON array of "hostport": response length>
      //   <hostport binary response 1><response 2>...
      // Possibly, PHP serialize($results) would work well too.
      $results[$hp] = base64_encode($ser_result);
    }

    return array(
      self::RESPONSE_RESULTS => $results,
      self::RESPONSE_SECRET => $secret);
  }


  public function processRequest() {
    if (!is_array($this->data)) {
      // Failed in argument parsing -- no extra logging needed
      return new Aphront400Response();
    }
    try {
      // FileResponse to produce non-HTML headers... not important.
      return id(new AphrontFileResponse())->setContent(
        BistroCurlProtection::protectResponse(
          $this->username, json_encode($this->validateAndMakeResponse())));
    } catch (BistroMultiCurlControllerError $ex) {
      self::debugLog(strval($ex));
      return new Aphront400Response();
    }
  }

}
