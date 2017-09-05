<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Renders StackedBarCharData.
 */
final class StackedBarChart {

  const PADDING_PX = 4;

  private $data;

  public function __construct(StackedBarChartData $data) {
    $this->data = $data;
  }

  public function render() {
    $data = $this->data;
    // Apply rightmost modifier first
    $modifiers = array_reverse($data->modifiers);

    foreach ($modifiers as $modifier) {
      $data = $modifier->preRender($data);
      if (!$data) {
        return 'no data';
      }
    }

    if ($data->forceWeightAsWidth) {
      if (!$data->getTotalWeight()) {
        return 'no data';
      }
      foreach ($data->bars as $bar) {
        $bar->width = $bar->weight;
      }
    } else if (!$this->computeBarWidths($data)) {
      return 'no data';
    }

    require_celerity_resource('stacked-bar-chart');
    $bars_html = array();
    foreach ($data->bars as $bar) {
      $bar->width -= 2 * self::PADDING_PX;  // subtract out padding
      $bars_html[] = $this->renderBar($bar);
    }
    $html = phutil_tag(
      'table',
      array('class' => 'stacked-bar-chart '.implode(' ', $data->cssClasses)),
      $bars_html);

    foreach ($modifiers as $modifier) {
      $html = $modifier->postRender($html);
    }
    return $html;
  }

  private function renderBar(StackedBarData $bar) {
    $styles = array(
      'width:'.intval($bar->width).'px',
      // Don't autoexpand the cell, ever.
      'max-width:'.intval($bar->width).'px');
    if ($bar->color !== null) {
      $styles[] = 'background-color:'.$bar->color;
    }
    if ($bar->textColor !== null) {
      $styles[] = 'color:'.$bar->textColor;
    }
    $attrs = array('style' => implode(';', $styles));
    if ($bar->htmlID !== null) {
      $attrs['id'] = $bar->htmlID;
    }
    if ($bar->title !== null) {
      $attrs['title'] = $bar->title;
    }
    return phabricator_tag('td', $attrs, $bar->html);
  }

  const _USE_MIN_WIDTH = -1;  // Sentinel value written into bar.width

  private function computeBarWidths(StackedBarChartData $data) {
    // Clear the widths to deal with double-rendering
    foreach ($data->bars as $bar) {
      $bar->width = null;
    }

    // The retry logic is needed to both fit in total_width, and not to have
    // bars shorter than min_width.  I am lazy, so this is quadratic :(
    // TODO: faster algorithm
    //
    // The loop stores the percentage width (or _USE_MIN_WIDTH) in $bar->width

    // Both values decrease whenever a bar is below min width
    $adjusted_width = $data->totalWidth;
    $total_weight = $data->getTotalWeight();
    if (!$total_weight) {
      return False;
    }

    $retry = True;
    while ($retry) {
      $retry = False;
      foreach ($data->bars as $bar) {
        if ($bar->width === self::_USE_MIN_WIDTH) {
          continue;
        }

        if ($total_weight) {
          $bar->width = $bar->weight / $total_weight;
          $missing_width = $adjusted_width * $bar->width - $data->minBarWidth;
          if ($missing_width < 0) {
            $adjusted_width -= $data->minBarWidth;
            $total_weight -= $bar->weight;
            $bar->width = self::_USE_MIN_WIDTH;
            $retry = True;
            break;
          }
        } else {
          $bar->width = self::_USE_MIN_WIDTH;
        }
      }
    }

    $total_width = 0;
    foreach ($data->bars as $bar) {
      if ($bar->width === self::_USE_MIN_WIDTH) {
        $bar->width = $data->minBarWidth;
      } else {
        $bar->width *= $adjusted_width;
      }
      $total_width += $bar->width;
    }

    // Round to get integers -- important for visually consistent widths
    foreach ($data->bars as $bar) {
      if ($total_width > $data->totalWidth) {
        $rounded = floor($bar->width);
      } else {
        $rounded = ceil($bar->width);
      }
      $total_width += $rounded - $bar->width;
      $bar->width = $rounded;
    }

    return True;
  }

}
