<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroSimpleHostPortSource extends BistroHostPortSource {

  protected static function getName() {
    return 'hp';
  }

  public static function getTitle() {
    return 'hostport list';
  }

  public static function getDescription() {
    return 'Data is host:port pairs separated by whitespace and/or commas.';
  }

  protected function parseImpl() {
    return bistro_parse_hostport_list($this->getData());
  }

}
