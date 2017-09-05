<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * Protects Bistro self-curl requests using strong cryptography with
 * automatically rotating per-Phabricator-user keys.
 *
 * This feels like gross overengineering given the "same IP" restriction, but
 *  - More layers of security is better than fewer.
 *  - I'm anticipating "same IP" being really bad for load balancing on
 *    multi-host Phabricator deployments.
 *
 * This attempts to implement best-practices encryption + integrity +
 * authentication. At the moment, this means CBC rijndael-128 + SHA-256.
 * Only change these if the cipher or the hash gets broken.
 *
 * P.S. This could reasonably be split into three mostly Bistro-independent
 * classes: one for crypto, and one for key rotation, and one for key
 * storage.
 */
final class BistroCurlProtection {

  // Must be false in production
  const DEBUG = false;

  const PREF_PROTECTION_KEYS = 'bistro-curl-crypto-security-keys';

  // Field names indexing into our array() of preferences
  //
  // Separate keys for request and response, to stop the request being
  // from being replayable as the response.
  const RESPONSE_KEYS = 1;
  const REQUEST_KEYS = 2;
  const PREV_RESPONSE_KEYS = 3;
  const PREV_REQUEST_KEYS = 4;
  const GENERATION_TIMESTAMP = 5;

  // Field names for indexing into the keys subarrays of preferences
  const ENCRYPTION_KEY = 1;
  const MAC_KEY = 2;

  // This must be larger than the maximum possible time a full request (with
  // all the curls) can be running.
  //
  // To test:
  //  - temporarily enable debug logging
  //  - temporarily set this to just over the time of your test
  //  - reload the page, and watch for 'Keys expired' messages
  //  - return debug logging and timeout to prod values (false + 1 week)
  //
  // Setting this to 1 week leaves a reasonable amount of time to debug issues.
  const EXPIRATION_SECONDS = 604800;

  // Used in a toggle to avoid passing function names.
  const MODE_PROTECT = 1;
  const MODE_UNPROTECT = 2;

  // BE EXTREMELY CAREFUL
  //
  // You need a very good reason and very good arguments to change these.
  //
  // The current encryption setup is based on expert recommendations.  You
  // should only change these if the cipher or the hash is proven
  // vulnerable.  If so, consult a crypto expert, and update the encryption
  // code to match -- it makes some assumptions about these constants.
  const PROTECTION_VERSION = 0;
  const ENCRYPTION_IV_SIZE = 16;
  const ENCRYPTION_BLOCK_SIZE = 16;
  const ENCRYPTION_ALGORITHM = 'rijndael-128';
  const ENCRYPTION_KEY_SIZE = 32;
  const ENCRYPTION_MODE = 'cbc';
  const MAC_ALGORITHM = 'sha256';
  const MAC_SIZE = 32;

  // Caches the keys arrays to avoid multiple trips to prefs in one request
  private static $usernameToKeys = array();

  //
  // Only do a fixed set of well-tested operations.
  //
  // All of them return null on failure, or a string on success.
  //

  public static function protectResponse($username, $str) {
    return self::tryKeys(
      self::MODE_PROTECT,
      $username,
      array(self::RESPONSE_KEYS, self::PREV_RESPONSE_KEYS),
      $str
    );
  }

  public static function protectRequest($username, $str) {
    return self::tryKeys(
      self::MODE_PROTECT,
      $username,
      array(self::REQUEST_KEYS, self::PREV_REQUEST_KEYS),
      $str
    );
  }

  public static function unprotectResponse($username, $str) {
    return self::tryKeys(
      self::MODE_UNPROTECT,
      $username,
      array(self::RESPONSE_KEYS, self::PREV_RESPONSE_KEYS),
      $str
    );
  }

  public static function unprotectRequest($username, $str) {
    return self::tryKeys(
      self::MODE_UNPROTECT,
      $username,
      array(self::REQUEST_KEYS, self::PREV_REQUEST_KEYS),
      $str
    );
  }

  //
  // Someday, put these in a separate utils package
  //

  public static function base64URIDecode($str) {
    return base64_decode(strtr($str, '_-', '+/'), /* strict = */ true);
  }

  public static function base64URIEncode($str) {
    return str_replace('=', '', strtr(base64_encode($str), '+/', '_-'));
  }

  //
  // Implementation details
  //

  private static function debugLog($message) {
    if (self::DEBUG) {
      error_log('BistroCurlProtection: '.$message);
    }
  }

  /**
   * We have to try the current and previous key, just in case some other
   * request expired the key from under us.
   */
  private static function tryKeys($mode, $username, array $key_types, $str) {
    $keys = self::loadKeysFromUsername($username);
    if ($keys === null) {
      self::debugLog('Did not get keys for mode '.$mode);
      return null;
    }
    foreach ($key_types as $key_type) {
      $key_pair = idx($keys, $key_type);
      if ($key_pair === null) {  // the *WithKeyPair functions assume non-null
        self::debugLog('No key type '.$key_type);
        continue;
      }
      $res = ($mode === self::MODE_PROTECT)
        ? self::protectWithKeyPair($key_pair, $str)
        : self::unprotectWithKeyPair($key_pair, $str);
      if ($res !== null) {
        return $res;
      }
      self::debugLog('Failed mode '.$mode.' with '.$key_type);
    }
    self::debugLog('All attempts failed');
    return null;
  }

