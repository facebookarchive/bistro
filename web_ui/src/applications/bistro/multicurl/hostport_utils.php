<?php
// Copyright 2004-present Facebook. All Rights Reserved.

// Functions to help with MultiCurl hostport handling.

/**
 * Prepend a '.' because a suffix of 'yoursite.com' could otherwise
 * be attacked via the domain 'evilyoursite.com'.
 */
function bistro_normalize_hostname_suffix(/* string */ $hostname_suffix) {
  $hostname_suffix = trim($hostname_suffix);
  if ($hostname_suffix === '' || $hostname_suffix[0] === '.') {
    return $hostname_suffix;
  }
  return '.'.$hostname_suffix;
}

/*
 * Paranoia to make sure we only talk to internal hostports from our
 * MultiCurl RPC endpoints:
 *
 *  - hostname must end in the required suffix
 *  - the port must not be below 1024
 *
 * CAUTION: This function is security-sensitive, so take care not to weaken
 * the tests here.
 *
 * NOTE: If you call this, remember to log failures appropriately.
 */
function bistro_hostport_is_valid_fqdn($hostport) {
  // Approximate host:port regex. The hostname part doesn't have to match
  // ALL valid hostnames, only those where people actually run Bistro :)
  //
  // The IPv6 part grossly simplifies http://stackoverflow.com/a/17871737,
  // but probably does not allow anything unsafe.
  //
  // It is important we validate the port part, since the range test could
  // otherwise be wrong.
  if (!preg_match(
    '/^(([a-zA-Z0-9-]+\.)*[a-zA-Z0-9-]+|\[[0-9a-zA-Z:%.]+\]):[0-9]{4,5}$/',
    $hostport)) {
    return false;
  }
  list($host, $port) = bistro_split_hostport($hostport);
  $port = intval($port);  // Redundant in PHP, but better safe than sorry.
  try {
    // Throws if 'bistro.get-state-hostname-suffix' was not configured.  If
    // the user doesn't care about security, they must set '' as the value.
    $hostname_suffix = bistro_get_sitewide_hostname_suffix();
  } catch (Exception $ex) {
    return false;  // Multicurl is massively parallel, so no logspam here.
  }
  // Paranoia: only allow user port numbers
  return $port >= 1024 && $port <= 65535
    && bistro_ends_with($host, $hostname_suffix);
}

// Only called in UI code, not in RPC code (since this throws).
function bistro_get_sitewide_hostname_suffix() {
  $hostname_suffix =
    PhabricatorEnv::getEnvConfig('bistro.get-state-hostname-suffix');
  if ($hostname_suffix === null) {
    throw new Exception(
      'You must set bistro.get-state-hostname-suffix, e.g. by visiting '.
      $_SERVER['HTTP_HOST'].'/config/edit/bistro.get-state-hostname-suffix/');
  }
  return bistro_normalize_hostname_suffix($hostname_suffix);
}

/**
 * The suffix we add/strip to hostnames comes from Bistro prefs rather than
 * the Phabricator pref 'bistro.get-state-hostname-suffix' because it's
 * intended to be easy to change per Bistro deployment.  On the other hand,
 * the global pref 'bistro.get-state-hostname-suffix' is a site-wide
 * security measure that is intended to be configured only by the site
 * administrator (it's validated in the multicurl controller).
 *
 * In order to get sane behavior, the per-deployment suffix must contain the
 * global suffix.
 *
 * Only called in UI code, not in RPC endpoint code (since this throws).
 */
function bistro_get_deployment_hostname_suffix($prefs) {
  $suffix = bistro_normalize_hostname_suffix(
    $prefs->get(BistroCommonPrefs::PREF_STRIP_HOST_SUFFIX));
  $global_suffix = bistro_get_sitewide_hostname_suffix();
  if ($suffix === '') {  // Special case: default to the sitewide suffix.
    return $global_suffix;
  }
  if (!bistro_ends_with($suffix, $global_suffix)) {
    throw new Exception(
      'The Bistro pref "'.BistroCommonPrefs::PREF_STRIP_HOST_SUFFIX.'" ("'.
      $suffix.'") must end with the Phabricator setting '.
      '"bistro.get-state-hostname-suffix" ("'.$global_suffix.'")');
  }
  return $suffix;
}

