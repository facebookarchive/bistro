<?php
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class StackedBarChartModifier {
  abstract public function preRender(StackedBarChartData $data);
  public function postRender(/* string */ $html) {
    return $html;
  }
}
