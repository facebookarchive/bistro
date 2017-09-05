<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Prefs are a set of typed key-value pairs that configure Bistro Monitor.
 *
 * You make prefs from a request:
 *  - Start with the default values of the prefs.
 *  - Ask the current hostport source for any overrides of the defaults.
 *  - Use the current request's GET params for the final override.
 *
 * Relationship with PhabricatorUserPreferences:
 *  - can grab sets of overrides from preferences
 *  - can save sets of overrides to preferences
 */
abstract class BistroPrefs {

  // 'all' is not a real endpoint. It is used as wildcard in "pref override"
  // configs.  E.g.  when you add a pref override for a hostport source, you
  // can allow a per-endpoint pref override:
  //   {"prefs": {"job_list": {"pref1": ...}, "all": {"pref2": ...}}}
  const ENDPOINT_ALL = 'all';

  const SECTION = 1;
  const DEFAULT_VALUE = 2;  // if omitted, defaults to NULL
  const TYPE = 3;
  const LABEL = 4;
  const CAPTION = 5;
  const CONTROL = 6;
  const UNITS = 7;
  const ALLOWED_VALUES = 8;  // if omitted, allows all values

  const TYPE_INT = 1;
  const TYPE_STR = 2;
  const TYPE_BOOL = 3;
  private static $typeNames = array(
    self::TYPE_INT => 'int',
    self::TYPE_STR => 'str',
    self::TYPE_BOOL => 'bool');

  private $schemata;
  private $sectionNamesInOrder;

  private $request;
  private $prefs;
  private $explicitPrefs;
  private $defaultOverrides;  // From the hostport source, saved for rendering

  abstract public function getEndpointID();  // Used to pick the prefs override
  abstract protected function createSectionNamesInOrder();
  abstract protected function createSchemata();

  /**
   * The object is not fully usable until its init() is called (usually by
   * BistroHostPortSource).  The reason for the two-stage initialization is
   * that we want hostport sources to have the option to override prefs.
   *
   * The class deliberately has limited functionality until the second init
   * happens, so that if you forget to apply overrides in some new code
   * path, the prefs code acts very broken and you notice immediately.
   *
   * You could make this more elegant by splitting the "early" and the
   * "full" prefs into two separate classes -- send a patch.
   */
  public function __construct(AphrontRequest $request) {
    $this->request = $request;
    $this->schemata = $this->createSchemata();
    $this->sectionNamesInOrder = $this->createSectionNamesInOrder();

    // We need the explicit prefs early to be able to init a hostport source.
    $this->explicitPrefs = $this->extractExplicitPrefs($request);
  }

  public function extractExplicitPrefs(AphrontRequest $request) {
    $explicit_prefs = array();
    $request_data = $request->getRequestData();
    foreach ($this->schemata as $pref_key => $schema) {
      // Values specified as GET params take priority.
      $type = $schema[self::TYPE];
      $get_value = idx($request_data, $pref_key);
      if ($get_value !== '' && $get_value !== null) {
        $explicit_prefs[$pref_key] = $get_value;
      }
    }
    $this->enforceTypes($explicit_prefs);
    return $explicit_prefs;
  }

  public function init(array $default_overrides) {
    $this->defaultOverrides = $default_overrides;  // Saved for render()
    // Set up the defaults: first use the hostport source, then the schema.
    $this->prefs = array();
    foreach ($this->schemata as $pref_key => $schema) {
      // First come the hostport source-provided defaults.
      $override_value = idx($default_overrides, $pref_key);
      if ($override_value !== null) {
        $this->prefs[$pref_key] = $override_value;
        continue;
      }

      // Failing that, the default
      $this->prefs[$pref_key] = $this->getDefault($pref_key);
    }
    $this->prefs = $this->explicitPrefs + $this->prefs;
    $this->enforceTypes($this->prefs);
    return $this;
  }

  /**
   * This is for the funny cases where we need a pref before prefs are fully
   * initialized.
   *
   * Overrides are not available here, by design, since this function is
   * only used in the process of fetching overrides.
   */
  public function getPreInit($pref_key) {
    if (isset($this->explicitPrefs[$pref_key])) {
      return $this->explicitPrefs[$pref_key];
    }
    return $this->getDefault($pref_key);
  }

  private static function coerceType($type, $value) {
    if ($type === self::TYPE_INT) {
      return intval($value);
    } else if ($type === self::TYPE_STR) {
      return strval($value);
    } else if ($type === self::TYPE_BOOL) {
      if (strtolower($value) === 'false') {
        return false;
      } else if (strtolower($value) === 'true') {
        return true;
      }
      return (bool)$value;
    }
  }

  private function validate($pref, $value) {
    $schema = $this->schemata[$pref];
    $value = self::coerceType($schema[self::TYPE], $value);
    $allowed = idx($schema, self::ALLOWED_VALUES);
    if ($allowed !== null && !in_array($value, $allowed)) {
      $default = $this->getDefault($pref);
      bistro_monitor_log()->warn(
        'Value "'.$value.'" is not valid for preference "'.$pref.'" '.
        '(allowed values are "'.implode('", "', $allowed).'"); '.
        'defaulting to "'.$default.'".');
      return $default;
    }
    return $value;
  }

