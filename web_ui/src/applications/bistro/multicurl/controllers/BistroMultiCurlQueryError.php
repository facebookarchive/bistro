<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroMultiCurlQueryError extends Exception {

  public $data;  // Cannot use getMessage() since PHP coerces it to string.

  public function __construct($data) {
    parent::__construct();  // Required by HackLang
    $this->data = $data;
  }

}
