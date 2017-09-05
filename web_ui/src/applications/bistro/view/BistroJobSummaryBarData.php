<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobSummaryBarData extends StackedBarChartData {

  const TOTAL_BAR_WIDTH = 1000;
  const MIN_BAR_WIDTH = 35;
  const MAX_SHORT_NAME_CHARS = 4;

  public function __construct() {
    $this->minBarWidth = self::MIN_BAR_WIDTH;
    $this->totalWidth = self::TOTAL_BAR_WIDTH;
    $this->modifiers = array(new SimpleNamesForStackedBarChart());
    $this->cssClasses = array('stacked-bar-borders');
  }

  public function addJob($job_id, $weight) {
    $bar = new StackedBarData();
    $bar->weight = $weight;
    $bar->shortName = $job_id;
    $bar->name = $job_id;
    $bar->color = "white";

    $this->bars[] = $bar;
  }

}