  private static function preprocessKeyPair($key_pair) {
    $encryption_key = base64_decode($key_pair[self::ENCRYPTION_KEY]);
    if (strlen($encryption_key) !== self::ENCRYPTION_KEY_SIZE) {
      self::debugLog('Encryption key is the wrong size');
      return null;
    }

    $mac_key = base64_decode($key_pair[self::MAC_KEY]);
    if (strlen($mac_key) !== self::MAC_SIZE) {
      self::debugLog('MAC key is the wrong size');
      return null;
    }

    return array($encryption_key, $mac_key);
  }

  /**
   * BE VERY CAREFUL
   *
   * Please restrict your changes to fixing real bugs. This does not need
   * new features unless the underlying crypto gets broken.
   */
  private static function protectWithKeyPair($key_pair, $plain_text) {
    $keys = self::preprocessKeyPair($key_pair);
    if ($keys === null) {
      return null;  // Debug log was done above
    }
    list($encryption_key, $mac_key) = $keys;

    $iv = bistro_read_random_bytes(self::ENCRYPTION_IV_SIZE);
    if (strlen($iv) !== self::ENCRYPTION_IV_SIZE) {
      self::debugLog('Got the wrong number of IV bytes');
      return null;
    }

    // Encrypt data
    $cipher_text = mcrypt_encrypt(
      self::ENCRYPTION_ALGORITHM,
      $encryption_key,
      // Need the length here because the cipher pads the text with \000 to
      // its block size.  Could use a manual padding that codes how to
      // remove it, but that's not worth the trouble here, since the data
      // sizes are large.
      strlen($plain_text).':'.$plain_text,
      self::ENCRYPTION_MODE,
      $iv
    );

    // MAC data
    $mac = hash_hmac(
      self::MAC_ALGORITHM,
      $iv.$cipher_text,
      $mac_key,
      /* raw output = */ true
    );
    if (strlen($mac) !== self::MAC_SIZE) {
      self::debugLog('Computed MAC of wrong size');
      return null;
    }

    return self::base64URIEncode(
      pack('C', self::PROTECTION_VERSION).$mac.$iv.$cipher_text
    );
  }

  /**
   * BE VERY CAREFUL
   *
   * Please restrict your changes to fixing real bugs. This does not need
   * new features unless the underlying crypto gets broken.
   */
  private static function unprotectWithKeyPair($key_pair, $base64_input) {
    $keys = self::preprocessKeyPair($key_pair);
    if ($keys === null) {
      return null;  // Debug log was done above
    }
    list($encryption_key, $mac_key) = $keys;

    $input = self::base64URIDecode($base64_input);
    if (strlen($input) < 2) {  // Also fails on null, false
      self::debugLog('Input too short');
      return null;
    }

    $i1 = 1;  // version byte
    if (substr($input, 0, $i1) !== pack('C', self::PROTECTION_VERSION)) {
      self::debugLog('Bad version byte');
      return null;
    }

    $i2 = $i1 + self::MAC_SIZE;
    $mac = substr($input, $i1, self::MAC_SIZE);
    if (strlen($mac) !== self::MAC_SIZE) {
      self::debugLog('Bad MAC size');
      return null;
    }

    $i3 = $i2 + self::ENCRYPTION_IV_SIZE;
    $iv = substr($input, $i2, self::ENCRYPTION_IV_SIZE);
    if (strlen($iv) !== self::ENCRYPTION_IV_SIZE) {
      self::debugLog('Bad IV size');
      return null;
    }

    $cipher_text = substr($input, $i3);
    $len = strlen($cipher_text);
    if ($len < 1 || $len % self::ENCRYPTION_BLOCK_SIZE !== 0) {
      self::debugLog('Bad ciphertext size');
      return null;
    }

    $expected_mac = hash_hmac(
      self::MAC_ALGORITHM,
      $iv.$cipher_text,
      $mac_key,
      /* raw output = */ true
    );
    if (!self::areMACsEqual($mac, $expected_mac)) {
      self::debugLog('Invalid MAC');
      return null;
    }

    $plain_text_with_length = mcrypt_decrypt(
      self::ENCRYPTION_ALGORITHM,
      $encryption_key,
      $cipher_text,
      self::ENCRYPTION_MODE,
      $iv
    );
    if (strlen($plain_text_with_length) < 2) {  // Also fails on null, false
      self::debugLog('Plaintext too short to contain length prefix');
      return null;
    }

    list($length, $padded_text) = explode(':', $plain_text_with_length, 2);
    if ($length !== strval(intval($length))) {
      self::debugLog('Plaintext length is not an integer');
      return null;
    }
    $length = intval($length);

    $padded_len = strlen($padded_text);
    if ($padded_len < $length ||
        $padded_len > $length + self::ENCRYPTION_BLOCK_SIZE) {
      self::debugLog('Padded length does not match plaintext length');
      return null;
    }

    return substr($padded_text, 0, $length);
  }

