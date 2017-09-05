<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class PhabricatorApplicationBistro
  extends PhabricatorApplication {

  public function getName() {
    return pht('Bistro');
  }

  public function getBaseURI() {
    return '/bistro/';
  }

  public function getTitleGlyph() {
    return BistroController::GLYPH;
  }

  public function getShortDescription() {
    return pht('Quickly cook many things at once.');
  }

  public function getRoutes() {
    $map = array(
      // Job list
      '' => BistroJobListQueryController::class,
      'jobs' => BistroJobListViewController::class,
      // Task logs
      'logs' => BistroHTTPTaskLogQueryController::class,
      'logs/view' => BistroHTTPTaskLogViewController::class,
      // MultiCurl endpoints. The username regex comes from the function
      // validateUsername(), and is followed by urlbase64-encoded data.
      'monitor2_http_multi/(?<user>[a-zA-Z0-9._-]*[a-zA-Z0-9_-])/'.
        '(?<data>[a-zA-Z0-9_-]+)'  =>
          BistroMonitor2HTTPMultiCurlController::class,
      'kill/task' => BistroKillTaskController::class);
    return array('/bistro/' => $map, '/anthill/' => $map);
  }

}
