<?php
// Copyright 2004-present Facebook. All Rights Reserved.

// Functions in this file provide a shim between Facebook-internal PHP +
// Phabricator, and upstream PHP + Phabricator.

// Marks literal strings as functions for Hack.
function fun($f) {
  return $f;
}

function phabricator_tag($tag, array $attributes = array(), $content = null) {
  return phutil_tag($tag, $attributes, $content);
}

function bistro_render($obj) {
  return $obj->render();
}

function bistro_render_markup($request, $text) {
  return PhabricatorMarkupEngine::renderOneObject(
      (new PhabricatorMarkupOneOff())->setContent($text),
      'default',
      $request->getUser());
}

function bistro_phabricator_form(AphrontRequest $request, $attrs, $content) {
  return phabricator_form($request->getUser(), $attrs, $content);
}

function bistro_phabricator_user_from_name($username) { 
  return id(new PhabricatorUser())->loadOneWhere(
    'username = %s', $username);
}
  
function bistro_load_user_preferences($user) {
  return PhabricatorUserPreferences::loadUserPreferences($user);
}

function bistro_save_user_preferences($prefs, $is_get_request) { 
  $unguarded = null;
  if ($is_get_request) {
    $unguarded = AphrontWriteGuard::beginScopedUnguardedWrites();
  }
  $prefs->save();
  unset($unguarded);
}

function bistro_get_remote_address($request) {
  return $request->getRemoteAddress();
}

function bistro_read_random_bytes($size) {
  return Filesystem::readRandomBytes($size);
}

function bistro_head($s) {
  return head($s);
}

function bistro_ucwords($s) {
  return ucwords($s);
}

function bistro_ucfirst($s) {
  return ucfirst($s);
}

// Future: maybe forki the now-deleted Phacility Phabricator code as
// BistroPanelView?
class AphrontPanelView implements PhutilSafeHTMLProducerInterface {

  private $box;

  const WIDTH_FORM = 'form';
  const WIDTH_WIDE = 'wide';

  public function __construct() {
    $this->box = new PHUIObjectBoxView();
  }

  public function appendChild($c) {
    $this->box->appendChild($c);
    return $this;
  }

  public function setHeader($s) {
    $this->box->setHeaderText($s);
    return $this;
  }

  public function setWidth($width) {
    require_celerity_resource('bistro-panel-view-css');
    $this->box->addClass('bistro-panel-width-'.$width);
    return $this;
  }
  
  public function addClass($class) {
    $this->box->addClass($class);
    return $this;
  }

  public function producePhutilSafeHTML() {
    return $this->box->render();
  }
}
