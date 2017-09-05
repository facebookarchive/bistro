<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @emails oncall+phabricator
 */

final class BistroHTTPTaskLogViewController extends BistroController {

  const NOT_A_LINE_ID = -1;

  private $prefs;

  private function getHostports($hp_source) {
    $override_hostports = bistro_parse_hostport_list(
      $this->prefs->get(BistroTaskLogPrefs::PREF_HOSTPORTS));
    $orig_hostports = $hp_source->parse();
    if (!$override_hostports) {
      bistro_monitor_log()
        ->info('Getting logs from '.$hp_source->getSummary());
      return $orig_hostports;
    }

    // Check that override hostports are present in the original hostport
    // source.  If one of the overrides is unknown, the likeliest cause is
    // that the requested URL is a bit old, and one of the schedulers has
    // since been restarted on a different host or port.  Our fallback is to
    // query all schedulers.  This is slower, but should work reliably.
    $orig_hostport_ids = array_fill_keys(
      array_map(fun('bistro_canonical_hostport_id'), $orig_hostports), 1);
    foreach ($override_hostports as $hp) {
      if (!isset($orig_hostport_ids[bistro_canonical_hostport_id($hp)])) {
        bistro_monitor_log()->warn(
          'The override hostport '.$hp.' was not in the list of hostports '.
          'provided by your hostport source, so we fell back to querying all '.
          count($orig_hostports).' scheduler hostports.');
        return $orig_hostports;
      }
    }

    if (count($override_hostports) < count($orig_hostports)) {
      bistro_monitor_log()->info(
        'For performance, we are querying only '.count($override_hostports).
        ' out of the '.count($orig_hostports).' available scheduler '.
        'hostports ('.implode(', ', $override_hostports).'). To query all '.
        'schedulers, click "Edit entire query", clear "Only get logs from '.
        'these host:ports", and try again.');
    }
    return $override_hostports;
  }

  private function getLineIDOrTimestamp() {
    $timestamp = null;
    $line_id = $this->prefs->get(BistroTaskLogPrefs::PREF_LINE_ID);
    if ($line_id === '') {
      $line_id = null;
      $timestamp_str = trim($this->prefs->get(BistroTaskLogPrefs::PREF_TIME));
      if ($timestamp_str !== '') {
        $int_time = intval($timestamp_str);
        // Our custom interpretation for integer inputs is much more useful
        // than strtotime("1980"), which returns "the current date + time in
        // the given year".
        if (strval($int_time) == $timestamp_str) {
          $timestamp = ($int_time > 0) ? $int_time : (time() + $int_time);
        } else {
          $timestamp = strtotime($timestamp_str);
          if ($timestamp === false) {
            throw new Exception('Cannot parse timestamp "'.$timestamp_str.'"');
          }
        }
      }
    }
    return array($line_id, $timestamp);
  }

  private function makeQuery($line_id, $timestamp) {
    $sort =
      $this->prefs->get(BistroTaskLogPrefs::PREF_ASCENDING) ? 'asc' : 'desc';
    $jobs = $this->prefs->get(BistroTaskLogPrefs::PREF_JOBS);
    $nodes = $this->prefs->get(BistroTaskLogPrefs::PREF_NODES);
    $regex = $this->prefs->get(BistroTaskLogPrefs::PREF_REGEX_FILTER);

    $query = array();
    foreach (array(
      'stdout' => BistroTaskLogPrefs::PREF_SHOW_STDOUT,
      'stderr' => BistroTaskLogPrefs::PREF_SHOW_STDERR,
      'statuses' => BistroTaskLogPrefs::PREF_SHOW_STATUSES) as $stream => $pref_id) {
      if (!$this->prefs->get($pref_id)) {
        continue;
      }
      $subquery = array(
        'handler' => 'task_logs',
        'log_type' => $stream,
        'sort' => $sort);
      if ($jobs) {
        $subquery['jobs'] = bistro_parse_json_list_or_space_separated($jobs);
      }
      if ($nodes) {
        $subquery['nodes'] = bistro_parse_json_list_or_space_separated($nodes);
      }
      if ($regex) {
        $subquery['regex_filter'] = $regex;
      }
      if ($timestamp !== null) {
        $subquery['time'] = $timestamp;
      }
      if ($line_id !== null) {
        $subquery['line_id'] = $line_id;
      }
      $query[$stream] = $subquery;
    }
    if (!$query) {
      throw new Exception('Choose at least one log stream to show.');
    }
    return $query;
  }

