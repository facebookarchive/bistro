<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2NodeGroup {

  private $levelToBitsToHostportToSamples = array();
  private $levelToBitsToHostportToCount = array();
  private $levelToBitsToRunningTasks = array();

  public function addHostportLevelBits(
      $hp, $level, $bits, $count, array $samples, array $all_running_tasks) {
    foreach (array_select_keys($all_running_tasks, $samples)
             as $node => $task) {
      array_set_once(
        $this->levelToBitsToRunningTasks[$level][$bits], $node, $task);
    }
    array_set_once(
      $this->levelToBitsToHostportToSamples[$level][$bits], $hp, $samples);
    array_set_once(
      $this->levelToBitsToHostportToCount[$level][$bits], $hp, $count);
  }

  public function getJSData($level, $bits) {
    $hp_to_nodes =
      adx(adx($this->levelToBitsToHostportToSamples, $level), $bits);
    $samples = array();
    foreach (adx(adx($this->levelToBitsToHostportToCount, $level), $bits)
             as $hp => $num_nodes) {
      $nodes = adx($hp_to_nodes, $hp);
      // See node_number_format() for an explanation of the round().
      array_unshift($nodes, round($num_nodes, 4));
      $samples[$hp] = $nodes;
    }
    return json_encode(array_filter(array(
      'hp_to_count_and_nodes' => $samples,
      'running_tasks' =>
        adx(adx($this->levelToBitsToRunningTasks, $level), $bits))));
  }

}