  private function enforceTypes(array &$prefs) {
    foreach ($prefs as $pref => $value) {
      $value = $this->validate($pref, $value);
      $prefs[$pref] = $value;
    }
  }

  public function getDefault($pref_key) {
    $schema = $this->schemata[$pref_key];
    // This doesn't call validate() to avoid infinite recursion.
    return self::coerceType(
      $schema[self::TYPE], idx($schema, self::DEFAULT_VALUE));
  }

  public function getUnits($pref_key) {
    return idx($this->schemata[$pref_key], self::UNITS, '');
  }

  public function get($pref_key) {
    return $this->prefs[$pref_key];
  }

  /**
   * If the pref is not a valid JSON array, returns array().
   */
  public function getJSONArray($key) {
    $level_to_range = json_decode($this->get($key), true);
    if (is_array($level_to_range)) {
      return $level_to_range;
    }
    bistro_monitor_log()->warn(
      'Preference "'.$key.'" must be a JSON array, defaulting to {}');
    return array();
  }

  public function getRequest() {
    return $this->request;
  }

  public function getExplicitPref($pref_key) {
    return $this->explicitPrefs[$pref_key];
  }

  public function hasExplicitPref($pref_key) {
    return isset($this->explicitPrefs[$pref_key]);
  }

  public function getExplicitPrefs() {
    return $this->explicitPrefs;
  }

  public function getURI($endpoint) {
    $uri = new PhutilURI($endpoint);
    $uri->setQueryParams($this->explicitPrefs);
    return $uri;
  }

  public function getCaption($pref_key) {
    $caption = idx($this->schemata[$pref_key], self::CAPTION);
    $default = $this->getDefault($pref_key);
    if ($default !== null) {
      $caption .=
        ' Defaults to '.var_export($default, 1).$this->getUnits($pref_key).'.';
    }
    return $caption;
  }

  public function render() {
    require_celerity_resource('bistro-prefs');

    $sections = array();
    foreach ($this->schemata as $pref_key => $schema) {
      // Prepare to render the top line
      $is_explicit = isset($this->explicitPrefs[$pref_key]);
      $key_tag = phabricator_tag(
        'span',
        array('class' => ($is_explicit ? 'ex' : 'im').'plicit-pref'),
        $pref_key);

      $default_value = $this->getDefault($pref_key);
      $override_value = idx($this->defaultOverrides, $pref_key);

      $value = $this->prefs[$pref_key];
      $value_tag = phabricator_tag(
        'span',
        array(
          'class' => ($value !== $default_value ? 'non' : '').'default-value'),
        var_export($value, 1));

      // Render the top line with the pref's key, value, and type
      $pref_value_tags = array(
        $key_tag, ' = ', $value_tag,
        phabricator_tag(
          'span',
          array('class' => 'schema-type'),
          self::$typeNames[$schema[self::TYPE]]));
      if ($override_value !== null && !$is_explicit) {
        $pref_value_tags[] = phabricator_tag(
          'span',
          array('class' => 'hostport-source-override'),
          'from hostport source');
      }

      // Render details with the label, caption, default, units
      $details = array();
      $label = idx($schema, self::LABEL);
      if ($label !== null) {
        $details[] = phabricator_tag('p', array('class' => 'schema-label'), $label);
      }
      $caption = $this->getCaption($pref_key);
      if ($caption !== null) {
        $details[] =
          phabricator_tag('p', array('class' => 'schema-caption'), $caption);
      }

      $sections[$schema[self::SECTION]][] = phabricator_tag('div', array(), array(
        phabricator_tag(
          'div', array('style' => 'margin-top: 5px'), $pref_value_tags),
        phabricator_tag('div', array('style' => 'margin-left: 5px'), $details)));
    }

    $section_tags = array();
    foreach ($this->sectionNamesInOrder as $sec) {
      $section_tags[] = phabricator_tag('div', array(), array(
        phabricator_tag('h2', array(), $sec),
        phabricator_tag(
          'div', array('style' => 'margin-left: 10px'), $sections[$sec])));
    }

    $listener_id = uniqid('query-prefs-disclosure');
    $detail_id = uniqid('query-prefs-details');

    Javelin::initBehavior(
      'add-detail-toggle-listener',
      array(
        'detail_html' => $section_tags,
        'detail_id' => $detail_id,
        'listener_id' => $listener_id));

    return id(new AphrontPanelView())
      ->setHeader(phabricator_tag('a', array('id' => $listener_id), hsprintf(
        '<span class="expand">&#9660;</span>'.
        '<span class="contract">&#9658;</span> Query Prefs')))
      ->setWidth(AphrontPanelView::WIDTH_FORM)
      ->addClass('bistro-prefs')
      ->appendChild(phabricator_tag('div', array('id' => $detail_id), null));
 }

