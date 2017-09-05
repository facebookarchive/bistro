<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * This is a union of all configuration fields used by StackedBarChart and
 * its modifiers -- do not set all fields at at the same time.
 */
final class StackedBarData {
  // Base data
  public $weight;
  public $color;
  public $textColor;  // optional, instead, you should prefer light backgrounds

  // Labels inside the bars, and on-hover
  // Overwritten by SimpleNamesForStackedBarChart
  public $html;
  public $title;

  // Used by SimpleNamesForStackedBarChart to generate html and title
  // Optionally used by DetailsForStackedBarChart to label the detail section
  public $name;
  public $shortName;

  // Used by DetailsForStackedBarChart to detect clicks on each bar
  public $htmlID;
  // Rendered by DetailsForStackedBarChart in the detail section
  public $detailHtml;
  // Used by DetailsForStackedBarChart to render / modify the detail
  // section.
  //
  // This should be a function name jsprintf'ed with some static arguments
  // (e.g.  JSON).  Doing anything more clever will break in some browsers.
  //
  // The function must return a closure, which will get called with the
  // details node as the only argument.
  public $detailJs;

  // For internal use only, computed from weight.
  public $width;
}
