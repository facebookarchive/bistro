<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * For each bar, populates the title and html on the basis of name and
 * shortName, putting the weight + short name as the in-bar text, and the
 * weight percentage + name as the on-hover title.
 */
final class SimpleNamesForStackedBarChart extends StackedBarChartModifier {

  public function preRender(StackedBarChartData $data) {
    $total_weight = $data->getTotalWeight();
    if (!$total_weight) {
      return null;
    }

    foreach ($data->bars as $bar) {
      $bar->html = hsprintf(
        '<b>%s</b>%s%s',
        node_number_format($bar->weight),
        $bar->shortName !== '' ? hsprintf('<br/>') : '',
        $bar->shortName);
      $bar->title = number_format((($bar->weight * 100) / $total_weight), 2).
                    '% '.$bar->name.' (out of '.$total_weight.')';
    }

    return $data;
  }

}