  /**
   * Used for giving the user a chance to modify a few selected prefs on a
   * page that receives prefs via GET params.
   *
   * Use $override_values = array(pref_key => value) to emit hidden
   * controls with pref values different from the current ones. If you
   * set a null value, no control will be made emitted for that pref.
   */
  public function renderHiddenControls(array $override_values = array()) {
    $controls = array();
    foreach ($override_values + $this->explicitPrefs as $pref_key => $value) {
      if ($value !== null) {
        // Using pref keys here let
        $controls[$pref_key] = phabricator_tag('input', array(
          'type' => 'hidden',
          'name' => $pref_key,
          'value' => $value));
      }
    }
    return $controls;
  }

  /**
   * Wrapper for renderHiddenControls to redirect the user to a prefs
   * query page based on BistroCommonQueryController.
   */
  public function renderEditQueryButton(AphrontRequest $request, $endpoint) {
    return bistro_phabricator_form(
      $request,
      array('action' => $endpoint, 'method' => 'GET'),
      array(
        $this->renderHiddenControls(),
        // Don't pre-populate the form with any presets from the Phabricator
        // user preferences, only fill in the GET params we pass from here.
        phabricator_tag('input', array(
          'type' => 'hidden',
          'name' => 'pref_preset',
          'value' => BistroCommonQueryController::DISABLE_PRESET)),
        phabricator_tag(
          'button',
          array('class' =>'green', 'type' => 'submit'),
          'Edit entire query')));
  }

  /**
   * Wrapper for renderHiddenControls to make it easy to change a few of the
   * prefs on a page.
   *
   * Note that this results in a messier URL than what the query page would
   * generate, since it doesn't distinguish between "control not set" and
   * "control set to the default".  Probably not worth fixing.
   */
  public function renderUpdateForm(
      AphrontRequest $request,
      array $override_values,  // pref_key => null erases a value
      $custom_controls) {

    return bistro_phabricator_form(
      $request,
      array(
        'action' => $request->getPath(),
        'method' => 'GET'),
      id(new PHUIFormLayoutView())
        ->appendChild($this->renderHiddenControls($override_values))
        ->appendChild($custom_controls));
  }

  /**
   * Renders a control for editing a single pref. Also has two helpers below.
   */
  public function renderPrefControl($pref_key, $preset_value) {
    $schema = $this->schemata[$pref_key];
    $type = $schema[self::TYPE];
    $default = $this->getDefault($pref_key);

    // Make the form field
    $control = idx($schema, self::CONTROL);
    if ($control !== null) {
      list($func, $args) = $control;
      $field = call_user_func_array($func, $args);
    } else {
      $field = new AphrontFormTextControl();
    }

    $caption = idx($schema, self::CAPTION);
    if ($default !== null) {
      $caption .= ' Defaults to '.var_export($default, 1).
        $this->getUnits($pref_key).'.';
    }

    return $field
      ->setName($pref_key)
      ->setLabel($schema[self::LABEL])
      ->setCaption($caption)
      ->setValue($preset_value);
  }

  public function renderPrefControlCurrentValue($pref_key) {
    return $this->renderPrefControl($pref_key, $this->get($pref_key));
  }

  /**
   * Renders a full form for editing all available prefs.
   */
  public function renderQueryForm($form, array $presets, $submit_button_text) {
    $sections = array();
    foreach ($this->schemata as $pref_key => $schema) {
      $sections[$schema[self::SECTION]][] = $this->renderPrefControl(
        $pref_key, idx($presets, $pref_key));
    }

    foreach ($this->sectionNamesInOrder as $sec) {
      $sec_panel = new AphrontPanelView();
      $sec_panel->setWidth(AphrontPanelView::WIDTH_FORM);
      $sec_panel->setHeader($sec);
      foreach ($sections[$sec] as $field) {
        $sec_panel->appendChild($field);
      }
      $form->appendChild($sec_panel);
      $form->appendChild(
        id(new AphrontFormSubmitControl())->setValue($submit_button_text));
    }
    return $form;
  }

  //
  // Helpers for making schemata for non-text controls
  //

  /**
   * Requires ALLOWED_VALUES, populates DEFAULT_VALUE & CONTROL.
   */
  protected function createDropdownSchema(array $arr) {
    $allowed = $arr[self::ALLOWED_VALUES];
    if (!isset($arr[self::DEFAULT_VALUE])) {
      $arr[self::DEFAULT_VALUE] = bistro_head($allowed);
    }
    // We want dropdowns to start in the 'unset' state.
    $allowed = array('' => '') + $allowed;
    $arr[self::CONTROL] =
      array('bistro_make_dropdown', array(array_combine($allowed, $allowed)));
    return $arr;
  }

  /**
   * Populates CONTROL and TYPE (bool).
   */
  protected function createBooleanSchema(array $arr) {
    $arr[self::CONTROL] = array('bistro_make_boolean_control', array());
    $arr[self::TYPE] = self::TYPE_BOOL;
    return $arr;
  }

}
