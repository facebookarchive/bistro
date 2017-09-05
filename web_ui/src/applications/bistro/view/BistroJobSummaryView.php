<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobSummaryView {
  private $hideBars;
  private $jobs;
  private $fetcher;

  public function __construct(
      BistroJobListPrefs $prefs, array $jobs, BistroDataFetcher $fetcher) {
    $this->hideBars =
      $prefs->get(BistroJobListPrefs::PREF_HIDE_RUNNING_TASK_SUMMARIES);
    $this->jobs = $jobs;
    $this->fetcher = $fetcher;
  }

  public function render() {
    return array($this->renderNumRunning(), $this->renderRuntimes());
  }

  private function renderNumRunning() {
    $total_running = 0;
    $data = new BistroJobSummaryBarData();
    foreach ($this->jobs as $job_id => $job) {
      $num_running = count($job->currentRuntimes);
      if (!$num_running) {
        continue;
      }
      $total_running += $num_running;
      $data->addJob($job_id, $num_running);
    }
    $ret = array(hsprintf(
      '<p class="phabricatordefault-p">'.
        '%s running tasks across the %s jobs <b>currently displayed</b> '.
        '(of the %s matching your regex, from a total of %s jobs):'.
      '</p>',
      $total_running,
      count($this->jobs),
      $this->fetcher->getNumFilteredJobs(),
      $this->fetcher->getNumAllJobs()));
    if (!$this->hideBars) {
      $ret[] = bistro_id(new StackedBarChart($data))->render();
    }
    return $ret;
  }

  private function renderRuntimes() {
    if ($this->hideBars) {
      return null;
    }
    $data = new BistroJobSummaryBarData();
    foreach ($this->jobs as $job_id => $job) {
      $runtimes = $job->currentRuntimes;
      if ($runtimes) {
        $data->addJob($job_id, array_sum($runtimes) / count($runtimes));
      }
    }
    return hsprintf(
      '<p class="phabricatordefault-p" style="margin-top:2px">'.
        'Average runtimes of currently running tasks, for '.
        '<b>currently displayed</b> jobs:'.
      '</p>%s',
      bistro_id(new StackedBarChart($data))->render());
  }

}
