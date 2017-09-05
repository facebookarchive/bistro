<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @emails oncall+phabricator
 */

abstract class BistroController extends PhabricatorController {

  const BASE_URI = '/bistro/';

  // "HOT SPRINGS" ("\xe2\x99\xa8") is quite appropriate but doesn't render
  // very cleanly at small sizes that are prevalent in tab titles.
  //
  // "NEUTER" ("\xe2\x9a\xb2") is a frying pan standing on its handle.
  //
  // "CONJUNCTION" ("\xe2\x98\x8c") is a smaller frying pan with the handle
  // pointing up and to the right.
  const GLYPH = "\xe2\x98\x8c";


  public function buildStandardPageResponse($view, array $data) {
    $page = $this->buildStandardPageView();

    if (method_exists($view, 'setUseJavelinMagicalInit')) {
      $view->setUseJavelinMagicalInit(true);
    }

    $page->setApplicationName('Bistro Monitor');
    $page->setBaseURI(self::BASE_URI);
    $page->setTitle(idx($data, 'title'));
    $page->setGlyph(self::GLYPH);

    $page->appendChild($view);

    $response = new AphrontWebpageResponse();
    return $response->setContent(bistro_render($page));
  }

}
