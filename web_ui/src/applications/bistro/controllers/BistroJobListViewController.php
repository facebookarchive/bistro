<?php
// Copyright 2004-present Facebook. All Rights Reserved.

/**
 * @emails oncall+phabricator
 */

final class BistroJobListViewController extends BistroController {

  const MAX_DUPLICATE_NODES_TO_SHOW = 100;

  private $fetcher;
  private $prefs;

  protected function processRequest() {
    $request = $this->getRequest();
    $this->prefs = new BistroJobListPrefs($request);
    $this->fetcher = BistroDataFetcher::newFrom(
      // This source must be constructed first, since it initializes the prefs
      BistroHostPortSource::newFrom($this->prefs),
      $this->prefs);

    $pager = $this->makePager();
    $this->loadJobIDs($pager);  // Tell the fetcher what jobs to get.

    // Must run before we render the error log.
    self::detectDuplicateNodeNames($this->fetcher->loadNodeNames());

    // Fetch and sort jobs.
    $sorted_jobs = $this->getJobsInRenderOrder($this->fetcher->loadJobs());

    $rendered_jobs = array();
    foreach ($sorted_jobs as $job) {
      $config_blob_id = celerity_generate_unique_node_id();
      $iframe_div_id = celerity_generate_unique_node_id();
      $was_modified = $job->modifiedTime->getConsensus() !==
        $job->createdTime->getConsensus();
      $job_info = array(
        $this->renderJobHeader($job, $config_blob_id, $iframe_div_id),
        $this->renderMeta('Depends on',
          $job->dependencies->renderDep(array($this, 'renderDependencyID'))),
        $this->renderMeta('Packages', $job->packageIDs->render()),
        $this->renderMeta('Path', $job->path->render()),
        $this->renderMeta('Owner', $job->owner->render()),
        $this->renderMeta('Priority', $job->priority->render()),
        // It's only useful to show this when it's inconsistent across
        // Bistro instances:
        $job->level->isInconsistent()
          ? $this->renderMeta('Level for tasks', $job->level->render()) : null,
        // Save space: show only the created time for never-modified jobs.
        $this->renderMeta(
          'Created'.($was_modified ? ' / Modified' : ''),
          array(
            $job->createdTime->render(),
            $was_modified ? array(' / ', $job->modifiedTime->render()) : null)),
        phabricator_tag(
          'div',
          array(
            'id' => $config_blob_id,
            'style' => 'display: none',  // Config blobs are shown by JS
          ),
          $this->renderMeta('Config', $job->config->render())),
        $this->renderIframeDiv($iframe_div_id),
        $job->summary->render($this->prefs));
      $rendered_jobs[] =
        phabricator_tag('div', array('class' => 'job-col'), $job_info);
    }

    require_celerity_resource('bistro-jobs');
    $panel = new AphrontPanelView();
    $panel->addClass('bistro-jobs');
    $panel->setHeader('Bistro Jobs');

    $mini_form_prefs = array(
      BistroJobListPrefs::PREF_JOB_ID_REGEX,
      BistroJobListPrefs::PREF_SORT_BY);
    $mini_form_controls = array(
      // Since BistroPrefs don't support having a default value be a function
      // of another pref value, we have to manually show the fallback here:
      $this->prefs->renderPrefControl(BistroJobListPrefs::PREF_JOB_ID_REGEX,
        $this->getJobIDRegex()),
      $this->prefs->renderPrefControlCurrentValue(
        BistroJobListPrefs::PREF_SORT_BY));
    if ($this->prefs->get(BistroJobListPrefs::PREF_JOBS)) {
      array_unshift($mini_form_prefs, BistroJobListPrefs::PREF_JOBS);
      array_unshift($mini_form_controls,
        $this->prefs->renderPrefControlCurrentValue(
          BistroJobListPrefs::PREF_JOBS));
      // Suppress PREF_DEPRECATED_JOB_IDS from the form, since it is
      // merged into JOBS, which we just set explicitly.
      // Future: delete this once PREF_DEPRECATED_JOB_IDS is gone.
      array_unshift(
        $mini_form_prefs, BistroJobListPrefs::PREF_DEPRECATED_JOB_IDS);
    }

    $panel->appendChild(phabricator_tag(
      'div',
      array('class' => 'job-filter'),
      array(
        // A green submit button to edit the current prefs via the query page.
        phabricator_tag('div', array('class' => 'bistro-jobs-edit-query-on-right'),
          $this->prefs->renderEditQueryButton($request, '/bistro')),
        // A two-control pref form for quick access.
        $this->prefs->renderUpdateForm(
          $request,
          // No hidden controls for these, since they have real controls below.
          array_fill_keys($mini_form_prefs, null),
          array(
            phabricator_tag('table', array('width'=>'100%'),
              phabricator_tag('tr', array(), array(
                phabricator_tag('td', array(), $mini_form_controls),
                phabricator_tag('td', array(),
                  phabricator_tag('button', array('type' => 'submit'), 'Update'))))))))));

    $panel->appendChild(phabricator_tag(
      'div',
      array('class' => 'fetcher-summary'),
      bistro_ucfirst($this->fetcher->getSummary())));
    $panel->appendChild($this->fetcher->loadErrors()->render());
    $panel->appendChild(bistro_id(new BistroJobSummaryView(
      $this->prefs, $sorted_jobs, $this->fetcher))->render());
    $panel->appendChild(bistro_monitor_log()->render());
    $panel->appendChild($pager);
    $panel->appendChild($rendered_jobs);
    $panel->appendChild($pager);

    // Forward to JS prefs that the logs endpoint should use for fetching logs
    require_celerity_resource('bistro-render-node-group-details');
    Javelin::initBehavior(
      'set-log-fetching-config', $this->prefs->getExplicitCommonPrefs());

    return $this->buildStandardPageResponse(
      array($panel, $this->prefs->render()),
      array('title' => 'Bistro Jobs'));
  }

