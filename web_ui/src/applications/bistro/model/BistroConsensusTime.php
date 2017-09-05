<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroConsensusTime extends BistroConsensusFinder {

  protected function renderItem(/* string */ $item, $_is_consensus) {
    if (!$item) {  // Older Bistro versions don't report times.
      return '';
    }
    return strftime('%Y/%m/%d %H:%M:%S %Z', $item);
  }

}
