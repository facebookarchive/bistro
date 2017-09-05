<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroLevelSegmenter {
  public $regex;
  public $render;

  public static function get($prefs, $level_name) {
    $level_prefs = adx(
      $prefs->getJSONArray(BistroJobListPrefs::PREF_SEGMENT_NODES),
      $level_name);
    $regex = idx($level_prefs, 'pcre');
    if (!is_string($regex) || !$regex) {
      if ($level_prefs !== array()) {
        bistro_monitor_log()->warn(
          BistroJobListPrefs::PREF_SEGMENT_NODES.' key "pcre" for level "'.
          $level_name.'" must be a Perl regex with one subpattern');
      }
      return null;
    }
    $s = new BistroLevelSegmenter();
    $s->regex = $regex;
    $s->render = idx($level_prefs, 'render', 'literal');
    return $s;
  }

  public function getSegment($sample) {
    $matches = array();
    if (preg_match($this->regex, $sample, $matches) &&
        isset($matches[1])) {
      return $matches[1];
    }
    return null;
  }

  public function render($segment) {
    if ($this->render === 'timestamp') {
      return strftime('%c', $segment);
    }
    return $segment;
  }

}
