<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2JobLevelBar {

  const MIN_BAR_WIDTH = 52;  // to accommodate 'disabled'

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

  const BAR_DONE = 'done';  // done and not avoided
  const BAR_RUNNING = 'running';  // running now
  const BAR_CAN_RUN = 'can run';  // can run now
  const BAR_BACKOFF = 'backoff';  // can run as soon as backoff expires
  const BAR_DISABLED = 'disabled';  // valid or in backoff, but not avoided
  const BAR_AVOIDED = 'avoided';  // even if a prior category also applies

  private static $barDisplayOrder = array(
    self::BAR_RUNNING,
    self::BAR_CAN_RUN,
    self::BAR_BACKOFF,
    self::BAR_DISABLED,
    self::BAR_DONE,
    self::BAR_AVOIDED);

  private static $barTaskDescription = array(
    self::BAR_RUNNING => 'that are running now',
    self::BAR_CAN_RUN =>
      'that may run now, subject to resources and dependencies',
    self::BAR_BACKOFF => 'that are waiting for backoff to expire',
    self::BAR_DISABLED => 'whose nodes are disabled',
    self::BAR_DONE => 'that are done (nodes are not filtered)',
    self::BAR_AVOIDED => 'whose nodes were filtered out by the job');

  // We'll sum all R, G, and B values across all the bits of a bucket, if
  // any value exceeds 1, all 3 are normalized (otherwise left alone).
  // Then, the values are mapped into the range from $barToColorMinMax
  private static $bitsToBlendableRGB = array(
    self::GROUP_HAS_UNSTARTED_TASK => array(0, 0, 1),
    self::GROUP_HAS_RUNNING_TASK => array(1, 1, 0),
    self::GROUP_HAS_DONE_TASK => array(0.25, 1, 0.25),
    self::GROUP_HAS_INCOMPLETE_TASK => array(0.25, 0.5, 0.75),
    self::GROUP_HAS_FAILED_TASK => array(1, 0.25, 0.25),
    self::GROUP_HAS_BACKOFF_TASK => array(0.5, 0.25, 0.75));

  // The RGB values for each sub-bar are normalized to be in these ranges.
  private static $barToColorMinMax = array(
    self::BAR_DONE => array(48, 236),
    self::BAR_RUNNING => array(48, 236),
    self::BAR_CAN_RUN => array(48, 236),
    self::BAR_BACKOFF => array(128, 192),
    self::BAR_DISABLED => array(0, 112),
    self::BAR_AVOIDED => array(200, 255));

  private static $bitsToIcons = array(
    self::GROUP_HAS_RUNNING_TASK => "\xe2\x9a\x99",
    self::GROUP_HAS_FAILED_TASK => "\xe2\x9c\x98",
    self::GROUP_HAS_DONE_TASK => "\xe2\x9c\x93",
    self::GROUP_HAS_INCOMPLETE_TASK => "\xe2\x8b\xae",
    self::GROUP_HAS_BACKOFF_TASK => "\xe2\x97\xb7",
    self::GROUP_HAS_UNSTARTED_TASK => "\xe2\x88\x85");

  private static $bitsToWords = array(
    self::GROUP_HAS_RUNNING_TASK => 'running',
    self::GROUP_HAS_FAILED_TASK => 'failed',
    self::GROUP_HAS_BACKOFF_TASK => 'backoff',
    self::GROUP_HAS_INCOMPLETE_TASK => 'incomplete',
    self::GROUP_HAS_DONE_TASK => 'done',
    self::GROUP_HAS_UNSTARTED_TASK => 'never started');

  private static $taskBitsToWords = array(
    self::TASK_IS_IN_BACKOFF => 'in backoff',
    self::TASK_NODE_DISABLED => 'for disabled nodes',
    self::TASK_JOB_AVOIDS_NODE => 'for filtered nodes');

  // The sort key is the tuple of matching bitmasks, in the order of this
  // list.  You can potentially add or-ed bits to tweak the sort.
  // The code assumes < 256 entries here.
  private static $bitsToSortKey = array(
    self::GROUP_HAS_RUNNING_TASK,
    self::GROUP_HAS_FAILED_TASK,
    self::GROUP_HAS_BACKOFF_TASK,
    self::GROUP_HAS_INCOMPLETE_TASK,
    self::GROUP_HAS_DONE_TASK,
    self::GROUP_HAS_UNSTARTED_TASK);
  const HEXY = 'abcdefghijklmnop';  // these should sort well in all locales

  private
    $cfg,
    $nodeGroup,
    $count = 0,  // for sorting levels
    $barToBitsToCount = array();  // the bars under this level's heading

  public function __construct($cfg, $node_group) {
    $this->cfg = $cfg;
    $this->nodeGroup = $node_group;
  }

  public function getSortKey() {
    return $this->cfg->sortBase - $this->count;
  }

  public function addBits($bar_name, $bits, $count) {
    $this->count += $count;
    // @ because Zend warns about missing indices otherwise.
    @$this->barToBitsToCount[$bar_name][$bits] += $count;
  }

  public function render() {
    require_celerity_resource('bistro-job');

    if (!$this->count) {
      return '';
    }
    list($full_chart_data, $bar_to_bits_to_data) = $this->makeFullChartData();
    // We must render the chart to find out the bar widths for the sectioner
    $full_chart_html =
      bistro_id(new StackedBarChart($full_chart_data))->render();

    return phabricator_tag(
      'div',
      array('class' => $this->cfg->barClass),
      array(
        $this->cfg->renderHeading($this->count),
        $this->renderSectionerBar(
          $bar_to_bits_to_data, $full_chart_data->getTotalWeight()),
        $full_chart_html));
  }

  private function makeFullChartData() {
    require_celerity_resource('bistro-render-node-group-details');
    $HEXY = self::HEXY;  // The Zend parser can't do self::HEXY[$i]
    $data = new StackedBarChartData();
    $data->minBarWidth = self::MIN_BAR_WIDTH;
    $data->totalWidth = $this->cfg->barWidth;
    $data->modifiers = array(
      new DetailsForStackedBarChart(),
      new SimpleNamesForStackedBarChart(),
      new HideZerosForStackedBarChart());
    $data->cssClasses = array('stacked-bar-borders');
    $data->bars = array();
    $bar_to_bits_to_data = array();  // Used to make "sectioner bar" widths
    foreach (self::$barDisplayOrder as $b) {
      $bits_to_count = idx($this->barToBitsToCount, $b);
      if (!$bits_to_count) {
        continue;
      }
      // Sort the bits before rendering
      $bits_to_key = array();
      foreach ($bits_to_count as $bits => $ignore) {
        $key = '';
        foreach (self::$bitsToSortKey as $i => $test_bits) {
          if ($test_bits & $bits) {
            // hex-y encoding not to think about sort locale
            $key .= $HEXY[$i >> 4].$HEXY[$i & 15];
          }
        }
        $bits_to_key[$bits] = $key;
      }
      asort($bits_to_key);
      foreach ($bits_to_key as $bits => $ignore) {
        $count = $bits_to_count[$bits];
        $bar = new StackedBarData();
        $bar->shortName = $this->shortNameForBits($bits);
        $bar->name = $this->longNameForBits($b, $bits);
        list($bar->color, $bar->textColor) = $this->colorsForBits($b, $bits);
        $bar->weight = $count;
        $bar->detailJs = jsprintf(
          'bistroRenderNodeGroupDetails(%s, %s, %s);',
          $this->cfg->jobId,
          $this->nodeGroup->getJSData($this->cfg->levelName, $bits),
          $this->cfg->runsTasks);  // Only show log links for levels with tasks
        $data->bars[] = $bar;
        $bar_to_bits_to_data[$b][$bits] = $bar;
      }
    }
    return array($data, $bar_to_bits_to_data);
  }

  private function renderSectionerBar(
    array $bar_to_bits_to_data, $total_count) {
    $data = new StackedBarChartData();
    $data->forceWeightAsWidth = true;
    $data->bars = array();
    $data->modifiers = array(new HideZerosForStackedBarChart());
    $data->cssClasses = array('sectioner', 'stacked-bar-borders');
    foreach (self::$barDisplayOrder as $b) {
      $bar_count = 0;
      $bar = new StackedBarData();
      foreach (adx($this->barToBitsToCount, $b) as $bits => $count) {
        $bar_count += $count;
        $full_width = $bar_to_bits_to_data[$b][$bits]->width;
        if ($full_width) {
          $bar->weight += $full_width + 2 * StackedBarChart::PADDING_PX;
        }
      }
      $bar->html = $b;
      $bar->title = node_number_format($bar_count).
        ' of '.$total_count.' '.$this->getBarDescription($b);
      $data->bars[] = $bar;
    }
    return bistro_id(new StackedBarChart($data))->render();
  }

  private function getBarDescription($bar_type) {
    return
      $this->cfg->descPrefix.'tasks '.self::$barTaskDescription[$bar_type];
  }

  private function shortNameForBits($bits) {
    $name = '';
    foreach (self::$bitsToIcons as $test_bits => $icon) {
      if ($test_bits & $bits) {
        $name .= $icon;
      }
    }
    return $name;
  }

  private function getWordsForBits($bits_to_words, $bits) {
    $words = array();
    foreach ($bits_to_words as $test_bits => $word) {
      if ($test_bits & $bits) {
        $words[] = $word;
      }
    }
    return $words;
  }

  private function longNameForBits($bar_type, $bits) {
    $words = $this->getWordsForBits(self::$bitsToWords, $bits);
    $res = $this->getBarDescription($bar_type).'. ';
    $res .= 'Last status'.(count($words) == 1 ? '' : 'es').': ';
    $res .= implode(', ', $words).'. ';
    // Usually, the task bit is evident from the bar type, but show any
    // additional bits if they exist.
    $task_words = $this->getWordsForBits(self::$taskBitsToWords, $bits);
    if (count($task_words) > 1) {
      $res .= 'All the tasks are '.implode(', ', $task_words).'. ';
    }
    return $res;
  }

  private function colorsForBits($bar_type, $bits) {
    // Blend colors for all the bits
    $r = 0;
    $g = 0;
    $b = 0;
    foreach (self::$bitsToBlendableRGB as $test_bits => $bits_rgb) {
      list($bits_r, $bits_g, $bits_b) = $bits_rgb;
      if ($bits & $test_bits) {
        $r += $bits_r;
        $g += $bits_g;
        $b += $bits_b;
      }
    }
    // Normalize colors (but keep darks dark)
    $max = max($r, $g, $b);
    if ($max > 1) {
      $r /= $max;
      $g /= $max;
      $b /= $max;
    }

    // Squash colors into the range allowed for each bar type
    list($cmin, $cmax) = self::$barToColorMinMax[$bar_type];
    $crange = $cmax - $cmin;
    $r = $cmin + $crange * $r;
    $g = $cmin + $crange * $g;
    $b = $cmin + $crange * $b;

    return array(
      sprintf('#%02x%02x%02x', $r, $g, $b),
      // Text is black or off-white depending on the background
      ($r * 299 + $g * 587 + $b * 114) / 1000 > 127 ? '#000' : '#eee');
  }

}
