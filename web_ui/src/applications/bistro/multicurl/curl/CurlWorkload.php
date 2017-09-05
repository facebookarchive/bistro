<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Used only with CurlMultiWorkload, see the main docblock there.
 *
 * This class intentionally has a very restricted API. There are two reasons:
 *
 *  - This is a form or RPC, and RPC is inherently hard to secure.
 *    The smaller the API, the fewer the holes. Don't add features.
 *    Don't add security holes.
 *
 *  - To make matters worse, server-side curls is a form of transport that
 *    is rife with exploits and errors, because the underlying protocol is
 *    complex and not optimized for safety.  This is intentionally a very
 *    locked-down use of the curl API.  Therefore, resist the temptation
 *    to turn this into some kind of general-purpose curl API.  Such APIs
 *    are very hard to make safe, and they requires expertise and time
 *    that I do not have.
 *
 * Therefore, this is a special purpose API to do one thing safely.
 * Please, do NOT attempt to generalize it. You will introduce a security
 * hole and everyone will be sad.
 *
 * Especially, do NOT remove the 'final' to do 'just one more thing'.
 */
final class CurlWorkload {

  // TODO(hi-pri): Make configurable, with a max
  const TIMEOUT_MS = 40000;
  const CONNECT_TIMEOUT_MS = 40000;

  private $bodyRecorder;

  private $errorText;
  private $errNo;

  private $curlHandle;
  private $multiHandle;  // So we can't be added to two multi-handles

  /**
   * Read the class doc first.
   *
   *   $host is only used in the HTTP request header -- we always talk to
   *   the current server's IP.
   *
   *   $path_prefix_const_name is the name of a string constant, i.e.
   *   'SOME_CONST_NAME' or 'SomeClass::CONST_NAME'.  The string is subject
   *   to some constraints -- see below.  I require a constant prefix to
   *   reduce the odds of a user of this class talking to arbitrary
   *   endpoints.
   *
   *   $path_suffix is the data / payload part of the URL path, and it's
   *   appended directly to the path prefix, after being url-encoded.
   *
   * DANGER: be very careful when composing the path -- your goal should be
   * to make it so that workloads will never hit anything but very specific
   * endpoints with hardcoded path prefixes.
   *
   * TODO(mid-pri): Ask somebody smart if I should switch to POST requests.
   */
  public function __construct($host, $path_prefix_const_name, $path_parts) {
    $path_prefix = constant($path_prefix_const_name);
    if (!preg_match('/^\/[a-zA-Z0-9_-][a-zA-Z0-9\/_-]*\/$/', $path_prefix)) {
      throw new CurlWorkloadException(
        'Path prefix '.$path_prefix.' from '.$path_prefix_const_name.' must '.
        'start and end with "/", contain only [a-zA-Z0-9/_-] characters, '.
        'be at least 3 characters long, not all slashes.'
      );
    }
    $path = substr($path_prefix, 0, -1);  // Remove the slash, re-add below
    foreach ($path_parts as $path_part) {
      $path .= '/'.urlencode($path_part);
    }

    $this->errorText = 'Not executed yet';

    $this->curlHandle = curl_init();

    // Usie the IP instead of a hostname in the URI for two reasons:
    //  1) Ensure that we talk to the current server, rather than any other
    //     machine (required as a security measure).
    //  2) Prevent some time to check/time to use attacks.
    $ip = $_SERVER['SERVER_ADDR'];
    if (filter_var($ip, FILTER_VALIDATE_IP, FILTER_FLAG_IPV6)) {
      // Failing to bracket an IPv6 address sometimes causes Curl to fail
      // with "IPv6 numerical address used in URL without brackets".
      $ip = '['.$ip.']';
    }
    curl_setopt($this->curlHandle, CURLOPT_URL, 'http://'.$ip.$path);

    // The request headers should still include the right hostname, because
    // Phabricator does not work otherwise.
    $headers = array('Host: '.$host);
    $magic_header =
      PhabricatorEnv::getEnvConfig('bistro.self-curl-extra-http-header');
    if ($magic_header) {
      $headers[] = $magic_header;
    }
    curl_setopt($this->curlHandle, CURLOPT_HTTPHEADER, $headers);
    // Ignore the response headers
    curl_setopt($this->curlHandle, CURLOPT_HEADER, False);

    $this->bodyRecorder = new BistroCurlBodyRecorder();
    curl_setopt(
      $this->curlHandle,
      CURLOPT_WRITEFUNCTION,
      array($this->bodyRecorder, 'curlWriteCallback')
    );

    // Set timeouts
    curl_setopt($this->curlHandle, CURLOPT_TIMEOUT_MS, self::TIMEOUT_MS);
    curl_setopt(
      $this->curlHandle,
      CURLOPT_CONNECTTIMEOUT_MS,
      self::CONNECT_TIMEOUT_MS
    );

    // Do not follow 'Location:' header redirects
    curl_setopt($this->curlHandle, CURLOPT_FOLLOWLOCATION, false);
    // Accept all known encodings
    curl_setopt($this->curlHandle, CURLOPT_ENCODING, '');
  }

  public function __destruct() {
    curl_close($this->curlHandle);
  }

  public function recordMultiInfo(array $mi) {
    // In theory, $errno should be set whenever the text is nonempty. In
    // practice, I query both just in case.  curl_errno() is rumored to be
    // useless in the curl_multi usage.
    $this->errorText = curl_error($this->curlHandle);
    $this->errNo = $mi['result'];
  }

  public function getError() {
    if ($this->errNo !== CURLE_OK || $this->errorText !== '') {
      return new CurlWorkloadException(
        'Got error: '.$this->errorText.', #'.$this->errNo
      );
    }
    if ($this->bodyRecorder->getBody() === '') {
      return new CurlWorkloadException('No body, did you forget to exec()?');
    }
    return null;
  }

  public function getResult() {
    $error = $this->getError();
    if ($error !== null) {
      throw $error;
    }
    return $this->bodyRecorder->getBody();
  }

  public function addToMultiHandle($multihandle) {
    if ($this->multiHandle !== null) {
      throw new CurlWorkloadException(
        'Curl handle '.$this->curlHandle.' already belongs to multihandle '.
        $this->multiHandle
      );
    }
    if (curl_multi_add_handle($multihandle, $this->curlHandle) !== 0) {
      throw new CurlWorkloadException(
        'Failed to add curl handle '.$this->curlHandle.' to multihandle '.
        $multihandle
      );
    }
    $this->multiHandle = $multihandle;
    $this->bodyRecorder->reset();  // Clear any previously fetched data
  }

  public function removeFromMultiHandle($multihandle) {
    if ($this->multiHandle !== $multihandle) {
      throw new CurlWorkloadException(
        'Curl handle '.$this->curlHandle.' can only be removed from '.
        ' multihandle '.$this->multiHandle.', but not '.$multihandle
      );
    }
    if (curl_multi_remove_handle($multihandle, $this->curlHandle) !== 0) {
      throw new CurlWorkloadException(
        'Failed to remove curl handle '.$this->curlHandle.' from '.
        'multihandle '.$multihandle
      );
    }
    $this->multiHandle = null;
  }

  public function getHash() {
    return self::getHashFromHandle($this->curlHandle);
  }

  /**
   * This is the only known way of getting a unique hashable description of
   * a curl handle.  In Zend PHP, spl_object_hash() does not work on
   * resources.  However, casting a handle to string gives a unique result.
   */
  public static function getHashFromHandle(/* curl handle */ $handle) {
    return (string)$handle;
  }

}
