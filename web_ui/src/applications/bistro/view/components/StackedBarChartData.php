<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Enables the various StackedBarChart modifiers to transparently pass
 * parameters through to each other.
 *
 * @concrete-extensible
 */
class StackedBarChartData {
  public $totalWidth;  // In pixels; optional, defaults to sum of weights
  public $minBarWidth;  // In pixels; optional, defaults to 1
  public $bars = array(/* StackedBarData */);
  public $cssClasses = array();
  public $forceWeightAsWidth = false;  // Disable smart bar sizing

  // These associate right-to-left, and can alter what bar data are needed
  public $modifiers = array(/* StackedBarChartModifier */);

  public function getTotalWeight() {
    return array_sum(ppull($this->bars, 'weight'));
  }
}
