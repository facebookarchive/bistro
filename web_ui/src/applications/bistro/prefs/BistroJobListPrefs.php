<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroJobListPrefs extends BistroCommonPrefs {

  // Using human-readable titles might be slightly less efficient, but it's
  // a pretty good guarantee we won't get key clashes between parents and
  // children.
  const SEC_FETCHER = 'What to fetch?';
  const SEC_VALIDATION = 'Validation';
  const SEC_PAGINATION = 'Pagination';
  const SEC_TWEAKS = 'Tweaks';

  //
  // WARNING: PREF_* names are used as GET params, don't change them lightly.
  //

  // Fetcher preferences
  const PREF_JOBS = 'jobs';
  const PREF_DEPRECATED_JOB_IDS = 'job_ids';
  const PREF_JOB_ID_REGEX = 'job_id_regex';
  const PREF_SAMPLE_SHARDS = 'sample_shards';
  const PREF_SEGMENT_NODES = 'segment_nodes';
  const PREF_SORT_BY = 'sort';

  // Pagination
  const PREF_PAGE_OFFSET = 'cur_page';
  const PREF_PAGE_SIZE = 'page_size';

  // Validation
  const PREF_MAX_RUNTIME = 'max_runtime';  // Warn above this many seconds
  // Error when last refresh was more than this long ago
  const PREF_ERR_STALENESS = 'error_staleness';
  const PREF_WARN_STALENESS = 'warn_staleness';  // Warn for this lower value
  // Warns if the nodes at some level don't fall in a given range
  const PREF_NODE_COUNT_RANGES = 'node_count_ranges';

  // Tweaks
  const PREF_GET_NODE_NAMES = 'get_node_names';
  const PREF_HIDE_LEVELS = 'hide_levels';
  const PREF_HIDE_RUNNING_TASK_SUMMARIES = 'hide_running_task_summaries';
  const PREF_JOB_ACTIONS = 'job_actions';
  const PREF_DEFAULT_JOB_ID_REGEX = 'default_job_id_regex';

  // Define all possible sort orders. This is janky because these strings
  // can end up in URLs, but if the sort order field breaks due to changing
  // options, it's not a huge deal.

  const SORT_RUNNING_TASKS_FIRST = 'Most running tasks first';

  const SORT_ENABLED_FIRST = 'Enabled first';

  const SORT_NAME_ASCENDING = 'Ascending by name';
  const SORT_NAME_DESCENDING = 'Descending by name';
  const SORT_RECENTLY_MODIFIED_FIRST = 'Recently modified first';
  const SORT_RECENTLY_CREATED_FIRST = 'Recently created first';
  const SORT_RECENTLY_MODIFIED_LAST = 'Recently modified last';
  const SORT_RECENTLY_CREATED_LAST = 'Recently created last';

  static public $mainSortOrders = array(
    self::SORT_NAME_ASCENDING,
    self::SORT_NAME_DESCENDING,
    self::SORT_RECENTLY_MODIFIED_FIRST,
    self::SORT_RECENTLY_MODIFIED_LAST,
    self::SORT_RECENTLY_CREATED_FIRST,
    self::SORT_RECENTLY_CREATED_LAST);

  public function getEndpointID() {
    return 'job_list';
  }

  protected function createSectionNamesInOrder() {
    return array_merge(
      parent::createSectionNamesInOrder(),
      array(
        self::SEC_FETCHER,
        self::SEC_VALIDATION,
        self::SEC_PAGINATION,
        self::SEC_TWEAKS));
  }

  // The resulting strings are parsed by getJobsInRenderOrder()
  private function makeSortOrders() {
    $ret = array();
    foreach (array(self::SORT_RUNNING_TASKS_FIRST, null) as $running_sort) {
      foreach (array(self::SORT_ENABLED_FIRST, null) as $enabled_sort) {
        foreach (self::$mainSortOrders as $main_sort) {
          $ret[] = implode(', ', array_filter(array(
            $running_sort, $enabled_sort, $main_sort)));
        }
      }
    }
    return $ret;
  }

  protected function createSchemata() {
    return array_merge(
      parent::createSchemata(),
      array(
        // SEC_FETCHER
        self::PREF_JOBS => array(
          self::LABEL => 'Only fetch job IDs:',
          self::CAPTION =>
            'JSON list or space-separated; blank for "all jobs".',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_STR),
        self::PREF_DEPRECATED_JOB_IDS => array(  // Use JOBS instead
          // IMPORTANT: I extend get() and below to use JOB_IDS as a
          // fallback for JOBS.  Delete that code when deleting JOB_IDS.
          self::LABEL => 'DEPRECATED: Only fetch job IDs:',
          self::CAPTION =>
            'DEPRECATED: Comma-separated; blank for "all jobs".',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_STR,
          // Hide the query form control for this deprecated field.
          self::CONTROL => array('bistro_make_hidden_text_control', array())),
        self::PREF_JOB_ID_REGEX => array(
          self::LABEL => 'Regex for job IDs',
          self::CAPTION => 'Use ".*" for "all jobs". If blank, falls back '.
          'to the "'.self::PREF_DEFAULT_JOB_ID_REGEX.'" pref.',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_STR),
        self::PREF_SAMPLE_SHARDS => array(
          self::LABEL => 'Sample Node Names',
          self::DEFAULT_VALUE => 2,
          self::UNITS => ' nodes',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_INT),
        self::PREF_SEGMENT_NODES => array(
          self::LABEL => 'Segment Node Names',
          self::DEFAULT_VALUE => '{}',
          self::CAPTION =>
            'Sample usage: {"level_name": {"pcre": "/_(.[0-9])$/", '.
            '"render": "timestamp"}}. Segment nodes of certain levels '.
            'using a parenthesized subpattern of a PCRE regex. Each '.
            'segment is shown as a separate bar. Only works if '.
            'sample_shards is high enough.',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_STR),
        self::PREF_SORT_BY => $this->createDropdownSchema(array(
          self::LABEL => 'Sort by',
          self::CAPTION =>
            // TODO: Make paging work better.
            'Only sorts the jobs that were fetched on the current page -- '.
            'does not page jobs in the order requested.',
          self::SECTION => self::SEC_FETCHER,
          self::TYPE => self::TYPE_STR,
          self::ALLOWED_VALUES => $this->makeSortOrders())),

        // SEC_VALIDATION
        self::PREF_MAX_RUNTIME => array(
          self::LABEL => 'Max Task Runtime',
          self::DEFAULT_VALUE => 3600,
          self::UNITS => ' seconds',
          self::SECTION => self::SEC_VALIDATION,
          self::TYPE => self::TYPE_INT),
        self::PREF_WARN_STALENESS => array(
          self::LABEL => 'Warning Instance Staleness',
          self::DEFAULT_VALUE => 600,
          self::UNITS => ' seconds',
          self::SECTION => self::SEC_VALIDATION,
          self::TYPE => self::TYPE_INT),
        self::PREF_ERR_STALENESS => array(
          self::LABEL => 'Error Instance Staleness',
          self::DEFAULT_VALUE => 900,
          self::UNITS => ' seconds',
          self::SECTION => self::SEC_VALIDATION,
          self::TYPE => self::TYPE_INT),
        self::PREF_NODE_COUNT_RANGES => array(
          self::LABEL => 'Per-Level Node Count Ranges',
          self::CAPTION =>
            'A JSON object of {"level_name": [min, max number of nodes]}.',
          self::DEFAULT_VALUE => '{}',
          self::SECTION => self::SEC_VALIDATION,
          self::TYPE => self::TYPE_STR),

        // SEC_PAGINATION
        self::PREF_PAGE_SIZE => array(
          self::LABEL => 'Jobs Per Page',
          self::DEFAULT_VALUE => 250,
          self::UNITS => ' jobs',
          self::SECTION => self::SEC_PAGINATION,
          self::TYPE => self::TYPE_INT),
        self::PREF_PAGE_OFFSET => array(
          self::LABEL => 'Page #',
          self::CAPTION => '0-base index of the job to show.',
          self::DEFAULT_VALUE => 0,
          self::SECTION => self::SEC_PAGINATION,
          self::TYPE => self::TYPE_INT),

        // SEC_TWEAKS
        self::PREF_GET_NODE_NAMES => $this->createBooleanSchema(array(
          self::LABEL => 'Get node names',
          self::CAPTION =>
            'A nonzero value makes us fetch node names, which are essential '.
            'for several sanity checks, but cause some perf overhead.',
          self::DEFAULT_VALUE => 1,
          self::SECTION => self::SEC_TWEAKS)),
        self::PREF_HIDE_LEVELS => array(
          self::LABEL => 'Hide levels',
          self::CAPTION => 'A JSON array of ["level to hide", ...].',
          self::DEFAULT_VALUE => '[]',
          self::SECTION => self::SEC_TWEAKS,
          self::TYPE => self::TYPE_STR),
        self::PREF_HIDE_RUNNING_TASK_SUMMARIES => $this->createBooleanSchema(
          array(
            self::LABEL => 'Hide running task summaries',
            self::CAPTION =>
              'If you have many jobs with few tasks, these summary bars are '.
              'not very informative',
            self::DEFAULT_VALUE => 0,
            self::SECTION => self::SEC_TWEAKS)),
        self::PREF_JOB_ACTIONS => array(
          self::LABEL => 'Job Action Menu',
          self::CAPTION =>
            'Custom entires for the [Job Actions] menu. A JSON array of '.
            '[{"name": "Foo", "url": "https://bar/{{job_id}}/view", '.
            '"type": "iframe or link"}, ...]',
          self::DEFAULT_VALUE => '[]',
          self::SECTION => self::SEC_TWEAKS,
          self::TYPE => self::TYPE_STR),
        self::PREF_DEFAULT_JOB_ID_REGEX => array(
          self::LABEL => 'Default job ID regex',
          self::CAPTION => 'Replaces pref "'.self::PREF_JOB_ID_REGEX.'" if '.
            'that is left blank. In this default, the magic string '.
            '%%%PHABRICATOR_USERNAME%%% is replaced by the current username.',
          self::DEFAULT_VALUE => '.*',
          self::SECTION => self::SEC_TWEAKS,
          self::TYPE => self::TYPE_STR)));
  }

  // Future: Delete this when JOB_IDS is gone.
  public function get($pref_key) {
    $val = parent::get($pref_key);
    if ($pref_key != self::PREF_JOBS) {
      return $val;
    }
    if ($pref_key == self::PREF_DEPRECATED_JOB_IDS) {
      throw new Exception('Internal bug: do not use PREF_DEPRECATED_JOB_IDS');
    }
    // JOBS can be a proxy for PREF_DEPRECATED_JOB_IDS.
    $job_ids = parent::get(self::PREF_DEPRECATED_JOB_IDS);
    if (!empty($job_ids) && !empty($val)) {
      bistro_monitor_log()->warn(
        'Both "'.self::PREF_DEPRECATED_JOB_IDS.'" and "'.self::PREF_JOBS.
        '" set for this query, ignoring the deprecated "job_ids" pref.');
    }
    if (!empty($val)) {  // JOBS take precedence over JOB_IDS.
      return $val;
    }
    // Auto-convert from comma-separated JOB_IDS to a JSON jobs format.
    if ($job_ids === '') {  // explode of empty string is broken.
      return '';
    }
    return json_encode(explode(',', $job_ids));
  }

}
