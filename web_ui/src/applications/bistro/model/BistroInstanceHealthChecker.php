<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * This class is a grab-bag of sanity checks. Here's a summary:
 *  - Are any queried Bistro instances stale (one of their core loops has
 *    not run in a while)?
 *  - Are any tasks running on jobs or shards that can't have running tasks?
 *
 * TODOs:
 *  - data completeness check
 *    - inconsistency in any salient job dimension
 *    - ...
 *  - node count check
 *  - ...
 */
final class BistroInstanceHealthChecker {

  private $prefs;
  private $hpToLevelToNodes = array();
  private $hpToJobs = array();
  private $hpToHistAndSamples = array();

  public function __construct(BistroJobListPrefs $prefs) {
    $this->prefs = $prefs;
  }

  public function checkForStaleInstances($refresh_time, $hp) {
    $cur_time = time();
    $error_staleness =
      $this->prefs->get(BistroJobListPrefs::PREF_ERR_STALENESS);
    $warn_staleness = min(
      $error_staleness,
      $this->prefs->get(BistroJobListPrefs::PREF_WARN_STALENESS));

    $staleness = $cur_time - $refresh_time;
    if ($staleness > $warn_staleness) {
      if ($staleness < $error_staleness) {
        bistro_monitor_log()->warn(
          'Refresh has not happened for '.$staleness.' > '.$warn_staleness.
          ' (GET param "warn_staleness") seconds on '.$hp);
      } else {
        bistro_monitor_log()->error(
          'Refresh has not happened for '.$staleness.' > '.$error_staleness.
          ' (GET param "staleness") seconds on '.$hp);
      }
    }
  }

  // TODO: Move duplicate-node checking in here?

  public function addNodes($hp, array $nodes_by_level) {
    array_set_once($this->hpToLevelToNodes, $hp, $nodes_by_level);
  }

  public function addJobs($hp, array $jobs) {
    array_set_once($this->hpToJobs, $hp, $jobs);
  }

  public function addHistograms($hp, array $hist_and_samples) {
    array_set_once($this->hpToHistAndSamples, $hp, $hist_and_samples);
  }

  // Get and validate the allowable node counts for each level
  private function getLevelToRangePref() {
    $level_to_range =
      $this->prefs->getJSONArray(BistroJobListPrefs::PREF_NODE_COUNT_RANGES);
    foreach ($level_to_range as $level => $r) {
      if (is_int($r)) {  // Treat a single integer as a one-element range
        $r =  array($r, $r);
      }
      if (!is_array($r) || count($r) !== 2 ||
          !is_int($r[0]) || !is_int($r[1])) {
        bistro_monitor_log()->warn(
          'Level '.$level.' must have a pair of integers as its range');
        $level_to_range[$level] = $r;
        return;
      }
      $level_to_range[$level] = $r;
    }
    return $level_to_range;
  }

  public function checkNodeCounts() {
    $level_to_range = $this->getLevelToRangePref();
    // Tally up the nodes by level
    $level_to_count = array();
    foreach ($this->hpToLevelToNodes as $ignore_hp => $level_to_nodes) {
      foreach ($level_to_nodes as $level => $sorted_nodes) {
        $n = count($sorted_nodes);
        @$level_to_count[$level] += $n;  // Silence the silly Zend warning
      }
    }
    // Compare the level counts against the ranges
    foreach ($level_to_range as $level => $range) {
      list($min_nodes, $max_nodes) = $range;
      $num = idx($level_to_count, $level);
      if ($num === null) {
        bistro_monitor_log()->warn(
          'Unknown node count range level '.$level.'. Or, maybe node '.
          'fetching is turned off (GET param get_node_names)?');
        return;
      }
      if ($num < $min_nodes || $num > $max_nodes) {
        bistro_monitor_log()->error(
          'Level '.$level.' has '.$num.' nodes, outside of the allowable '.
          'range of '.$min_nodes.' ... '.$max_nodes);
      }
    }
  }

  /**
   * Detect tasks running with jobs or shards that shouldn't run anything.
   */
  public function checkRunningTasks($hp, array $running_tasks) {
    $orphan_labels = array_filter(array(  // At least one is nonempty
      $this->getJobAndDeletedNodeOrphans($hp, $running_tasks),
      $this->getDisabledAvoidedBackoffNodeOrphans($hp)));
    if ($orphan_labels) {
      bistro_monitor_log()->warn(array(
        'Running tasks that should not be running:',
        phabricator_tag('ul', array('class' => 'bistro-log-list'), $orphan_labels)));
    }
    // TODO: Maybe emptying all state here would improve performance.  Or
    // maybe it wouldn't...
  }

  private function getHostportNodes($hp) {
    $nodes = array();
    foreach (adx($this->hpToLevelToNodes, $hp) as $level => $sorted_nodes) {
      foreach ($sorted_nodes as $node) {
        $nodes[$node] = 1;
      }
    }
    if ($this->prefs->get(BistroJobListPrefs::PREF_GET_NODE_NAMES)
        && count($nodes) === 0) {
      bistro_monitor_log()->warn($hp.' has no nodes.');
    }
    return $nodes;
  }

