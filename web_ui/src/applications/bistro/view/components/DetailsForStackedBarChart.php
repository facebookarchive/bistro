<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class DetailsForStackedBarChart extends StackedBarChartModifier {

  private $detailDivID;
  private $totalWidth;

  public function preRender(StackedBarChartData $data) {
    $this->detailDivID = uniqid('stacked-bar-details');
    $total_weight = $data->getTotalWeight();
    $this->totalWidth = $data->totalWidth;

    foreach ($data->bars as $bar) {
      $bar->htmlID = uniqid('stacked-bar');

      // Clicking on the bar displays additional details
      $details = $this->renderBarDetails($bar, $total_weight);
      Javelin::initBehavior(
        'add-detail-toggle-listener',
        array(
          // See the comment in StackedBarData.
          'detail_js' => $bar->detailJs,
          'detail_html' => $details,
          'detail_id' => $this->detailDivID,
          'listener_id' => $bar->htmlID));
    }

    return $data;
  }

  public function postRender(/* string */ $html) {
    require_celerity_resource('details-for-stacked-bar-chart');
    return phabricator_tag('div', array(
      'class' => 'stacked-bar-chart-with-details',
      'style' => 'width: '.$this->totalWidth.'px;'), array(
      $html,
      phabricator_tag('div', array(
        'id' => $this->detailDivID,
        'class' => 'stacked-bar-chart-detail-view'))));
  }

  private function renderBarDetails(StackedBarData $bar, $total_weight) {
    return phabricator_tag('p', array(), array(
      phabricator_tag(
        'b',
        array(),
        $bar->name === null
          ? $bar->html
          : node_number_format($bar->weight).
            ' of '.$total_weight.' '.$bar->name),
      $bar->detailHtml));
  }

}
