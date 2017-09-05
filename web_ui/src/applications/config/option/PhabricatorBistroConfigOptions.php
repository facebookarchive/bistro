<?php
// Copyright 2004-present Facebook. All Rights Reserved.

final class PhabricatorBistroConfigOptions
  extends PhabricatorApplicationConfigOptions {

  public function getName() {
    return pht("Bistro");
  }

  public function getDescription() {
    return pht("Bistro options.");
  }

  public function getGroup() {
    return pht("apps");
  }

  public function getOptions() {
    return array(
      // The default of null is forbidden by bistro_hostport_is_valid(), so
      // this option is required.
      $this->newOption('bistro.get-state-hostname-suffix', 'string', null)
        ->setSummary(pht("Restrict queries to hostnames with this suffix"))
        ->setDescription(
          pht(
            "REQUIRED: Using Bistro Monitor to query a malicious host ".
            "will probably compromise your web server. To reduce your ".
            "attack area, you can prohibit queries to any hostname that ".
            "does not end with this suffix.\n\n".
            "A dot is automatically added in front of your suffix, so leave ".
            "it out.\n\n".
            "This is not recommended outside of testing, but if you must, ".
            "you can set this to '' to allow all hostnames.\n\n".
            "When this option is nonempty, we automatically append the ".
            "suffix to any input hostnames that do not already have it. ".
            "This lets you use unqualified domains."
          )
        )
        ->addExample('facebook.com', pht('Valid Setting')),
      // If your serving setup requires some magic HTTP headers for
      // self-curls to work, this option saves the day.
      $this->newOption('bistro.self-curl-extra-http-header', 'string', null)
        ->setSummary(pht("Add this HTTP header to self-curls"))
        ->setDescription(
          pht(
            "OPTIONAL: If your Phabricator install is proxied behind ".
            "some host that sets a magic HTTP header, then Bistro's ".
            "self-CURLs might fail without this magic HTTP header. ".
            "If so, set it using this option."
          )
        )
        ->addExample('Magic-header: SomeValue', pht('Sample Setting')));
  }

}
