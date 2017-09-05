<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Returns true the keys of $arr are exactly $expected_keys (disregarding
 * order) and all values are non-null.
 *
 * Returns false otherwise.
 */
function array_has_exact_keys_set(array $arr, array $expected_keys) {
  foreach ($expected_keys as $key) {
    if (!isset($arr[$key])) {
      return false;
    }
  }
  return count($arr) === count($expected_keys);
}
