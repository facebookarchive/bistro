<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @concrete-extensible
 *
 * This class can do three things:
 *
 *  - Collect items from locations, like "job enabled" values from hostports.
 *
 *  - Compute the "consensus" item, by picking the one that's reported
 *    from the largest number of locations.
 *
 *  - Render all reported items, together with their locations. The typical
 *    rendering is like this:
 *
 *      Some item +
 *
 *    Clicking on the + displays the list of locations reporting the item.
 *
 *    When locations report different items, all the items are rendered, one
 *    above another, as well as an inconsistency warning, and a magenta
 *    background.  The + sign works the same way.
 *
 *    Provide a custom addItem() to customize how items are collected.
 *    Overload renderItem() to provide custom rendering.
 */
class BistroConsensusFinder {

  protected $data = array(/* serialized input => array(location => 1) */);

  /**
   * A location is typically a hostport, but sometimes a hostport + bottle
   */
  public function add(/* string */ $item, /* string */ $location) {
    $this->data[$item][$location] = 1;
  }

  public function getConsensus() {
    $counts = array_map(fun('count'), $this->data);
    return array_search(max($counts), $counts);
  }

  public function isInconsistent() {
    return count($this->data) > 1;
  }

  public function render() {
    require_celerity_resource('bistro-consensus-finder');

    if (!$this->data) {
      return '';  // Returning a DOM node can prevent margin collapse.
    }

    $is_inconsistent = $this->isInconsistent();

    $variants = array();
    if ($is_inconsistent) {
      $variants[] = phabricator_tag('p');
    }

    // Render the consensus item differently
    $consensus_item = $this->getConsensus();

    foreach ($this->data as $item => $locations) {
      $item_html = $this->renderItem($item, $consensus_item === $item);
      if ($item_html === '' && !$is_inconsistent) {
        // When all locations just want to render an empty string, it's
        // nicer not to render the 'locations reporting' chrome to keep the
        // visual noise down.
        return '';
      }

      $locations = array_keys($locations);
      asort($locations);

      $listener_id = uniqid('consensus-finder-variant');
      $location_div_id = uniqid('consensus-finder-locations');
      $location_count_label =
        'From '.count($locations).' location'.
        (count($locations) > 1 ? 's' : '').':';

      $variants[] = phabricator_tag('div', array(), phabricator_tag(
        'table',
        array(
          // Hovering over a variant shows what locations have it
          'title' => $location_count_label."\n".implode("\n", $locations)),
        array(
          hsprintf(
            '<tr><td>%s</td>'.
            '<td><span class="consensus-variant">%s</span></td></tr>',
            phabricator_tag(
              'a',
              array(
                'id' => $listener_id,
                'title' => 'Show/hide locations reporting this'.
                ($is_inconsistent ? ' inconsistent' : '').' value'),
              hsprintf(
                '<span class="expand">+%s</span>'.
                '<span class="contract">-%s</span>',
                count($locations), count($locations))),
            $item_html),
          phabricator_tag(
            'tr',
            array(),
            phabricator_tag(
              'td',
              array(
                'id' => $location_div_id,
                'colspan' => 2,
                'class' => 'locations'))))));

      Javelin::initBehavior(
        'add-detail-toggle-listener',
        array(
          'detail_html' => hsprintf(
            // The linebreaks are in lieu of vertical padding, which we want
            // to avoid, since the toggle logic just edits the innerHTML
            // instead of using display: none.
            '<br/><b>%s</b> %s <br/><br/>',
            $location_count_label,
            implode(', ', $locations)),
          'detail_id' => $location_div_id,
          'listener_id' => $listener_id)
      );

    }

    $classes = 'bistro-consensus-finder';
    // Highlight the variants if there is more than one.
    $classes .= $is_inconsistent ? ' inconsistent' : '';
    return phabricator_tag('div', array('class' => $classes), $variants);
  }

  protected function renderItem(/* string */ $item, /* bool */ $is_consensus) {
    return $item;
  }

}
