<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Prefs shared by the JobList and TaskLog endpoints.
 */
abstract class BistroCommonPrefs extends BistroPrefs {

  const SEC_HOSTPORT = 'How to query Bistro instances?';
  const SEC_TUNE_FETCHING = 'Fine-tune fetching';

  //
  // WARNING: PREF_* names are used as GET params, don't change them lightly.
  //

  // SEC_HOSTPORT
  //
  // While not strictly a hostport preference, it's needed everywhere that
  // uses the hostport preferences:
  //  - task logs use this to determine the transport
  //  - hostport overrides can be transport-specific
  // Really, this should probably be called "transport" but I don't want to
  // break old links.
  const PREF_FETCHER = 'fetcher';
  const PREF_HOSTPORT_SOURCE = 'hostport_source';
  const PREF_HOSTPORT_DATA = 'hostport_data';
  const PREF_OVERRIDE_PORT = 'override_port';
  const PREF_SAMPLE_NUM = 'sample_numerator';
  const PREF_SAMPLE_DENOM = 'sample_denominator';

  // SEC_TUNE_FETCHING
  const PREF_SEND_TIMEOUT_MS = 'send_timeout_ms';
  const PREF_RECV_TIMEOUT_MS = 'recv_timeout_ms';
  const PREF_HOSTPORTS_PER_CURL = 'hostports_per_curl';
  const PREF_MAX_CURL_COUNT = 'max_curl_count';
  // Doesn't really belong in this section, but no value in making one more
  const PREF_STRIP_HOST_SUFFIX = 'strip_host_suffix';

  private $commonSchemata;

  protected function createSectionNamesInOrder() {
    return array(self::SEC_HOSTPORT, self::SEC_TUNE_FETCHING);
  }

  protected function createSchemata() {
    return $this->commonSchemata;
  }

  /**
   * Return only those explicit prefs that are part of the common schema,
   * good for transferring settings from the job list to the log viewer.
   */
  public function getExplicitCommonPrefs() {
    $prefs = array();
    foreach ($this->getExplicitPrefs() as $key => $value) {
      if (isset($this->commonSchemata[$key])) {
        $prefs[$key] = $value;
      }
    }
    return $prefs;
  }

  public function __construct(AphrontRequest $request) {
    $this->commonSchemata = array(
      // SEC_HOSTPORT
      self::PREF_FETCHER => array(
        self::LABEL => 'Transport type',
        self::DEFAULT_VALUE => 'monitor2_http',
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_STR,
        self::CONTROL => array(
          // This is a bit overloaded, since it represents both HTTP vs
          // Thrift and which data fetcher class to use, but that's too
          // much trouble to fix now.
          'bistro_make_loadable_radio_button', array('BistroDataFetcher'))),
      self::PREF_HOSTPORT_SOURCE => array(
        self::LABEL => 'Host:port Source',
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_STR,
        self::CONTROL => array(
          'bistro_make_loadable_radio_button', array('BistroHostPortSource'))),
      self::PREF_HOSTPORT_DATA => array(
        self::LABEL => 'Host:port Data',
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_STR,
        self::CONTROL => array(
          'newv', array('AphrontFormTextAreaControl', array()))),
      self::PREF_SAMPLE_NUM => array(
        self::LABEL => 'Host:ports Hashing To ...',
        self::CAPTION =>
          'Deterministically sample a fraction of host:ports for better '.
          'performance. Change this number to iterate, in chunks, through '.
          'all hostports. Only used when modulo is given.',
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_INT),
      self::PREF_SAMPLE_DENOM => array(
        self::LABEL => '... Modulo',
        self::CAPTION =>
          'Controls the fraction of host:ports that will land in each '.
          'sample; 0 means no sampling.',
        self::DEFAULT_VALUE => 0,
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_INT),
      self::PREF_OVERRIDE_PORT => array(
        self::LABEL => 'Override Port',
        self::CAPTION => 'If specified, replaces the port of every host:port.',
        self::SECTION => self::SEC_HOSTPORT,
        self::TYPE => self::TYPE_INT),

      // SEC_TUNE_FETCHING
      self::PREF_SEND_TIMEOUT_MS => array(
        self::LABEL => 'Send Timeout',
        self::DEFAULT_VALUE => 2000,
        self::SECTION => self::SEC_TUNE_FETCHING,
        self::UNITS => ' ms',
        self::TYPE => self::TYPE_INT),
      self::PREF_RECV_TIMEOUT_MS => array(
        self::LABEL => 'Receive Timeout',
        self::DEFAULT_VALUE => 7000,
        self::SECTION => self::SEC_TUNE_FETCHING,
        self::UNITS => ' ms',
        self::TYPE => self::TYPE_INT),
      self::PREF_MAX_CURL_COUNT => array(
        self::LABEL => 'Max fetching threads',
        self::CAPTION =>
          'Spread the queries over this many threads. DANGER: Too large a '.
          'number will take down your web server.',
        self::DEFAULT_VALUE => 100,
        self::SECTION => self::SEC_TUNE_FETCHING,
        self::TYPE => self::TYPE_INT),
      self::PREF_HOSTPORTS_PER_CURL => array(
        self::LABEL => 'Hostports per thread',
        self::CAPTION =>
          'Each thread prefers to query this many hostports, but may need '.
          'to do more.',
        self::DEFAULT_VALUE => 5,
        self::SECTION => self::SEC_TUNE_FETCHING,
        self::TYPE => self::TYPE_INT),
      self::PREF_STRIP_HOST_SUFFIX => array(
        self::LABEL => 'Strip Host Suffix',
        self::CAPTION => 'Shortens displayed hostnames.',
        self::DEFAULT_VALUE => '',
        self::SECTION => self::SEC_TUNE_FETCHING,
        self::TYPE => self::TYPE_STR));
    parent::__construct($request);
  }

}