  /**
   * Detect running tasks from deleted, invalid or disabled jobs, and
   * deleted nodes. This is messier than doing jobs and nodes separately
   * but makes cleaner output.
   */
  private function getJobAndDeletedNodeOrphans($hp, array $running_tasks) {
    if (!isset($this->hpToLevelToNodes[$hp])) {
      bistro_monitor_log()->warn(
        'Cannot detect running tasks belonging to a deleted shard '.
        'because node names were not fetched (GET param get_node_names)');
    }
    $jobs = $this->hpToJobs[$hp];
    $nodes = $this->getHostportNodes($hp);

    $orphan_labels = array();
    foreach ($running_tasks as $job_id => $job_running_tasks) {
      foreach ($job_running_tasks as $node => $running_task) {
        $labels = array();
        $job = idx($jobs, $job_id);
        if (!$job) {
          $labels[] = 'job deleted';
        } else if (idx($job, 'why_invalid') !== null) {
          $labels[] = 'job invalid';
        } else if (idx($job, 'enabled') !== true) {
          $labels[] = 'job disabled';
        }
        if ($nodes && !isset($nodes[$node])) {
          $labels[] = 'node deleted';
        }
        if ($labels) {
          $orphan_labels[] = phabricator_tag('li', array(),
            $job_id.' on '.$node.' with '.json_encode($running_task).': '.
            implode(', ', $labels));
        }
      }
    }
    return $orphan_labels;
  }

  /**
   * Determine if tasks are running on any nodes that are avoided or
   * disabled or in backoff.
   */
  private function getDisabledAvoidedBackoffNodeOrphans($hp) {
    $jobs = $this->hpToJobs[$hp];
    $hist_and_samples = $this->hpToHistAndSamples[$hp];
    $orphan_labels = array();
    // This loop is slightly redundant with BistroMonitor2JobSummary, but
    // it's way cleaner to keep them separate.
    foreach (adx($hist_and_samples, 'histograms') as $job_id => $level_hist) {
      foreach ($level_hist as $level => $histogram) {
        // adx/idx in case jobs and histograms are transiently out of sync.
        if ($level !== idx(adx($jobs, $job_id), 'level')) {
          continue;  // Only task histograms are useful.
        }
        foreach ($histogram as $encoded => $count) {
          $bits = bistro_decode_status_bits($encoded);
          if (!($bits & BistroMonitor2JobLevelBar::GROUP_HAS_RUNNING_TASK)) {
            continue;  // Only check groups of tasks that are running
          }
          $labels = array();
          if ($bits & BistroMonitor2JobLevelBar::TASK_IS_IN_BACKOFF) {
            $labels[] = 'task in backoff';
          }
          if ($bits & BistroMonitor2JobLevelBar::TASK_JOB_AVOIDS_NODE) {
            $labels[] = 'job avoids node';
          }
          if ($bits & BistroMonitor2JobLevelBar::TASK_NODE_DISABLED) {
            $labels[] = 'node disabled';
          }
          if ($labels) {
            $samples = adx(adx(adx(adx(
              $hist_and_samples, 'samples'), $job_id), $level), $encoded);
            if ($samples) {
              $samples_str = ' ('.implode(', ', $samples).')';
            } else {
              $samples_str = '';
            }
            $orphan_labels[] = phabricator_tag('li', array(),
              $job_id.' on '.$count.' nodes'.$samples_str.': '.
              implode(', ', $labels));
          }
        }
      }
    }
    return $orphan_labels;
  }

  public function checkPerLevelShardCounts($hp) {
    if (!isset($this->hpToLevelToNodes[$hp])) {
      bistro_monitor_log()->warn(
        'Cannot double-check histogram per-level counts because node names '.
        'were not fetched (GET param get_node_names)');
      return;
    }
    $jobs = $this->hpToJobs[$hp];
    // An easy way of counting the nodes per level
    $level_to_count = array_map(
      fun('count'), idx($this->hpToLevelToNodes, $hp, array()));
    // Summing per-level histograms should yield the same result for any job
    foreach (adx($this->hpToHistAndSamples[$hp], 'histograms')
             as $job_id => $level_hist) {
      // Conveniently, this is null when the job is invalid, so both counts
      // default to 0, and no warning is raised.
      $task_level = idx(adx($jobs, $job_id), 'level');
      // TODO: As soon as possible, check all level counts -- right now the
      // backend sends broken data when a node has no task-level
      // descendants:
      //   $level_to_count === array_map(fun('array_sum'), $level_hist)
      $task_count = idx($level_to_count, $task_level, 0);
      $hist_task_count = array_sum(idx($level_hist, $task_level, array()));
      if ($task_count !== $hist_task_count) {
        bistro_monitor_log()->error($job_id.' inconsistent on '.$hp.': '.
          $task_count.' !=='.$hist_task_count);
      }
    }
  }

}
