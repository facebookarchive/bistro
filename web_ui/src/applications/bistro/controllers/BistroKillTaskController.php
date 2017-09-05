<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @emails oncall+phabricator
 */

final class BistroKillTaskController extends PhabricatorController {

  // These are better as prefs, since Bistro's soft-kill timeout is not fixed
  const SEND_TIMEOUT = 3000;
  const RECV_TIMEOUT = 30000;  // Bistro's soft-kill time defaults to 10s


  public function processRequest() {
    $hp = $this->getRequest()->getStr('scheduler');
    $job = $this->getRequest()->getStr('job');
    $node = $this->getRequest()->getStr('node');

    try {
      $response_str = fetch_monitor2_via_http(
        $hp,
        json_encode(array(
          'k' => array(
            'handler' => 'kill_task',
            'job_id' => $job,
            'node_id' => $node))),
        self::SEND_TIMEOUT,
        self::RECV_TIMEOUT);
      $response_arr = json_decode($response_str, /* assoc = */ true);
      if (!is_array($response_arr) || !is_array($response_arr['k'])) {
        $message = 'Error: '.$response_str;
      } else if (isset($response_arr['k']['error'])) {
        $message = 'Error: '.$response_arr['k']['error'];
      } else if (isset($response_arr['k']['data'])) {
        $message = 'killed';
      } else {
        $message = 'Error: '.$response_str;
      }
    } catch (Exception $e) {
      $message = 'Error: '.$e->getMessage();
    }
    return id(new AphrontAjaxResponse())->setContent(hsprintf('%s', array(
      '- ', phabricator_tag('b', array(), $message), ' ')));
  }

}