  /**
   * Query all specified Bistro instances for log lines. Merge the log
   * lines, and figure out the line_id to use to get the next page of the
   * log.
   *
   * This function works analogously to Bistro's WorkerRunner::getJobLogs().
   *
   * @return array(
   *   is query unbounded?,
   *   next line_id,
   *   array(array(stream, job, node, time, line, line_id), ...)
   * )
   */
  private function getLogLines($hp_source) {
    list($query_line_id, $query_timestamp) = $this->getLineIDOrTimestamp();
    $query = $this->makeQuery($query_line_id, $query_timestamp);

    $next_line_id = self::NOT_A_LINE_ID;
    $stream_to_lines_lists = array();
    $is_ascending = $this->prefs->get(BistroTaskLogPrefs::PREF_ASCENDING);

    // Use MultiCurl to fetch logs from all Bistro instances in parallel.
    $multi_curl_fetcher = new BistroMonitor2HTTPMultiCurlClient();
    foreach ($multi_curl_fetcher->getSuccessfulHostportResponses(
        $this->prefs, $this->getHostports($hp_source), json_encode($query))
          as $hp => $response_str) {

      list($response, $_is_complete) =
        parse_monitor2_response($query, $hp, $response_str);
      // Even if some streams had errors, there's a chance the others did not.
      foreach ($response as $stream => $subresponse) {
        if (!$subresponse) {
          continue;  // In the absence of lines, Bistro returns {}
        }
        $stream_to_lines_lists[$stream][] = $subresponse['lines'];
        $cur_next_line_id = intval($subresponse['next_line_id']);
        // Pick the most restrictive next_line_id of all the Bistro hosts.
        if (
          // Bistro returns NOT_A_LINE_ID to mark "no more lines", so we
          // must not use it to restrict the "next line ID".
          ($cur_next_line_id !== self::NOT_A_LINE_ID) && (
            // Only matches initially, due to the previous test.
            ($next_line_id === self::NOT_A_LINE_ID) ||
            ($is_ascending && $next_line_id > $cur_next_line_id) ||
            (!$is_ascending && $next_line_id < $cur_next_line_id)
          )
        ) {
          $next_line_id = $cur_next_line_id;
        }
      }
    }

    // Filter out log lines that are beyond the most restrictive "next line ID"
    $out_lines = array();
    foreach ($stream_to_lines_lists as $stream => $lines_lists) {
      foreach ($lines_lists as $lines) {
        foreach ($lines as $line) {
          $line[4] = intval($line[4]);
          if (
            ($next_line_id === self::NOT_A_LINE_ID) ||
            ($line[4] === self::NOT_A_LINE_ID) ||
            ($is_ascending && $line[4] < $next_line_id) ||
            (!$is_ascending && $line[4] > $next_line_id)
          ) {
            array_unshift($line, $stream);
            $out_lines[] = $line;
          }
        }
      }
    }

    // Always display lines from oldest to newest
    return array(
      $query_line_id === null && $query_timestamp === null,
      $next_line_id,
      isort($out_lines, 5));
  }