  private function getJobIDRegex() {
    $job_id_regex = $this->prefs->get(BistroJobListPrefs::PREF_JOB_ID_REGEX);
    if ($job_id_regex) {
      return $job_id_regex;
    }
    $job_id_regex =
      $this->prefs->get(BistroJobListPrefs::PREF_DEFAULT_JOB_ID_REGEX);
    return preg_replace(
      '{%%%PHABRICATOR_USERNAME%%%}',
      $this->getRequest()->getUser()->getUsername(),
      $job_id_regex);
  }

  private function makePager() {
    $pager = new BistroPagerView();
    $pager->setOffset(
      $this->prefs->get(BistroJobListPrefs::PREF_PAGE_OFFSET));
    $pager->setPageSize(
        $this->prefs->get(BistroJobListPrefs::PREF_PAGE_SIZE));
    $pager->setURI(
      $this->getRequest()->getRequestURI(),
      BistroJobListPrefs::PREF_PAGE_OFFSET);
    return $pager;
  }

  private function loadJobIDs(BistroPagerView $pager) {
    $job_ids = bistro_parse_json_list_or_space_separated(
      $this->prefs->get(BistroJobListPrefs::PREF_JOBS));
    return
      $this->fetcher->loadJobIDs($job_ids, $pager, $this->getJobIDRegex());
  }

  private function renderMeta($label, $metadata) {
    if ($metadata === '' ||
        (is_array($metadata) && count(array_filter($metadata)) === 0)) {
      return null;
    }
    return phabricator_tag('div', array('class' => 'job-metadata'), array(
      phabricator_tag('p', array(), $label.':'), $metadata));
  }

  public function renderDependencyID($dep_id) {  // Is a callback, hence public
    return phabricator_tag(
      'a',
      array('href' => BistroJob::getURIForID($this->prefs, $dep_id)),
      $dep_id);
  }

  private function renderIframeDiv($iframe_div_id) {
    return phabricator_tag(
      'div',
      array(
        'class' => 'iframe-div',
        'id' => $iframe_div_id,
        'style' => 'display: none',  // Iframes are shown by JS
      ),
      array(
        phabricator_tag('div', array(), array(
          'This iframe lives ',
          phabricator_tag('b', array(), phabricator_tag('a', array(), 'here')),
          '. If it uses HTTP, you will may need to authorize '.
          'your browser to load it. In Chrome, use the "shield" icon '.
          'at the top-right.')),
        phabricator_tag('iframe')));
  }

  private function prepareLinksAndIframes(BistroJob $j) {
    $urls = array('link' => array(), 'iframe' => array());
    $options = array();
    $pref = $this->prefs->get(BistroJobListPrefs::PREF_JOB_ACTIONS);
    foreach (json_decode($pref, 1) as $entry) {
      $url = $entry['url'];
      $url = str_replace('{{job_id}}', $j->jobId, $url);
      $type = $entry['type'];
      $urls[$type][] = $url;
      $options[] = phabricator_tag('option', array(
        'value' => $type.'_'.(count($urls[$type]) - 1)), $entry['name']);
    }
    return array($urls, $options);
  }

