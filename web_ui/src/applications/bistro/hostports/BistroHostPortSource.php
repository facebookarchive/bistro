<?php
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class BistroHostPortSource extends BistroLoadableByName {

  protected $prefs;

  private $overridePort;
  private $sampleNum;
  private $sampleDenom;

  private $hostports;

  public static function newFrom(BistroCommonPrefs $prefs) {
    $source_name =
      $prefs->getExplicitPref(BistroCommonPrefs::PREF_HOSTPORT_SOURCE);
    if ($source_name === null) {
      throw new Exception('You must set the hostport_source GET param');
    }
    /* HH_FIXME[4105] Type extra args (to be sent through to constructor) */
    return parent::baseNewFrom($source_name, __CLASS__, $prefs);
  }

  // You should never construct this class's children directly.
  //
  // Needs to be public because PHP5 will not otherwise let this class's
  // base (BistroLoadableByName) call this constructor via newv().
  public function __construct(BistroCommonPrefs $prefs) {
    $this->prefs = $prefs;
    $this->initPrefs(array());
    $this->overridePort = $prefs->get(BistroCommonPrefs::PREF_OVERRIDE_PORT);
    $this->sampleNum = $prefs->get(BistroCommonPrefs::PREF_SAMPLE_NUM);
    $this->sampleDenom = $prefs->get(BistroCommonPrefs::PREF_SAMPLE_DENOM);
  }

  abstract protected function parseImpl();

  abstract public static function getTitle();

  // Can be overloaded to provide default overrides on the basis of the
  // hostport source.
  protected function initPrefs(array $default_overrides) {
    $this->prefs->init($default_overrides);
  }

  public function getData() {
    return
      $this->prefs->getExplicitPref(BistroCommonPrefs::PREF_HOSTPORT_DATA);
  }

  public function getSummary() {
    return
      count($this->parse()).' hostports from '.
      // Using static:: to appease HackLang :/
      static::getTitle().' ('.$this->getData().') '.
      (
        $this->sampleDenom
          ? ', sampling hostports that hash to '.$this->sampleNum.' modulo '.
            $this->sampleDenom
          : ''
      ).
      (
        $this->overridePort
          ? ', overriding all ports to '.$this->overridePort
          : ''
      );
  }

  /**
   * Returns an array of 'host:port' strings. Use strings instead of arrays
   * because are they hashable and easy-to-print.  Get the host and port
   * using bistro_split_hostport().
   */
  public function parse() {
    if ($this->hostports) {
      return $this->hostports;
    }

    $hps = $this->parseImpl();

    // Apply hostport preferences, and then dedup hostports by mapping their
    // hostnames into a canonical form.
    $unique_hps = array();
    foreach ($hps as $hp) {
      // If sampling hostports, hash the user-provided strings so that the
      // choice is reproducible and transparent.
      if ($this->sampleDenom &&
          hexdec(substr(md5($hp), 0, 15)) % $this->sampleDenom !==
            $this->sampleNum) {
        continue;
      }
      // Optionally, override the port.
      list($host, $port) = bistro_split_hostport($hp);
      if ($this->overridePort) {
        $port = $this->overridePort;
      }
      // Record the hostport if it's not a duplicate.
      $iphp = bistro_canonical_host_id($host).':'.$port;
      if (isset($unique_hps[$iphp])) {
        bistro_monitor_log()
          ->warn($hp.' is a duplicate of '.$unique_hps[$iphp].', ignoring.');
      } else {
        $unique_hps[$iphp] = $host.':'.$port;
      }
    }

    $this->hostports = array_values($unique_hps);
    return $this->hostports;
  }

}