  /**
   * Compare MACs in a constant amount of time in order to avoid leaking
   * timing information as in:
   *  http://rdist.root.org/2009/05/28/timing-attack-in-google-keyczar-library/
   */
  private static function areMACsEqual($mac, $expected_mac) {
    if (strlen($mac) != strlen($expected_mac)) {
      return false;
    }

    $ret = 0;
    for ($i = strlen($mac) - 1; $i >= 0; $i--) {
      $ret |= (ord($mac[$i]) ^ ord($expected_mac[$i]));
    }
    return $ret === 0;
  }

  // Returns null on failure
  private static function loadKeysFromUsername($username) {
    // Lexically valid username
    if (!PhabricatorUser::validateUsername($username)) {
      self::debugLog('Invalid username');
      return null;
    }
    // If we've cached a null, there was a prior error
    if (!array_key_exists($username, self::$usernameToKeys)) {
      // Username with DB entry
      $user = bistro_phabricator_user_from_name($username);
      if ($user) {
        self::$usernameToKeys[$username] = self::refreshKeysForUser($user);
      } else {
        self::debugLog('No such user');
        self::$usernameToKeys[$username] = null;
      }
    }
    return idx(self::$usernameToKeys, $username);
  }

  private static function generateKeys() {
    self::debugLog('Generating keys');
    // Must base64_encode all keys because Phabricator prefs cannot store
    // binary data.
    $keys = array(
      self::RESPONSE_KEYS => array(
        self::ENCRYPTION_KEY => base64_encode(
          bistro_read_random_bytes(self::ENCRYPTION_KEY_SIZE)
        ),
        self::MAC_KEY =>
          base64_encode(bistro_read_random_bytes(self::MAC_SIZE))),
      self::REQUEST_KEYS => array(
        self::ENCRYPTION_KEY => base64_encode(
          bistro_read_random_bytes(self::ENCRYPTION_KEY_SIZE)
        ),
        self::MAC_KEY =>
          base64_encode(bistro_read_random_bytes(self::MAC_SIZE))));
    // I can't imagine when we would fail to get the right number of bytes
    foreach (array(
      array(
        self::RESPONSE_KEYS, self::ENCRYPTION_KEY, self::ENCRYPTION_KEY_SIZE),
      array(self::RESPONSE_KEYS, self::MAC_KEY, self::MAC_SIZE),
      array(
        self::REQUEST_KEYS, self::ENCRYPTION_KEY, self::ENCRYPTION_KEY_SIZE),
      array(self::REQUEST_KEYS, self::MAC_KEY, self::MAC_SIZE)) as $check) {
      list($key_set, $key_type, $expected_size) = $check;
      $key_size = strlen(base64_decode($keys[$key_set][$key_type]));
      if ($key_size !== $expected_size) {
        self::debugLog(
          'Got '.$key_size.' !== '.$expected_size.' bytes for ['.$key_set.
          ']['.$key_type.'] key'
        );
        return null;
      }
    }
    $keys[self::GENERATION_TIMESTAMP] = time();  // Post-loop to appease Hack
    return $keys;
  }

  private static function refreshKeysForUser(PhabricatorUser $user) {
    $preferences = bistro_load_user_preferences($user);
    $all_keys = $preferences->getPreference(self::PREF_PROTECTION_KEYS);
    // An obsolete keys format had no generation time
    if ($all_keys === null || !isset($all_keys[self::GENERATION_TIMESTAMP])) {
      $all_keys = self::generateKeys();
    } else if (
      // This allows us to expire all keys by pushing code
      time() > $all_keys[self::GENERATION_TIMESTAMP] + self::EXPIRATION_SECONDS
    ) {
      // TODO(lo-pri): This might have some race conditions (i.e. two
      // simultaneous rotations leave one set of keys invalid), but it's not
      // worth fixing them until somebody complains.
      self::debugLog('Keys expired, rotating');
      $all_keys = self::generateKeys();
      if ($all_keys !== null) {
        $all_keys += array(
          self::PREV_RESPONSE_KEYS => $all_keys[self::RESPONSE_KEYS],
          self::PREV_REQUEST_KEYS => $all_keys[self::REQUEST_KEYS]);
      }
    } else {
      return $all_keys;  // avoid the write, since nothing changed.
    }

    if ($all_keys === null) {
      return null;  // Error already logged in generateKeys()
    }

    $preferences->setPreference(self::PREF_PROTECTION_KEYS, $all_keys);
    bistro_save_user_preferences($preferences, /* is_get_request = */ true);

    return $all_keys;
  }

}