  private function renderJobHeader(
    BistroJob $j, $config_blob_id, $iframe_div_id) {
    $dropdown_id = celerity_generate_unique_node_id();
    list($urls, $options) = $this->prepareLinksAndIframes($j);
    Javelin::initBehavior(
      'bistro-job-actions-menu',
      array(
        'dropdownID' => $dropdown_id,
        'configID' => $config_blob_id,
        'iframeDivID' => $iframe_div_id,
        'iframeURLs' => $urls['iframe'],
        'linkURLs' => $urls['link']));
    return phabricator_tag('h2', array(
      // Disabled jobs get an extra visual indicator
      'class' => (
        $j->enabled->getConsensus() && $j->isCurrentConsensus->getConsensus()
      ) ? '' : 'disabled-job'), array(
      phabricator_tag('a', array('href' => $j->getURI()), $j->jobId),
      phabricator_tag(
        'span',
        array('class' => 'job-state'),
        array(
          $j->enabled->render(),
          $j->isCurrentConsensus->render(),
          ' ',
          phabricator_tag('select', array('id' => $dropdown_id), array(
            phabricator_tag('option', array(
              'selected' => 'selected',
              'value' => ''), '[Job Actions]'),
            phabricator_tag('option', array('value' => 'cfg'), 'Show Config'),
            $options))))));
  }

  private static function detectDuplicateNodeNames($location_to_nodes) {
    $prev_nodes = array();
    $duplicates = array();
    $num_duplicates = 0;
    foreach ($location_to_nodes as $location => $nodes) {
      foreach ($nodes as $node) {
        if (isset($prev_nodes[$node])) {
          $duplicates[$node][$prev_nodes[$node]] = 1;
          $duplicates[$node][$location] = 1;
          ++$num_duplicates;
          if ($num_duplicates >= self::MAX_DUPLICATE_NODES_TO_SHOW) {
            bistro_monitor_log()->warn(
              'You have '.$num_duplicates.' or more duplicate node names. '.
              'If you are using replicas, you should definitely disable '.
              'the setting "get_node_names" for your Bistro deployment '.
              'by adding "get_node_names": false to your monitor preference '.
              'overrides.');
            break;
          }
        } else {
          $prev_nodes[$node] = $location;
        }
      }
    }
    /*
    // This code is 20-40% faster than the above, UNLESS your
    // array_intersect_key has a regression, which just happened in HPHP.
    foreach ($location_to_nodes as $location => $nodes) {
      $nodes = array_fill_keys($nodes, $location);
      foreach (array_intersect_key($nodes, $prev_nodes) as $node => $_) {
        $duplicates[$node][$prev_nodes[$node]] = 1;
        $duplicates[$node][$location] = 1;
      }
      $prev_nodes += $nodes;
    }
    */
    $rendered = '';
    foreach ($duplicates as $node => $locations) {
      $rendered .= $node.' in '.implode(', ', array_keys($locations)).'; ';
    }
    if ($rendered) {
      // This should really be an error() and start with:
      //
      // 'EXTREMELY DANGEROUS! Set num_workers to 0 to stop ALL jobs, and '.
      // 'contact bistro-eng immediately. '
      //
      // However, since we do not yet have distributed locking of nodes, we
      // will unavoidably see this transiently as nodes change hosts.
      bistro_monitor_log()->warn(
        'Contact bistro-eng if you see this warning frequently, or if '.
        'it does not go away within 2 hours. Different instances have '.
        'the same node name: '.$rendered.(
          ($num_duplicates < self::MAX_DUPLICATE_NODES_TO_SHOW)
            ? '' : ' ... -- only the first '.$num_duplicates.' are shown.'));
    }
  }

  private function parseSortOrder(/* string */ $sort_by_str) {
    $sort_by = explode(', ', $sort_by_str);
    $ret = array();
    foreach (array(
      array(BistroJobListPrefs::SORT_RUNNING_TASKS_FIRST),
      array(BistroJobListPrefs::SORT_ENABLED_FIRST),
      BistroJobListPrefs::$mainSortOrders) as $cur_sort_arr) {
      $sort_match = array_intersect($sort_by, $cur_sort_arr);
      if (!$sort_match) {
        $ret[] = null;
      } else {
        $ret[] = bistro_head($sort_match);
        if (count($sort_match) != 1) {
          bistro_monitor_log()->error(
            'Expected only one sort match, not '.json_encode($sort_match));
        }
      }
    }
    // Reconstruct the $sort_by_str to sanity-check the parse.  Since $prefs
    // constrains fields to the allowed values, this should never fire.
    $real_sort_by_str = implode(', ', array_filter($ret));
    if ($sort_by_str != $real_sort_by_str) {
      bistro_monitor_log()->error(
        'Interpreting sort order "'.$sort_by_str.'" as "'.
        $real_sort_by_str.'". This should not happen.');
    }
    return $ret;
  }

