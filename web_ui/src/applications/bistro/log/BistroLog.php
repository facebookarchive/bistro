<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroLog {

  // Omitting critical, because then you should just throw an Exception
  // Omitting info, because then you should just use phlog()
  const ERROR = 1;  // Some server is down
  const WARNING = 2;  // Timeouts, and other transient stuff
  const INFO = 3;  // Educational messages

  public static $levelName = array(
    self::ERROR => 'error',
    self::WARNING => 'warning',
    self::INFO => 'info');

  private $entries = array(/* BistroLogEntry */);

  public function info(/* string */ $message, $exception_or_str = null) {
    $this->entries[] =
      new BistroLogEntry(self::INFO, $message, $exception_or_str);
  }

  public function warn(/* string */ $message, $exception_or_str = null) {
    $this->entries[] =
      new BistroLogEntry(self::WARNING, $message, $exception_or_str);
  }

  public function error(/* string */ $message, $exception_or_str = null) {
    $this->entries[] =
      new BistroLogEntry(self::ERROR, $message, $exception_or_str);
  }

  public function render() {
    $str = array();
    foreach ($this->entries as $entry) {
      $str[] = $entry->render();
    }
    return $str;
  }

  private static $instance;

  private function __construct() {}  // There can only be one

  public static function getInstance() {
    if (self::$instance === null) {
      self::$instance = new BistroLog();
    }
    return self::$instance;
  }

}
