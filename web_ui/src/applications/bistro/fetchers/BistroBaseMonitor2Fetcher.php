<?php
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class BistroBaseMonitor2Fetcher extends BistroDataFetcher {

  private $hostportToResponse;

  public static function getName() {
    return null;  // Appease LoadableByName reflection -- set by children
  }

  abstract public static function getTitle();

  abstract protected function getResponses($hostports, $query);

  public function getSummary() {
    return  // Using static:: to appease HackLang :/
      static::getTitle().' connection to '.$this->hostPortSource->getSummary();
  }

  private function addResponseToJob($job, /* string */ $hp, array $response) {
    $job->addCurrentRunningTasks(
      adx(adx($response, 'running_tasks'), $job->jobId), $hp);

    $j = idx(adx($response, 'jobs'), $job->jobId);
    $job->isCurrentConsensus->add($j !== null, $hp);

    if ($j === null) {
      return;
    }
    $job->enabled->add(idx($j, 'enabled'), $hp);
    $deps = adx($j, 'depends_on');
    $job->dependencies->addDep($deps ? $deps : array(), $hp);
    $job->path->add(idx($j, 'full_path'), $hp);
    $job->owner->add(idx($j, 'owner'), $hp);
    $job->priority->add(idx($j, 'priority'), $hp);
    $level = idx($j, 'level');
    $job->level->add($level ? $level : idx($j, 'level_for_tasks'), $hp);
    $job->modifiedTime->add(idx($j, 'modify_time'), $hp);
    $job->createdTime->add(idx($j, 'create_time'), $hp);
    $job->config->add(json_encode($j), $hp);

    // Bistro has no package management, so these are only used in PyAnthill.
    $package_id = idx($j, 'package');
    if (isset($j['package_ver'])) {
      $package_id .= $j['package_ver'];
    }
    $job->packageIDs->add($package_id, $hp);

    // TODO: backoff interval? cutoff_time? resources! config? filters?
  }

  public function loadJobs() {
    $job_ids = $this->getJobIDs();
    $this->loadResponses($job_ids);

    $jobs = array();
    foreach ($job_ids as $job_id) {
      $job = new BistroJob($job_id, $this->prefs);
      $job_summary =
        new BistroMonitor2JobSummary($job, $this->hostportToResponse);
      foreach ($this->hostportToResponse as $hp => $response) {
        $this->addResponseToJob($job, $hp, $response);
      }
      $job->summary = $job_summary;
      $jobs[$job_id] = $job;
    }

    return $jobs;
  }

  /**
   * Loads job-independent errors. Must call loadJobIDs first.
   *
   * To test:
   *  - run_local_test.sh simple
   *  - edit /data/bistro/prefs/simple
   *  - change one of the "limit" values in "resources" to a string
   *  - wait < 15 seconds and reload
   * Be sure to click the upper-right-corner '+' buttons.
   */
  public function loadErrors() {
    // Just because errors are always packaged with the main request
    $this->loadResponses($this->getJobIDs());
    $errors = new BistroErrors();
    foreach ($this->hostportToResponse as $hp => $response) {
      foreach (adx($response, 'errors') as $type => $error) {
        $errors->add($error, $hp.' - '.$type);
      }
    }
    return $errors;
  }

  public function loadNodeNames() {
    $this->loadResponses($this->getJobIDs());
    $hp_to_nodes = array();
    foreach ($this->hostportToResponse as $hp => $response) {
      $nodes = array();
      foreach (adx($response, 'nodes') as $level => $sorted_nodes) {
        foreach ($sorted_nodes as $node) {
          $nodes[] = $node;
        }
      }
      $hp_to_nodes[$hp] = $nodes;
    }
    return $hp_to_nodes;
  }

  protected function fetchAllJobIDs() {
    $this->loadResponses(array());
    $all_job_ids = array();
    foreach ($this->hostportToResponse as $ignore_hp => $response) {
      foreach ($response['jobs'] as $job_id => $ignore) {
        $all_job_ids[$job_id] = 1;
      }
    }
    return array_keys($all_job_ids);
  }

  private function loadResponses(array $job_ids) {
    if ($this->hostportToResponse !== null) {
      return;  // already loaded
    }

    // Compose the request
    $request = array(
      'histograms' => array('handler' => 'histogram'),
      'jobs' => array('handler' => 'jobs'),
      'running_tasks' => array('handler' => 'running_tasks'),
      // TODO: Delete once all Bistro deployments have running_tasks,
      // also clean up parse_monitor2_response
      'runtimes' => array('handler' => 'job_node_runtime'),
      'refresh_time' => array('handler' => 'refresh_time'),
      'errors' => array('handler' => 'errors'));
    if ($job_ids) {
      $request['histograms']['jobs'] = $job_ids;
      $request['jobs']['jobs'] = $job_ids;
      $request['running_tasks']['jobs'] = $job_ids;
      // TODO: Delete once all Bistro deployments have running_tasks
      $request['runtimes']['jobs'] = $job_ids;
    }
    $sample_shards =
      $this->prefs->get(BistroJobListPrefs::PREF_SAMPLE_SHARDS);
    if ($sample_shards) {
      $request['histograms']['num_samples'] = $sample_shards;
    }
    if ($this->prefs->get(BistroJobListPrefs::PREF_GET_NODE_NAMES)) {
      $request['nodes'] = array('handler' => 'sorted_node_names');
    }

    $this->hostportToResponse = array();
    foreach ($this->getResponses($this->hostPorts, json_encode($request))
        as $hp => $response_str) {
      list($response, $is_complete) = parse_monitor2_response(
        $request, $hp, $response_str);
      if ($is_complete) {
        $this->hostportToResponse[$hp] = $response;
        $this->instanceHealthChecker->checkForStaleInstances(
          idx($response, 'refresh_time'), $hp);
        if (isset($request['nodes'])) {
          $this->instanceHealthChecker->addNodes($hp, adx($response, 'nodes'));
        }
        $this->instanceHealthChecker->addJobs($hp, adx($response, 'jobs'));
        $this->instanceHealthChecker->addHistograms(
          $hp, adx($response, 'histograms'));  // Includes samples too
        // Checks need the data from the add calls, so they come last.
        $this->instanceHealthChecker->checkRunningTasks(
          $hp, adx($response, 'running_tasks'));
        $this->instanceHealthChecker->checkPerLevelShardCounts($hp);
      }
    }
    $this->instanceHealthChecker->checkNodeCounts();
  }

}