  private function getJobsInRenderOrder($jobs) {
    list(
      $most_running_first,
      $enabled_first,
      $main_sort) = $this->parseSortOrder(
      $this->prefs->get(BistroJobListPrefs::PREF_SORT_BY));

    // This flips the kind of sort we do, and how most_running & enabled work.
    $sort_ascending = true;
    if (in_array($main_sort, array(
      BistroJobListPrefs::SORT_NAME_DESCENDING,
      BistroJobListPrefs::SORT_RECENTLY_MODIFIED_FIRST,
      BistroJobListPrefs::SORT_RECENTLY_CREATED_FIRST))) {
      $sort_ascending = false;
    } else if (!in_array($main_sort, array(
      BistroJobListPrefs::SORT_NAME_ASCENDING,
      BistroJobListPrefs::SORT_RECENTLY_MODIFIED_LAST,
      BistroJobListPrefs::SORT_RECENTLY_CREATED_LAST))) {
      bistro_monitor_log()->error('Bad main_sort: '.$main_sort);
    }

    // Sorting a key map is way faster than uasort.
    $key_jobid_map = array();
    foreach ($jobs as $job_id => $job) {
      // The game plan is to append fixed-width string fields to the sort
      // key, causing lexical sort to do the right thing.
      $sort_key = '';

      // Option: First sort by the number of running tasks
      if ($most_running_first ===
          BistroJobListPrefs::SORT_RUNNING_TASKS_FIRST) {
        $num_running_shards = count($job->currentRuntimes);
        // Jobs with the most running shards go first
        if ($sort_ascending) {
          $sort_val = 2000000000 - $num_running_shards;  // ok on 32-bit PHP
          if ($sort_val < 0) {  // Should never happen
            bistro_monitor_log()->warn('Job '.$job_id.' has a bad sort key.');
          }
        } else {
          $sort_val = $num_running_shards;
        }
        // Adds 11 bytes: 10 digits, plus '1' if negative or '0' otherwise
        $sort_key .= sprintf('%011d', $sort_val);
      } else if ($most_running_first !== null) {
        bistro_monitor_log()->error(
          'Bad most_running_first: '.$most_running_first);
      }

      // Option: Enabled jobs with nothing running come before disabled ones
      if ($enabled_first ===
          BistroJobListPrefs::SORT_ENABLED_FIRST) {
        if ($sort_ascending) {
          $sort_key .= intval(!$job->enabled->getConsensus());
        } else {
          $sort_key .= intval(!!$job->enabled->getConsensus());
        }
      } else if ($enabled_first !== null) {
        bistro_monitor_log()->error('Bad enabled_first: '.$enabled_first);
      }

      // Only add to the key if sorting by timestamps; job name is always added
      if (in_array($main_sort, array(
        BistroJobListPrefs::SORT_RECENTLY_MODIFIED_FIRST,
        BistroJobListPrefs::SORT_RECENTLY_MODIFIED_LAST))) {
        $sort_key .= sprintf('%011d', $job->modifiedTime->getConsensus());
      } else if (in_array($main_sort, array(
        BistroJobListPrefs::SORT_RECENTLY_CREATED_FIRST,
        BistroJobListPrefs::SORT_RECENTLY_CREATED_LAST))) {
        $sort_key .= sprintf('%011d', $job->createdTime->getConsensus());
      } else if (!in_array($main_sort, array(
        BistroJobListPrefs::SORT_NAME_ASCENDING,
        BistroJobListPrefs::SORT_NAME_DESCENDING))) {
        bistro_monitor_log()->error('Bad main_sort: '.$main_sort);
      }

      $key_jobid_map[$job_id] = $sort_key.$job_id;
    }

    if ($sort_ascending) {
      asort($key_jobid_map);
    } else {
      arsort($key_jobid_map);
    }

    $ordered_jobs = array();
    foreach ($key_jobid_map as $job_id => $job_key) {
      $ordered_jobs[$job_id] = $jobs[$job_id];
    }

    return $ordered_jobs;
  }

}
