<?php
// Copyright 2004-present Facebook. All Rights Reserved.

//
// This file is a grab-bag of functions because Phabricator lint rules
// don't like having helper functions next to the classes that use them,
// and I don't like having gratuitous static functions on classes.
//


// XXX switching to something that returns null when $arr is null should
// increase perf by short-circuiting deep evaluations.
function adx($arr, $k) {
  return idx($arr, $k, array());
}

// Equivalent to Phabricator `id`. Shuts up FB-internal Hack linters.
function bistro_id($v) {
  return $v;
}

function array_set_once(&$arr, $key, $val) {
  if (isset($arr[$key])) {
    throw new Exception($key.' was already set');
  }
  $arr[$key] = $val;
}

function bistro_ends_with(/* string */ $haystack, /* string */ $needle) {
  $len = strlen($needle);
  return $len === 0 || substr($haystack, -$len) === $needle;
}

function bistro_make_loadable_radio_button($base_class) {
  $prefix_to_class = call_user_func(
    array($base_class, 'getNameToClass'), $base_class);

  $radio = new AphrontFormRadioButtonControl();
  foreach ($prefix_to_class as $prefix => $class) {
    $radio->addButton(
      $prefix,
      bistro_ucwords(call_user_func(array($class, 'getTitle'))),
      call_user_func(array($class, 'getDescription')));
  }
  return $radio;
}

function bistro_make_dropdown(array $values) {
  return id(new AphrontFormSelectControl())
    ->setOptions($values);
}

/**
 * HTML checkboxes lack an "unspecified" state, and submit only when they
 * are checked.  I attempted to integrate them with prefs, and found it to
 * be sheer horror that adds far more complexity than it's worth.  So,
 * instead I use two radio buttons.
 */
function bistro_make_boolean_control() {
  return id(new AphrontFormRadioButtonControl())
    ->addButton('1', 'yes', '')
    ->addButton('0', 'no', '');
}

// Used to hide the form for a query pref.
function bistro_make_hidden_text_control() {
  return newv('AphrontFormTextControl', array())->setHidden(true);
}

/**
 * This is a Anthill-specific feature: its histogram semantics return
 * fractional node counts for the non-bottom levels, so we need to render
 * them.  Hiding .00 is good for integer-only Bistro, and for bottom Anthill
 * levels. Also see the round() in getJSData.
 */
function node_number_format($n) {
  return round($n, 2);
}

function bistro_decode_status_bits($encoded) {
  $encoded = strval($encoded);  // '1' becomes 1 in array keys
  $bits = BistroMonitor2JobSummary::letterToBits($encoded[0]);
  if (strlen($encoded) > 1) {
    $bits |= BistroMonitor2JobSummary::letterToBits($encoded[1]) << 6;
  }
  return $bits;
}

// Use instead of explode(':', $hp), since IPv6 addresses have colons.
function bistro_split_hostport($hp) {
  list($revport, $revhost) = explode(':', strrev($hp), 2);
  return array(strrev($revhost), strrev($revport));
}

function bistro_parse_hostport_list($hostports_str) {
  $data = trim(preg_replace('/[ \t\n\r,;]+/', ' ', $hostports_str));
  if (!$data) {
    return array();
  }
  $hostports = array();
  foreach (explode(' ', $data) as $hp_str) {
    $hp = bistro_split_hostport($hp_str);
    if (count($hp) !== 2) {
      throw new Exception($hp_str.' must have the form host:port');
    }
    // Future: It would be more sensible to do validation & FQDN stuff here,
    // so that all possible fetchers, and not just MultiCurl ones are
    // protected from the shenanigans that MultiCurl worries about.  The
    // cleanest way to achieve this would actually be to return a string
    // from here, and then to do the parsing together with
    // bistro_hostports_validate_and_make_fqdn() in BistroHostPortSource.
    $hostports[] = $hp[0].':'.intval($hp[1]);
  }
  return $hostports;
}

function bistro_canonical_host_id_uncached($host) {
  $dns_recs = dns_get_record($host);
  if ($dns_recs) {
    // Find the lexicographically largest (in binary) IP of each type.
    //
    // ASIDE: Could it be a better / more legible strategy to get the
    // lexicographically largest 'host' entry from the DNS records?
    $ipv6 = null;
    $ipv4 = null;
    foreach ($dns_recs as $dns_rec) {
      if (idx($dns_rec, 'type') === 'A' && isset($dns_rec['ip'])) {
        $binary_ip = inet_pton($dns_rec['ip']);
        if ($ipv4 === null || $binary_ip > $ipv4) {
          $ipv4 = $binary_ip;
        }
      } else if (idx($dns_rec, 'type') === 'AAAA' && isset($dns_rec['ipv6'])) {
        $binary_ip = inet_pton($dns_rec['ipv6']);
        if ($ipv6 === null || $binary_ip > $ipv6) {
          $ipv6 = $binary_ip;
        }
      }
    }
    if ($ipv6 !== null) {  // IPv6 is canonical if it's available
      return 'ipv6:'.$ipv6;
    }
    if ($ipv4 !== null) {
      return 'ipv4:'.$ipv4;
    }
  }
  return 'unresolved:'.$host;
}

// Returns a binary string to compare hostnames in a canonical way.
function bistro_canonical_host_id($host) {
  // Cache the DNS lookups since they can be pretty slow.
  static $host_to_canonical_id = array();
  if (!isset($host_to_canonical_id[$host])) {
    $host_to_canonical_id[$host] = bistro_canonical_host_id_uncached($host);
  }
  return $host_to_canonical_id[$host];
}

function bistro_canonical_hostport_id($hp) {
  list($host, $port) = bistro_split_hostport($hp);
  return bistro_canonical_host_id($host).':'.$port;
}

function bistro_parse_json_list_or_space_separated($str) {
  // The rule for job & node IDs is: try parsing them as a JSON array, if
  // that fails, assume the IDs are whitespace-separated.
  $maybe_ids = @json_decode($str, true);
  if (is_array($maybe_ids)) {
    // If we got a JSON object, we will also accept it and use the values,
    // but the user should not rely on this abuse continuing to work.
    return array_values($maybe_ids);
  }
  return preg_split("/\s+/", $str, 0, PREG_SPLIT_NO_EMPTY);
}
