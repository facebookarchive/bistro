<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2JobSummary extends BistroJobResponseView {
  // These constants are copied from HistogramHandler in Bistro
  //
  // GROUP_* bits are properties of a group of nodes -- they are ORed
  // together over many nodes.
  const GROUP_HAS_UNSTARTED_TASK = 0x1;
  const GROUP_HAS_RUNNING_TASK = 0x2;
  const GROUP_HAS_DONE_TASK = 0x4;
  const GROUP_HAS_INCOMPLETE_TASK = 0x8;
  const GROUP_HAS_FAILED_TASK = 0x10;
  const GROUP_HAS_BACKOFF_TASK = 0x20;
  const GROUP_BITS = 0x3f;  // OR of the above
  // TASK_* bits are like histogram buckets, in that your node will get
  // counted with other nodes that have the exact same set of attributes.
  const TASK_IS_IN_BACKOFF = 0x40;
  const TASK_JOB_AVOIDS_NODE = 0x80;
  const TASK_NODE_DISABLED = 0x100;
  // Use the Base64-URL alphabet because it survives JSON and URLs unescaped.
  const CODE =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

  // Reverse map for CODE
  private static $letterToBitsMap = array();

  public static function letterToBits(/*string*/ $letter) {
    if (!self::$letterToBitsMap) {  // Initialize the bit-decoding map
      foreach (str_split(self::CODE) as $i => $c) {
        self::$letterToBitsMap[$c] = $i;
      }
    }
    return self::$letterToBitsMap[$letter];
  }

  public function render(BistroJobListPrefs $prefs) {
    require_celerity_resource('bistro-job');
    $level_for_tasks = $this->job->level->getConsensus();
    $levels = array();
    $levels_to_hide = array_fill_keys(
      json_decode($prefs->get(BistroJobListPrefs::PREF_HIDE_LEVELS), 1), 1);

    $node_groups = new BistroMonitor2NodeGroups();

    $job_id = $this->job->jobId;
    foreach ($this->hostportToResponse as $hp => $response) {
      $level_to_encbits_to_samples =
        adx(adx(adx($response, 'histograms'), 'samples'), $job_id);
      $node_to_running_tasks = adx(adx($response, 'running_tasks'), $job_id);
      foreach (adx(adx(adx($response, 'histograms'), 'histograms'), $job_id)
               as $level_name => $histogram) {
        if (isset($levels_to_hide[$level_name])) {
          continue;
        }
        $segmenter = BistroLevelSegmenter::get($prefs, $level_name);
        if (!isset($levels[$level_name])) {
          $levels[$level_name] = new BistroMonitor2JobLevel(
            $segmenter, $job_id, $level_name,
            $level_for_tasks == $level_name, $node_groups
          );
        }
        $encbits_to_samples = adx($level_to_encbits_to_samples, $level_name);
        $level = $levels[$level_name];
        foreach ($histogram as $encoded => $count) {
          $bits = bistro_decode_status_bits($encoded);
          $all_samples = adx($encbits_to_samples, $encoded);
          // Populate the NodeGroups
          $node_groups->allGroup->addHostportLevelBits(
            $hp, $level_name, $bits, $count, $all_samples,
            $node_to_running_tasks);
          // Segment this histogram bucket's samples
          $unsegmented_count = $count;
          $segment_to_samples = array();
          if ($segmenter) {
            $unsegmented_samples = array();
            foreach ($all_samples as $sample) {
              $segment = $segmenter->getSegment($sample);
              if ($segment !== null) {
                $segment_to_samples[$segment][] = $sample;
                --$unsegmented_count;
              } else {
                $unsegmented_samples[] = $sample;
              }
            }
            $node_groups->unsegmentedGroup->addHostportLevelBits(
              $hp, $level_name, $bits,
              $unsegmented_count, $unsegmented_samples,
              $node_to_running_tasks);
            foreach ($segment_to_samples as $segment => $segment_samples) {
              $g = idx($node_groups->segmentToGroup, $segment);
              if (!$g) {
                $g = new BistroMonitor2NodeGroup();
                $node_groups->segmentToGroup[$segment] = $g;
              }
              $g->addHostportLevelBits(
                $hp, $level_name, $bits,
                count($segment_samples), $segment_samples,
                $node_to_running_tasks);
            }
          }
          // This depends on the NodeGroups being populated
          $level->addBits($bits, $count,
            $unsegmented_count, array_map(fun('count'), $segment_to_samples));
        }
      }
    }

    return array(
      $this->makeChild('BistroMonitor2JobErrors')->render(),
      mpull(msort($levels, 'getSortKey'), 'render'));
  }
}

