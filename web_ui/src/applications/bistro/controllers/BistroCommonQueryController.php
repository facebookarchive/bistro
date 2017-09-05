<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @emails oncall+phabricator
 */

/**
 * Provides a query page for BistroCommonPrefs-powered endpoints.
 */
abstract class BistroCommonQueryController extends BistroController {

  const DISABLE_PRESET = 'none';

  abstract protected function makeBistroPrefs();
  abstract protected function getPresetPreferenceKey();
  abstract protected function getResultEndpoint();
  abstract protected function getPageTitle();
  abstract protected function getSubmitText();

  protected function processRequest() {
    $request = $this->getRequest();
    $user = $request->getUser();

    // We pre-fill the form with "presets" that are saved in the Phabricator
    // user preferences.  There can be multiple presets, although right now
    // there is no UI to choose a non-default one (TODO).  However, we do
    // need this GET param in order to turn off presets for
    // renderEditQueryButton().
    //
    // Note that since 'pref_preset' is not a pref, it won't get forwarded
    // to the POST, and we will save the latest set of prefs as a preset.
    // This is probably what we want.
    $preset_id = $request->getStr('pref_preset', 'default');

    $prefs = $this->makeBistroPrefs();
    if ($prefs->hasExplicitPref(BistroCommonPrefs::PREF_HOSTPORT_SOURCE)) {
      // Use the hostport source to initialize the prefs -- this makes query
      // editing work a bit better when the hostport source has overrides.
      //
      // TODO: Surface the overrides in the form (e.g. next to the defaults).
      $hp_source = BistroHostPortSource::newFrom($prefs);
    } else {
      $prefs->init(array());  // Don't have any prefs overrides
    }

    if ($request->isFormPost()) {
      $this->saveUserPreset($preset_id, $prefs);
      return id(new AphrontRedirectResponse())->setURI(
        $prefs->getURI($this->getResultEndpoint()));
    }

    $form = id(new AphrontFormView())->setUser($user);
    $form->appendChild(
      id(new AphrontPanelView())
        ->setWidth(AphrontPanelView::WIDTH_FORM)
        ->setHeader('Usage')
        ->appendChild(phabricator_tag(
          'ol',
          array('style' => 'list-style: disc inside'),
          array(
            phabricator_tag('li', array(), 'You must select a "Transport type".'),
            phabricator_tag('li', array(),
              'You must pick a "Host:port Source" and give it "Host:port '.
              'Data".'),
            phabricator_tag('li', array(),
              'A hostport source can supply defaults that override those '.
              'listed here.'),
            phabricator_tag('li', array(),
              'The values you enter below override all defaults, and will be '.
              'baked into your URL as GET params.')))));

    // Allow query page GET params to override user presets -- as a result,
    // one can hand out links to partially filled query pages.  This is also
    // needed for renderEditQueryButton() to work.
    $presets = $prefs->extractExplicitPrefs($request);
    if ($preset_id !== self::DISABLE_PRESET) {
      $presets += idx(
        bistro_load_user_preferences($user)->getPreference(
          $this->getPresetPreferenceKey(), array()), $preset_id, array());
    }
    $panel = new AphrontPanelView();
    $panel->setWidth(AphrontPanelView::WIDTH_WIDE);
    $panel->setHeader($this->getPageTitle());
    $panel->appendChild(
      $prefs->renderQueryForm($form, $presets, $this->getSubmitText()));

    return $this->buildStandardPageResponse(
      $panel, array('title' => $this->getPageTitle()));
  }

  private function saveUserPreset($preset_id, $prefs) {
    if ($preset_id === self::DISABLE_PRESET) {
      return;
    }
    $user_preferences =
      bistro_load_user_preferences($this->getRequest()->getUser());
    $preset_pref_key = $this->getPresetPreferenceKey();
    $all_presets = $user_preferences->getPreference($preset_pref_key, array());
    $all_presets[$preset_id] = $prefs->getExplicitPrefs();
    $user_preferences->setPreference($preset_pref_key, $all_presets);
    bistro_save_user_preferences(
      $user_preferences, /* is_get_request = */ false);
  }

}