  private function renderLogErrors(array $errors) {
    if (!$errors) {
      return null;
    }
    $error_boxes = array();
    foreach ($errors as $error => $stream_to_time) {
      $from_arr = array();
      foreach ($stream_to_time as $stream => $time_us) {
        $from_arr[] = 'from '.$stream.' at '.strftime('%c', $time_us);
      }
      $error_boxes[] = phabricator_tag(
        'div', array(), array(
          phabricator_tag(
            'div',
            array('class' => 'log-error-source'),
            'Error '.implode(', ', $from_arr).': '
          ),
          $error));
    }
    return
      phabricator_tag('div', array('class' => 'log-errors'), $error_boxes);
  }

  private function renderMetadataItem($kind, $cur, $prev) {
    return phabricator_tag(
      'span',
      array('class' => 'log-'.$kind.(
        $cur === $prev ? '' : ' log-line-meta-changed')),
      $cur);
  }

  private function renderLogLinesAndErrors(array $raw_lines) {
    require_celerity_resource('bistro-task-logs');
    list($prev_stream, $prev_job, $prev_node) = array(null, null, null);
    $errors = array();
    $lines = array();
    foreach ($raw_lines as $raw_line) {
      list($stream, $job, $node, $time_us, $val) = $raw_line;

      // Since each error will likely show up for all 3 streams, do not
      // render errors inline.  Instead, aggregate into redboxes that will
      // precede the logs.
      if ($job === '' && $node === '') {
        $errors[$val][$stream] = $time_us;
        continue;
      }

      // If the job, node, or stream type changes, break out a special
      // "metadata rendering" row.  This avoids the problem of constantly
      // showing the same data in single-job or single-node logs.
      $line = array();
      if ($prev_stream !== $stream
          || $prev_job !== $job
          || $prev_node !== $node) {
        $line[] = $this->renderMetadataItem('stream', $stream, $prev_stream);
        $line[] = $this->renderMetadataItem('job', $job, $prev_job);
        $line[] = $this->renderMetadataItem('node', $node, $prev_node);
      }
      if ($line) {
        $line = array(
          phabricator_tag('div', array('class' => 'log-line-meta'), $line));
      }
      $line[] = phabricator_tag(  // Append a space for nicer copy-pasting
        'span', array('class' => 'log-time'), strftime('%c', $time_us).' ');

      // Use CSS to differentiate log lines that end with \n from those that
      // do not.
      $val_len = strlen($val);
      if ($val_len && $val[$val_len - 1] === "\n") {
        $line[] = phabricator_tag(
          'pre',
          array('class' => 'has-newline log-value'),
          substr($val, 0, $val_len - 1));
      } else {
        $line[] = phabricator_tag('pre', array('class' => 'log-value'), $val);
      }

      // The log line is colored differently depending on the stream type.
      $lines[] = phabricator_tag(
        'div', array('class' => 'log-stream-'.$stream), $line);

      list($prev_stream, $prev_job, $prev_node) = array($stream, $job, $node);
    }
    return array(
      $this->renderLogErrors($errors),
      phabricator_tag('div', array('class' => 'log-lines'), $lines));
  }

