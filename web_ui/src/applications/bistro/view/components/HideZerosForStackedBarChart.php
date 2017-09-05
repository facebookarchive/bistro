<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Only render the bars with nonzero weight.
 */
final class HideZerosForStackedBarChart extends StackedBarChartModifier {

  public function preRender(StackedBarChartData $data) {
    // Shallow-copy the properties to avoid changing the original data
    $new_data = clone $data;
    $new_data->bars = array();
    foreach ($data->bars as $bar) {
      if ($bar->weight) {
        $new_data->bars[] = $bar;
      }
    }
    return $new_data;
  }

}
