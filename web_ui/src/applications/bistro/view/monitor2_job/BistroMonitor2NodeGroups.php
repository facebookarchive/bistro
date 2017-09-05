<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMonitor2NodeGroups {
  public $allGroup;
  public $unsegmentedGroup;
  public $segmentToGroup = array();

  public function __construct() {
    $this->allGroup = new BistroMonitor2NodeGroup();
    $this->unsegmentedGroup = new BistroMonitor2NodeGroup();
  }
}
