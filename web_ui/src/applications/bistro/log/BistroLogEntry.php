<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroLogEntry {

  private $timestamp;
  private $level;
  private $message;
  private $details;

  public function __construct(
    /* int */ $level,
    /* string */ $message,
    /* Exception or string */ $exception_or_str = null) {
    $this->timestamp = microtime(true);
    $this->details =
      $exception_or_str === null ? null : strval($exception_or_str);
    phlog(
      // phlog already includes the integer parts of the timestamp
      intval(1000000 * ($this->timestamp - intval($this->timestamp))).'us @ '.
      'Bistro Monitor '.BistroLog::$levelName[$level].': '.
      // Some messages are arrays containing HTML
      ((string)hsprintf('%s', $message)).
      ($this->details ? "\n".strval($this->details) : ''));
    $this->level = $level;
    $this->message = $message;
  }

  private static $severityMap = array(
    BistroLog::ERROR => BistroErrorView::SEVERITY_ERROR,
    BistroLog::WARNING => BistroErrorView::SEVERITY_WARNING,
    BistroLog::INFO => BistroErrorView::SEVERITY_NOTICE);

  public function render() {
    require_celerity_resource('bistro-log-entry');

    $view = bistro_id(new BistroErrorView())
      ->setSeverity(self::$severityMap[$this->level]);
    $level = bistro_ucfirst(BistroLog::$levelName[$this->level]);
    if ($this->details) {
      $view
        ->setTitle($level.': '.$this->message)
        ->appendChild(phabricator_tag(
          'pre',
          array('class' => 'bistro-log-exception'),
          $this->details));
    } else {
      $view
        ->setTitle($level)
        ->appendChild($this->message);
    }
    // Timestamp
    $view->appendChild(phabricator_tag(
      'div',
      array('class' => 'bistro-log-timestamp'),
      strftime('%c', $this->timestamp).' @ '.
      intval(1000000 * ($this->timestamp - intval($this->timestamp))).'us'));
    return $view->render();
  }

}
