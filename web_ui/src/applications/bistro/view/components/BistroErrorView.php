<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class BistroErrorView {

  const SEVERITY_ERROR = 'error';
  const SEVERITY_WARNING = 'warning';
  const SEVERITY_NOTICE = 'notice';
  const SEVERITY_NODATA = 'nodata';

  private $title;
  private $errors;
  private $severity;
  private $id;
  private $children = array();

  public function setTitle($title) {
    $this->title = $title;
    return $this;
  }

  public function setSeverity($severity) {
    $this->severity = $severity;
    return $this;
  }

  public function setErrors(array $errors) {
    $this->errors = $errors;
    return $this;
  }

  public function setID($id) {
    $this->id = $id;
    return $this;
  }

  final public function appendChild($child) {
    $this->children[] = $child;
    return $this;
  }

  public function render() {
    require_celerity_resource('bistro-error-view-css');

    $errors = $this->errors;
    if ($errors) {
      $list = array();
      foreach ($errors as $error) {
        $list[] = phabricator_tag(
          'li',
          array(),
          $error);
      }
      $list = phabricator_tag(
        'ul',
        array(
          'class' => 'bistro-error-view-list'),
        $list);
    } else {
      $list = null;
    }

    $title = $this->title;
    if (strlen($title)) {
      $title = phabricator_tag(
        'h1',
        array(
          'class' => 'bistro-error-view-head'),
        $title);
    } else {
      $title = null;
    }

    $this->severity = nonempty($this->severity, self::SEVERITY_ERROR);

    $classes = array();
    $classes[] = 'bistro-error-view';
    $classes[] = 'bistro-error-severity-'.$this->severity;
    $classes = implode(' ', $classes);

    $children = $this->children;
    $children[] = $list;

    return phabricator_tag(
      'div',
      array(
        'id' => $this->id,
        'class' => $classes),
      array(
        $title,
        phabricator_tag(
          'div',
          array(
            'class' => 'bistro-error-view-body'),
          $children)));
  }
}
