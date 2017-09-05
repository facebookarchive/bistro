<?php
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class BistroDataFetcher extends BistroLoadableByName {

  const NUMBER_OF_JOBS_UNKNOWN = 'an unknown number of';

  protected $hostPorts;
  protected $hostPortSource;
  protected $instanceHealthChecker;

  protected $prefs;

  private $numAllJobs;  // So that the user knows if they are seeing all jobs
  private $numFilteredJobs;  // How many matched the filter?
  private $jobIDs;  // Used for a sanity check.

  public static function newFrom(
    BistroHostPortSource $hp_source, BistroJobListPrefs $prefs) {
    $fetcher_name = $prefs->get(BistroJobListPrefs::PREF_FETCHER);
    /* HH_FIXME[4105] Type extra args (to be sent through to constructor) */
    return parent::baseNewFrom($fetcher_name, __CLASS__, $hp_source, $prefs);
  }

  // You should never construct this class directly.
  //
  // Needs to be public because PHP5 will not otherwise let this class's
  // base (BistroLoadableByName) call this constructor via newv().
  public function __construct(
    BistroHostPortSource $hp_source, BistroJobListPrefs $prefs) {
    $this->prefs = $prefs;
    $this->hostPortSource = $hp_source;
    $this->hostPorts = $this->hostPortSource->parse();
    $this->instanceHealthChecker =
          new BistroInstanceHealthChecker($this->prefs);
  }

  //
  // These can only be called after loadJobIDs():
  //

  // Returns populated BistroJobs
  abstract public function loadJobs();
  // For duplicate shard detection. TODO: rename to loadLevelToNodes
  abstract public function loadNodeNames();
  // Returns a job-independent BistroInstanceErrors object.
  abstract public function loadErrors();

  // Fetcher-specific helper for loadJobIDs(). Note that if you fetch all
  // data for all jobs in this function, you will reduce latency of the "get
  // all" query, but will also eliminate most perf benefits of paging.
  abstract protected function fetchAllJobIDs();

  /**
   * Get job IDs to render on this request. Can be called in two ways:
   *
   *  1) [Only when PREF_JOBS is not set]
   *
   *     The argument $job_ids is empty, and we must load all available job
   *     IDs (modulo the pager).  However, it may save latency for your
   *     fetcher to also get summary data for all jobs (and memoize it),
   *     since the common case is currently to show all jobs, rather than to
   *     page through jobs.
   *
   *  2) $job_ids is non-empty -- this puts the fetcher in a state where
   *     it will only fetch data for the requested jobs.
   */
  public function loadJobIDs(
      array $job_ids,
      BistroPagerView $pager,
      /* string */ $job_id_regex) {

    if ($this->jobIDs !== null) {
      if ($this->jobIDs !== $job_ids) {
        throw new Exception(
          'Already called load() with job IDs '.
          print_r($this->jobIDs, 1).' but now requesting job IDs '.
          print_r($job_ids, 1));
      }
      return $this->jobIDs;
    }

    // If the user had specified the job IDs upfront, we won't fetch the
    // whole list.
    if ($job_ids) {
      // TODO: We should ask Bistro for the total # of jobs as part of the
      // loadJobs request, and feed that integer into a ConsensusFinder.
      $this->setNumAllJobs(BistroDataFetcher::NUMBER_OF_JOBS_UNKNOWN);
    } else {
      // NOT TO DO: If this were to take offset & N = $pager->getPageSize()
      // + 1, and fetch all job data for just those jobs, then we wouldn't
      // overfetch when paging.  However, if the job lists don't line up
      // perfectly across instances, we'll have major consensus issues, and
      // no clear way to present this well in the UI.
      //
      // TODO: A more salient optimization for the paged case is just to do
      // two roundtrips: (1) to fetch all job IDs, page them, then (2) fetch
      // data just for the paged IDs.  This increases the chance of
      // connection failure, and the latency of the non-paged case, so we
      // don't do it yet.
      //
      // Possible microoptimization for two roundtrips: fetch up to N IDs
      // starting from offset from each hostport, discard the Nth element
      // from each, merge the lists, and fetch job info for the union (even
      // if it's larger than the pager).  Display more than the pager asked
      // for.
      $job_ids = $this->fetchAllJobIDs();
      $this->setNumAllJobs(count($job_ids));
    }

    $filtered_ids = array();
    foreach ($job_ids as $job_id) {
      // PHP requires delimiters; using curly braces avoids the need for
      // escaping.
      if (preg_match('{'.$job_id_regex.'}', $job_id)) {
        $filtered_ids[] = $job_id;
      }
    }
    $this->setNumFilteredJobs(count($filtered_ids));

    asort($filtered_ids);
    $this->jobIDs =
      $pager->sliceResults(array_slice($filtered_ids, $pager->getOffset()));

    return $this->jobIDs;
  }

  protected function getJobIDs() {
    if ($this->jobIDs === null) {
      throw new Exception('must loadJobIDs before getJobIDs');
    }
    return $this->jobIDs;
  }

  // Tell the user that the job summary bar may not reflect all known jobs.
  public function getNumAllJobs() {
    if ($this->numAllJobs === null) {
      throw new Exception('must loadJobIDs before getNumAllJobs');
    }
    return $this->numAllJobs;
  }
  private function setNumAllJobs($num) {
    if ($this->numAllJobs !== null) {
      throw new Exception('numAllJobs already set to '.$this->numAllJobs);
    }
    $this->numAllJobs = $num;
  }

  // Tell the user how many jobs were filtered out.
  public function getNumFilteredJobs() {
    if ($this->numFilteredJobs === null) {
      throw new Exception('must loadJobIDs before getNumFilteredJobs');
    }
    return $this->numFilteredJobs;
  }
  private function setNumFilteredJobs($num) {
    if ($this->numFilteredJobs !== null) {
      throw new Exception(
        'numFilteredJobs already set to '.$this->numFilteredJobs);
    }
    $this->numFilteredJobs = $num;
  }
}
