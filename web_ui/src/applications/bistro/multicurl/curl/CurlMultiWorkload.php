<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * This class lets you parallelize work by making multiple simultaneous HTTP
 * requests to the same server machine via curl_multi_* functions.
 *
 * SECURITY WARNINGS
 *
 * 1) It is hard to write a secure RPC endpoint for this class. Some things
 *    your endpoint must do:
 *
 *     - Only accept connections from the current server's IP.
 *
 *     - Limit the size of incoming requests (for stability / DOS protection).
 *
 *     - Correctly use strong cryptography for BOTH authentication and
 *       integrity protection (and optionally, encryption). This is hard to
 *       get right, consult an expert.
 *
 *     - Accept only very well-specified and safe workloads (do not execute
 *       arbitrary or even complex code, have as little business logic as
 *       possible).
 *
 *     - Strictly validate and safely parse the input: check all input sizes,
 *       check that input strings strictly conform to requirements, escape
 *       all inputs as needed, etc.
 *
 *     - Encrypt its output with the same integrity / authenticity
 *       protection, but a different key (to avoid replay).
 *
 *    If you get all of these right, there's a good chance you won't be
 *    adding a security hole.
 *
 * 2) This class intentionally has a very restricted API, for two reasons:
 *
 *  - This is a form or RPC, and RPC is inherently hard to secure.
 *    The smaller the API, the fewer the holes. Don't add features.
 *    Don't add security holes.
 *
 *  - To make matters worse, server-side curls is a form of transport that
 *    is rife with exploits and errors, because the underlying protocol is
 *    complex and not optimized for safety. This is intentionally a very
 *    locked-down use of the curl API. Therefore, resist the temptation
 *    to turn this into some kind of general-purpose curl API. Such APIs
 *    are very hard to make safe, and they requires expertise and time
 *    that I do not have.
 *
 * Therefore, this is a special purpose API to do one thing safely.
 * Please, do NOT attempt to generalize it. You will introduce a security
 * hole and everyone will be sad.
 *
 * Especially, do NOT remove the 'final' to do 'just one more thing'.
 *
 * Sample usage:
 *
 *   class ParallelWorker {
 *
 *     const ENDPOINT_PREFIX = '/parallel/worker/';
 *
 *     private $hostname;
 *     private $encrypter;
 *
 *     private $multiWorkload;
 *     private $workloads = array();
 *
 *     // Needs the current request object to get the hostname.
 *     public function __construct($request, $encryption_key) {
 *       $this->multiWorkload = new CurlMultiWorkload();
 *       $this->hostname = $request->getHost();
 *       $this->encrypter = new AuthenticationAndIntegrityEncrypter(
 *         $encryption_key
 *       );
 *     }
 *
 *     public function addWork($id, array $workload_spec) {
 *       $workload = new CurlWorkload(
 *         $this->hostname,
 *         'ParallelWorker::ENDPOINT_PREFIX',
 *         $this->encrypter->encrypt(json_encode($workload_spec))
 *       );
 *       $this->multiWorkload->addWorkload($workload);
 *       $this->workloads[$id] = $workload;
 *     }
 *
 *     // Returns results and errors in one array, maybe not the best API ever
 *     public function computeAllResults() {
 *       $this->multiWorkload->exec();
 *
 *       $results = array();
 *       foreach ($this->workloads as $id => $workload) {
 *         $error = $workload->getError();
 *         if ($error === null) {
 *           $results[$id] = $workload->getResult();
 *         } else {
 *           $results[$id] = $error;
 *         }
 *         $this->multiWorkload->removeWorkload($workload);  // Not needed?
 *       }
 *
 *       // Get ready to accept more work.
 *       $this->workloads = array();
 *       $this->multiWorkload = new CurlMultiWorkload();
 *     }
 *   }
 *
 * Do NOT remove the 'final' to do 'just one more thing' -- see above.
 */
final class CurlMultiWorkload {

  const TIMEOUT_SEC = 90;  // TODO(hi-pri): Make this configurable, with a max

  const HANDLE_KEY = 1;
  const STATUS_KEY = 2;

  private $multiHandle = null;
  private $handleHashToWorkload = array();

  public function __construct() {
    $this->multiHandle = curl_multi_init();
  }

  public function __destruct() {
    curl_multi_close($this->multiHandle);
  }

  public function addWorkload(CurlWorkload $workload) {
    $workload->addToMultiHandle($this->multiHandle);
    $this->handleHashToWorkload[$workload->getHash()] = $workload;
  }

  public function removeWorkload(CurlWorkload $workload) {
    $workload->removeFromMultiHandle($this->multiHandle);
    unset($this->handleHashToWorkload[$workload->getHash()]);
  }

  /**
   * Tries to execute all the attached curl handles.
   *
   * Returns true if some handle is still executing, false otherwise.
   *
   * This will only return once all requests are finished (or the timeout is
   * hit).
   */
  public function exec() {
    $deadline = microtime(true) + self::TIMEOUT_SEC;
    do {
      $still_running = true;  // is really assigned by reference two lines down
      while (
        curl_multi_exec($this->multiHandle, $still_running) ==
          CURLM_CALL_MULTI_PERFORM &&
        $still_running
      ) {}

      $can_select = true;
      // It's crucial that we use multi_info, because its 'result' field the
      // only reliable way of finding if an error has occurred. curl_errno()
      // and curl_error() can be empty despite errors.
      while (($mi = curl_multi_info_read($this->multiHandle))) {
        $hash = CurlWorkload::getHashFromHandle($mi['handle']);
        $workload = idx($this->handleHashToWorkload, $hash);
        if (!$workload) {
          // This shouldn't happen except for cosmic rays, so ignore it.
          bistro_monitor_log()->error(
            'Failure fetching server data: Unknown workload hash '.$hash);
          continue;
        }
        $workload->recordMultiInfo($mi);

        if ($mi['result'] !== CURLE_OK) {
          // We will retry this workload
          $still_running = true;
          $this->removeWorkload($workload);
          $this->addWorkload($workload);
          // Now we need to call curl_multi_exec() before we can call
          // curl_multi_select() again.
          $can_select = false;
        }
      }

      $now = microtime(true);
      if ($still_running && $can_select && $now < $deadline) {
        // Ignore the return value since -1 is the only special value and
        // it can be returned spuriously anyway.
        curl_multi_select($this->multiHandle, $deadline - $now);
      }
      // I think it's best not to update $now here, since curl_multi_exec()
      // is intended not to block (see libcurl curl_multi_perform).
    } while ($still_running && $now < $deadline);

    return $still_running;
  }

}