  private function renderPager($is_unbounded, $next_line_id) {
    $request = $this->getRequest();
    $is_ascending = $this->prefs->get(BistroTaskLogPrefs::PREF_ASCENDING);

    $oldest_text = "|\xe2\x97\x80";
    $fwd_text = "\xe2\x97\x80";
    $back_text = "\xe2\x96\xb6";
    $newest_text = "\xe2\x96\xb6|";

    // The action of the "older" & "newer" buttons is reversed by this flag.
    if ($is_ascending) {
      list($fwd_text, $back_text) = array($back_text, $fwd_text);
    }
    $showing_start = $next_line_id === self::NOT_A_LINE_ID;
    $showing_end = $is_unbounded;
    $fwd = $this->prefs->renderUpdateForm(
      $request,
      array(
        BistroTaskLogPrefs::PREF_TIME => null,  // Unset any timestamp
        BistroTaskLogPrefs::PREF_LINE_ID => $next_line_id),
      id(new AphrontFormSubmitControl())
        ->setValue($fwd_text)
        ->setDisabled($showing_start));
    $back = $this->prefs->renderUpdateForm(
      $request,
      array(BistroTaskLogPrefs::PREF_ASCENDING => !!!$is_ascending),
      id(new AphrontFormSubmitControl())
        ->setValue($back_text)
        ->setDisabled($showing_end));
    // Don't switch the button locations around when the direction changes.
    if ($is_ascending) {
      list($fwd, $back) = array($back, $fwd);
      list($showing_start, $showing_end) = array($showing_end, $showing_start);
    }

    // Getting "oldest" and "newest" does not depend on $is_ascending.
    $start = $this->prefs->renderUpdateForm(
      $request,
      array(
        BistroTaskLogPrefs::PREF_TIME => null,
        BistroTaskLogPrefs::PREF_LINE_ID => null,
        BistroTaskLogPrefs::PREF_ASCENDING => 1),
      id(new AphrontFormSubmitControl())
        ->setValue($oldest_text)
        ->setDisabled($showing_start));
    $end = $this->prefs->renderUpdateForm(
      $request,
      array(
        BistroTaskLogPrefs::PREF_TIME => null,
        BistroTaskLogPrefs::PREF_LINE_ID => null,
        BistroTaskLogPrefs::PREF_ASCENDING => 0),
      id(new AphrontFormSubmitControl())
        ->setValue($newest_text)
        ->setDisabled($showing_end));

    return phabricator_tag('div', array('class' => 'log-pager'), array(
      phabricator_tag('span', array('class' => 'log-pager-left'), array(
        phabricator_tag('span', array(), array($start)),
        phabricator_tag('span', array(), array($fwd)))),
      phabricator_tag('span', array(), array($back)),
      phabricator_tag('span', array(), array($end))));
  }

  protected function processRequest() {
    $request = $this->getRequest();
    $this->prefs = new BistroTaskLogPrefs($request);
    // The hostport source must be made first, since it initializes the prefs
    $hp_source = BistroHostPortSource::newFrom($this->prefs);

    // Gotta fetch task logs before rendering the bistro_monitor_log errors.
    $log_line_view = null;
    $log_error_view = null;
    $pager = null;
    try {
      list($is_unbounded, $next_line_id, $log_lines) =
        $this->getLogLines($hp_source);
      list($log_error_view, $log_line_view) =
        $this->renderLogLinesAndErrors($log_lines);
      $pager = $this->renderPager($is_unbounded, $next_line_id);
    } catch (Exception $e) {
      bistro_monitor_log()->error('Exception while getting logs:', $e);
    }

    $intro = new AphrontPanelView();
    $intro->setWidth(AphrontPanelView::WIDTH_FORM);
    $intro->setHeader('Bistro Task Logs');
    $rendered_object = bistro_render_markup(
      $request,
      "This page aggregates task logs across Bistro instances. You can:\n".
      "* Filter lines by job ID, node ID, or a regex of the content\n".
      "* Page through the logs chronologically, or jump to a specific ".
      "timestamp\n");
    $intro->appendChild($rendered_object);
    $intro->appendChild(phabricator_tag('br'));
    $intro->appendChild(
      phabricator_tag('table', array('width' => '100%'),
        phabricator_tag('tr', array(), array(
          phabricator_tag('td', array(),
            phabricator_tag(
              'a',
              array('class' => 'button', 'href' => '#queryPrefs'),
              'Scroll to view the current query')),
          phabricator_tag('td', array('align' => 'right'),
            $this->prefs->renderEditQueryButton($request, '/bistro/logs'))))));

    return $this->buildStandardPageResponse(
      array(
        bistro_monitor_log()->render(),
        $intro,
        $log_error_view,
        $pager,
        $log_line_view,
        $pager,
        phabricator_tag('a', array('id' => 'queryPrefs'), ''),
        $this->prefs->render()),
      array('title' => 'Bistro Task Logs'));
  }

}
