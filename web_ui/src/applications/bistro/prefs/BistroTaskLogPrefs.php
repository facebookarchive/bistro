<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroTaskLogPrefs extends BistroCommonPrefs {

  const SEC_LOGS = 'Which logs do you need?';

  // It's useful to be able to override the hostport prefs. Why? Hostport
  // prefs tell us how to contact all the Bistro instances, and also provide
  // prefs overrides.  But, at times it's useful to talk only to a few
  // specific Bistro instances -- we often know where the logs are.
  const PREF_HOSTPORTS = 'hostports';

  const PREF_SHOW_STDOUT = 'stdout';
  const PREF_SHOW_STDERR = 'stderr';
  const PREF_SHOW_STATUSES = 'statuses';
  const PREF_JOBS = 'jobs';
  const PREF_NODES = 'nodes';
  const PREF_REGEX_FILTER = 'regex_filter';
  const PREF_ASCENDING = 'ascending';
  const PREF_TIME = 'time';
  const PREF_LINE_ID = 'line_id';

  public function getEndpointID() {
    return 'task_logs';
  }

  protected function createSectionNamesInOrder() {
    return array_merge(
      array(self::SEC_LOGS),
      parent::createSectionNamesInOrder());
  }

  protected function createSchemata() {
    return array_merge(
      parent::createSchemata(),
      array(
        self::PREF_HOSTPORTS => array(
          self::LABEL => 'Only get logs from these host:ports',
          self::CAPTION =>
            'Comma-separated; blank for "all hostports from the host:port '.
            'source".',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_SHOW_STDOUT => $this->createBooleanSchema(array(
          self::LABEL => 'Show stdout',
          self::DEFAULT_VALUE => 1,
          self::SECTION => self::SEC_LOGS)),
        self::PREF_SHOW_STDERR => $this->createBooleanSchema(array(
          self::LABEL => 'Show stderr',
          self::DEFAULT_VALUE => 1,
          self::SECTION => self::SEC_LOGS)),
        self::PREF_SHOW_STATUSES => $this->createBooleanSchema(array(
          self::LABEL => 'Show statuses',
          self::DEFAULT_VALUE => 1,
          self::SECTION => self::SEC_LOGS)),
        self::PREF_JOBS => array(
          self::LABEL => 'Fetch job IDs',
          self::CAPTION =>
            'Space-separated or a JSON list; empty for "all jobs".',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_NODES => array(
          self::LABEL => 'Fetch node IDs',
          self::CAPTION =>
            'Space-separated or a JSON list; blank for "all nodes".',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_REGEX_FILTER => array(
          self::LABEL => 'Regex filter',
          self::CAPTION =>
            'Server-side regex filter; may show no lines per page, but '.
            'paging will still work.',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_TIME => array(
          self::LABEL => 'Either timestamp',
          self::CAPTION =>
            'A positive integer means seconds since 1970 UTC. A negative '.
            'integer means "seconds to subtract from the current time". '.
            'All other strings are fed into strtotime(), allowing e,g, '.
            '"1 hour ago" or "Apr 2, 1970". When neither timestamp nor '.
            'line ID is given, start at the earliest (if ascending) or '.
            'latest (if descending) available log line.',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_LINE_ID => array(
          self::LABEL => 'Or line ID',
          self::CAPTION =>
            'An opaque line ID that lets you index into the logs. If '.
            'specified, overrides the timestamp. Useful for paging or for '.
            'linking to a precise point in the logs.',
          self::DEFAULT_VALUE => '',
          self::SECTION => self::SEC_LOGS,
          self::TYPE => self::TYPE_STR),
        self::PREF_ASCENDING => $this->createBooleanSchema(array(
          self::LABEL => 'Ascending by time',
          self::CAPTION =>
            'Fetch lines that are newer than the starting point? '.
            'Note that we always display fetched lines from oldest to newest.',
          self::DEFAULT_VALUE => 0,
          self::SECTION => self::SEC_LOGS))));
  }

}