/**
 * Called on all hostports before displaying them, with the intent of making
 * the displayed information more concise.  Since we strip the same suffix
 * that we add in bistro_hostports_validate_and_make_fqdn(), the displayed
 * hostports should be fine to paste back into Bistro Monitor.
 *
 * Only called in UI code, not in RPC endpoint code (since this throws).
 */
function bistro_hostport_strip_host_suffix($prefs, $hp) {
  $suffix = bistro_get_deployment_hostname_suffix($prefs);
  if ($suffix) {
    list($host, $port) = bistro_split_hostport($hp);
    if (bistro_ends_with($host, $suffix)) {
      return substr($host, 0, -strlen($suffix)).':'.$port;
    }
  }
  return $hp;
}

/**
 * Helper for bistro_hostports_validate_and_make_fqdn().
 *
 * WARNING: If you are tempted to call this directly, see the "Future" note
 * in bistro_parse_hostport_list()
 */
function bistro_hostport_make_fqdn($hp, $hostname_suffix) {
  if ($hostname_suffix === '') {
    return $hp;  // No suffix required
  }
  list($host, $port) = bistro_split_hostport($hp);
  if (bistro_ends_with($host, $hostname_suffix)) {
    return $hp;
  } else {
    return $host.$hostname_suffix.':'.$port;
  }
}

/**
 * As a security measure, our RPC endpoints refuse to make requests to hosts
 * that do not end in the sitewide suffix (being tricked to send Bistro
 * Monitor requests to the wrong host could be bad).
 *
 * At the same time, it's nice to use unqualified hostnames like "foo.bar".
 *
 * The solution: if the user-supplied hostname does not end in the
 * *deployment* suffix, append it.  Then, we are sure to talk to something
 * within our network, which is hopefully safe.
 *
 * Checking & appending the deployment suffix is stronger than the RPC
 * endpoint's test (sitewide suffix), but leads to clear behavior.
 *
 * This runs in the UI code, rather than in the RPC endpoint, so it's
 * actually an extra pre-validation.  The goal is to make it easy for the
 * end user to diagnose bad hostport issues.  If this failed on the
 * multi-curl RPC endpoint (where it's actually relevant for security), the
 * resulting error would be completely opaque.
 *
 * Returns a pair:
 *  (i) the valid hostports, with the suffix appended as necessary,
 *  (ii) a "fully qualified hp => original hp" map, containing an entry for
 *       each hostport that had the suffix appended.
 *
 * Only called in UI code, not in RPC endpoint code (since this throws & logs).
 *
 * @returns
 *    array(valid fully qualified hostports, array(qualified => original))
 */
function bistro_hostports_validate_and_make_fqdn($prefs, array $hostports) {
  $hostname_suffix = bistro_get_deployment_hostname_suffix($prefs);
  $invalid_hostports = array();
  $fqdn_hostports = array();
  $fqdn_to_original = array(/* FQDN:port => original hostport */);
  foreach ($hostports as $hp) {
    $fqdn_hp = bistro_hostport_make_fqdn($hp, $hostname_suffix);
    if (bistro_hostport_is_valid_fqdn($fqdn_hp)) {
      $fqdn_hostports[] = $fqdn_hp;
      if ($fqdn_hp !== $hp) {
        $fqdn_to_original[$fqdn_hp] = $hp;
      }
    } else {
      $invalid_hostports[] = $hp;
    }
  }
  if ($invalid_hostports) {
    bistro_monitor_log()->error(
      'Not fetching invalid hostports: '.implode(', ', $invalid_hostports).
      '. Ports below 1024 are forbidden; hostnames must end in "'.
      bistro_get_sitewide_hostname_suffix().'".');
  }
  return array($fqdn_hostports, $fqdn_to_original);
}
