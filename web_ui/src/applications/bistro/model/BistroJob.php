<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @concrete-extensible
 */
final class BistroJob {

  public $jobId;
  public $prefs;

  // These are all ConsensusFinders to detect configuration differences
  // between hosts.
  public $isCurrentConsensus;
  public $dependencies;
  public $enabled;
  public $config;
  public $owner;
  public $priority;
  public $level;
  public $packageIDs;
  public $path;
  public $modifiedTime;
  public $createdTime;

  public $summary;  // the status summary presentation

  public $currentRuntimes = array();

  public function __construct($job_id, BistroJobListPrefs $prefs) {
    $this->jobId = $job_id;
    $this->prefs = $prefs;

    $this->isCurrentConsensus = new BistroJobIsCurrent();
    $this->dependencies = new BistroJobDependencies();
    $this->enabled = new BistroJobEnabled();
    $this->owner = new BistroConsensusFinder();
    $this->priority = new BistroConsensusFinder();
    $this->level = new BistroConsensusFinder();
    $this->packageIDs = new BistroConsensusFinder();
    $this->path = new BistroJobPath();
    $this->createdTime = new BistroConsensusTime();
    $this->modifiedTime = new BistroConsensusTime();
    $this->config = new BistroConfigBlob();
  }

  /**
   * Collects running_tasks from multiple hostports, detects tasks that
   * have been running too long.
   */
  public function addCurrentRunningTasks(array $cur_running_tasks, $hp) {
    $max_runtime = $this->prefs->get(BistroJobListPrefs::PREF_MAX_RUNTIME);
    // An idealist would use the scheduler time here, but not us ;)
    $cur_time = time();
    foreach ($cur_running_tasks as $node => $running_task) {
      $runtime = $cur_time - $running_task['start_time'];
      // Somewhat redundant with duplicate node detection, but why not?
      if (isset($this->currentRuntimes[$node])) {
        bistro_monitor_log()->warn(
          'Another scheduler already reported a runtime of '.
          $this->currentRuntimes[$node].' seconds for '.$this->jobId.' on '.
          $node.', while '.$hp.' reports '.$runtime.' seconds / '.
          json_encode($running_task));
      }
      $this->currentRuntimes[$node] = $runtime;
      if ($runtime > $max_runtime) {
        bistro_monitor_log()->warn(
          $this->jobId.' running for '.$runtime.' > '.$max_runtime.
          ' seconds (GET param "max_runtime") on '.$node.' on '.$hp.
          ' / '.idx($running_task, 'worker_shard', ''));
      }
    }
  }

  public function getURI() {
    return self::getURIForID($this->prefs, $this->jobId);
  }

  public static function getURIForID(BistroJobListPrefs $prefs, $job_id) {
    $uri = $prefs->getURI('/bistro/jobs');
    $uri->setQueryParam(
      BistroJobListPrefs::PREF_JOBS,
      // The top-of-page query form looks nicer if we don't wrap the job ID
      // in JSON, so only use it if the ID contains whitespace.
      (preg_match('/\s/', $job_id) ? json_encode(array($job_id)) : $job_id));
    return $uri;
  }

}
