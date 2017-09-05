<?php
// Copyright 2004-present Facebook. All Rights Reserved.

abstract class BistroLoadableByName {

  // YOU MUST IMPLEMENT THESE (but PHP 5.2 hates abstract static)
  //
  // abstract protected static function getName();
  // abstract public static function getTitle();
  // abstract public static function getDescription();

  /**
   * Returns a string summary of the object
   */
  abstract public function getSummary();

  public static function getNameToClass(/* string */ $base_class) {
    $fetcher_symbols = id(new PhutilSymbolLoader())
      ->setType('class')
      ->setAncestorClass($base_class)
      ->selectAndLoadSymbols();
    $name_to_class = array();
    foreach ($fetcher_symbols as $fetcher_symbol) {
      $fetcher_class = $fetcher_symbol['name'];
      $name = call_user_func(array($fetcher_class, 'getName'));
      if ($name === null) {
        continue;  // This is a way for classes to opt out of being enumerated
      }
      if (!preg_match('/[a-z_]*/', $name)) {
        throw new Exception('class '.$fetcher_class.'has bad name '.$name);
      }
      $name_to_class[$name] = $fetcher_class;
    }
    return $name_to_class;
  }

  // Children implement the real newFrom(), since PHP 5.2 does not allow
  // arguments to vary between parent and child.
  public static function baseNewFrom(
    /* string */ $name,
    /* string */ $base_class
    /* all extra args are passed to the constructor */
  ) {
    $name_to_class = self::getNameToClass($base_class);  // TODO: memoize
    return newv(
      $name_to_class[$name], array_slice(func_get_args(), 2));
  }

}
