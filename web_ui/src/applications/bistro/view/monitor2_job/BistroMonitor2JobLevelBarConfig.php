<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2JobLevelBarConfig {

  // Should never conflict with string segment names
  const ALL = 1;
  const UNSEGMENTED = 2;

  // Used for sort keys, kept below 2^31 for 32-bit PHP's sake
  const BIG_INT = 2000000000;
  const RUNS_TASKS_BONUS = 1000000000;

  public $segmenter;
  public $jobId;
  public $levelName;
  public $runsTasks;  // We only show log links on such levels

  public $labelPrefix;
  public $descPrefix;

  public $sortBase;
  public $barType;

  // These are null until you use cloneForSegment
  public $segment;
  public $barClass;
  public $barWidth;

  private static $widths = array(
    'primary-bar' => 970,
    'secondary-bar' => 955,
    // Sub-bars are indented 30px farther than the normal bars
    'primary-sub-bar' => 940,
    'secondary-sub-bar' => 925);

  // Does not set segment-related variables -- you must use cloneForSegment
  public function __construct($segmenter, $job_id, $level_name, $runs_tasks) {
    $this->segmenter = $segmenter;
    $this->jobId = $job_id;
    $this->levelName = $level_name;
    $this->sortBase = self::BIG_INT;
    $this->runsTasks = $runs_tasks;
    if ($runs_tasks) {
      $this->barType = 'primary';
      $this->labelPrefix = 'Tasks may run on ';
      $this->descPrefix = '';
      $this->sortBase -= self::RUNS_TASKS_BONUS;
    } else {
      $this->barType = 'secondary';
      $this->labelPrefix = 'Grouped into ';
      $this->descPrefix = $level_name.' nodes are parents of ';
    }
  }

  public function cloneForSegment($segment) {
    $c = clone($this);
    $c->segment = $segment;
    $subtype = $this->barType.($segment === self::ALL ? '' : '-sub').'-bar';
    $c->barClass = 'bistro-job-'.$subtype;
    $c->barWidth = self::$widths[$subtype];
    return $c;
  }

  public function renderHeading($count) {
    if ($this->segment === self::ALL) {
      return hsprintf('<p class="phabricatordefault-p">%s<b>%s %s</b> nodes:</p>',
        $this->labelPrefix, $count, $this->levelName);
    } else if ($this->segment === self::UNSEGMENTED) {
      return bistro_id(new BistroErrorView())
        ->setSeverity(BistroErrorView::SEVERITY_ERROR)
        ->setTitle($count.' '.$this->levelName.' nodes could not be segmented')
        ->appendChild(hsprintf('Increase "sample_shards".'))
        ->render();
    } else {
      return hsprintf('<p class="phabricatordefault-p"><b>%s</b> with <b>%s</b> %s nodes:</p>',
        $this->segmenter->render($this->segment), $count, $this->levelName);
    }
  }

}
