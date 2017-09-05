<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2JobLevel {

  private $nodeGroups;
  private $allBar;
  private $unsegmentedBar;  // not null iff this level is segmented
  private $segmentToBar = array();

  private $baseCfg;

  public function __construct(
      $segmenter, $job_id, $level_name, $runs_tasks, $node_groups) {
    $this->nodeGroups = $node_groups;
    $this->baseCfg = new BistroMonitor2JobLevelBarConfig(
        $segmenter, $job_id, $level_name, $runs_tasks);
    $this->allBar = new BistroMonitor2JobLevelBar(
      $this->baseCfg->cloneForSegment(BistroMonitor2JobLevelBarConfig::ALL),
      $node_groups->allGroup);
    if ($segmenter) {
      $this->unsegmentedBar = new BistroMonitor2JobLevelBar(
        $this->baseCfg->cloneForSegment(
          BistroMonitor2JobLevelBarConfig::UNSEGMENTED),
        $node_groups->unsegmentedGroup);
    }
  }

  public function addBits(
      $bits, $count, $unsegmented_count, $segment_to_count) {
    $bar_name = $this->bitsToBar($bits);
    $this->allBar->addBits($bar_name, $bits, $count);
    if ($this->unsegmentedBar) {
      $this->unsegmentedBar->addBits($bar_name, $bits, $unsegmented_count);
      foreach ($segment_to_count as $segment => $segment_count) {
        $bar = idx($this->segmentToBar, $segment);
        if (!$bar) {
          $bar = new BistroMonitor2JobLevelBar(
            $this->baseCfg->cloneForSegment($segment),
            $this->nodeGroups->segmentToGroup[$segment]);
          $this->segmentToBar[$segment] = $bar;
        }
        $bar->addBits($bar_name, $bits, $segment_count);
      }
    }
  }

  public function render() {
    $res = array($this->allBar->render());
    if ($this->unsegmentedBar) {
      $res[] = $this->unsegmentedBar->render();
    }
    if ($this->segmentToBar) {
      $res[] = phabricator_tag(
        'div',
        // Ensures the label shows up with the right indentation
        array('class' => $this->baseCfg->cloneForSegment(
          BistroMonitor2JobLevelBarConfig::UNSEGMENTED)->barClass),
        hsprintf('Nodes split into <b>%s segments</b>:',
          count($this->segmentToBar)));
      krsort($this->segmentToBar);
      foreach ($this->segmentToBar as $bar) {
        $res[] = $bar->render();
      }
    }
    return $res;
  }

  public function getSortKey() {
    // TODO: Bistro should return sorted levels, but this heuristic is okay.
    return $this->allBar->getSortKey();
  }

  // This is outside BistroMonitor2JobLevelBar as a micro-optimization.
  private function bitsToBar($bits) {
    if ($bits & BistroMonitor2JobLevelBar::TASK_JOB_AVOIDS_NODE) {
      return BistroMonitor2JobLevelBar::BAR_AVOIDED;
    }
    // For some usecases, it's helpful to distinguish between "done but
    // disabled" and "done and enabled".  For example, if you are using a
    // cron-style nodefetcher that disables nodes after a certain age (but
    // leaves them in for easy monitoring), then it's great to have these
    // two be separable, so we check DISABLED before DONE.
    if ($bits & BistroMonitor2JobLevelBar::TASK_NODE_DISABLED) {
      return BistroMonitor2JobLevelBar::BAR_DISABLED;
    }
    if (($bits & BistroMonitor2JobLevelBar::GROUP_BITS) ===
        BistroMonitor2JobLevelBar::GROUP_HAS_DONE_TASK) {
      return BistroMonitor2JobLevelBar::BAR_DONE;
    }
    if ($bits & BistroMonitor2JobLevelBar::TASK_IS_IN_BACKOFF) {
      return BistroMonitor2JobLevelBar::BAR_BACKOFF;
    }
    if ($bits & BistroMonitor2JobLevelBar::GROUP_HAS_RUNNING_TASK) {
      return BistroMonitor2JobLevelBar::BAR_RUNNING;
    }
    return BistroMonitor2JobLevelBar::BAR_CAN_RUN;
  }

}
