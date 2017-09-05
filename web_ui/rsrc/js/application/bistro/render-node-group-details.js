/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * @provides bistro-render-node-group-details
 * @requires javelin-dom
 */

// TODO: I don't know jack about JS or Phabricator Javelin usage, but
// probably this shouldn't be at global scope.
function bistroRenderNodeGroupDetails(job, details, has_logs) {
  details = JSON.parse(details);
  var running_tasks = 'running_tasks' in details ? details.running_tasks : {};
  return function(elt) {
    for (var hp in details.hp_to_count_and_nodes) {
      // hp_to_count_and_nodes  format: [count, node, node, node ...]
      var count = details.hp_to_count_and_nodes[hp][0];
      // - remove 'count' - first item!
      // - sort string task ids naturally ['1', '2', ... '10']
      //   http://stackoverflow.com/a/38641281
      var nodes = details.hp_to_count_and_nodes[hp].splice(1).sort(
        new Intl.Collator(undefined, {numeric: true, sensitivity: 'base'})
          .compare
      );

      JX.DOM.appendContent(elt, count + ' from ' + hp);

      if (!has_logs) {
        // Just render names.
        for (var i = 0; i < nodes.length; ++i) {
          JX.DOM.appendContent(elt, nodes[i]);
        }
        continue;
      }

      // helper function: return an <a> anchor to the node's logs.
      var nodeLogsAnchor = function(node) {
        var uri;
        if (window.bistroLogFetchingConfig.fetcher === 'monitor2_thrift') {
          // DEPRECATED: Only used for PyAnthill
          uri = new JX.URI('/bistro/thrift_logs').setQueryParams({
            log_type: 'statuses',
            hostport: hp,
            jobs: job,
            nodes: node
          });
        } else {
          // Overwrite some properties. It's fine since we do for every link.
          window.bistroLogFetchingConfig.hostports = hp;
          // JSON instead of the alternative space-separated list, since
          // we shouldn't assume that jobs/nodes have no spaces in them.
          window.bistroLogFetchingConfig.jobs = JSON.stringify([job]);
          window.bistroLogFetchingConfig.nodes = JSON.stringify([node]);
          uri = new JX.URI('/bistro/logs/view').setQueryParams(
            window.bistroLogFetchingConfig
          );
        }
        return JX.$N('a', {href: uri.toString(), target: '_blank'}, node);
      };


      // For non running nodes: display as a list of links to logs.
      {
        var nonRunningNodes = nodes.filter(
          function(n) { return !(n in running_tasks); }
        );

        if (nonRunningNodes.length != 0) {
          // For non-running nodes just display the list of links to logs.
          JX.DOM.appendContent(elt, JX.$N('br'));
          for (var i = 0; i < nonRunningNodes.length; ++i) {
            JX.DOM.appendContent(elt, nodeLogsAnchor(nonRunningNodes[i]));
            JX.DOM.appendContent(elt, ', ');
          }
          JX.DOM.appendContent(elt, JX.$N('br'));
        }
      }

      // For running nodes display data in a table.
      var runningNodes = nodes.filter(
        function(n) { return n in running_tasks; }
      );

      var table = JX.$N('table');
      JX.DOM.appendContent(elt, table);

      for (var i = 0; i < runningNodes.length; ++i) {
        var node = runningNodes[i];

        // row with 4 columns: link to task, runtime info, host, kill button
        var tr = JX.$N('tr');
        JX.DOM.appendContent(table, tr);

        // Task
        {
          var tdTask = JX.$N('td', {style: {'text-align': 'right'}});
          JX.DOM.appendContent(tr, tdTask);
          JX.DOM.appendContent(tdTask, nodeLogsAnchor(node));
        }

        // Runtime
        {
          var tdRuntime = JX.$N('td', {style: {'text-align': 'right'}});
          JX.DOM.appendContent(tr, tdRuntime);
          // Show the runtime and worker shard (if available)
          var rt = running_tasks[node];
          var runtime = Math.round(Date.now() / 1000 - rt.start_time);
          JX.DOM.appendContent(tdRuntime, runtime + ' sec');
        }

        // Host
        {
          var tdHost = JX.$N('td');
          JX.DOM.appendContent(tr, tdHost);
          if ('worker_shard' in rt) {
            JX.DOM.appendContent(tdHost, rt.worker_shard);
          }
        }

        // Kill
        {
          var tdKill = JX.$N('td');
          JX.DOM.appendContent(tr, tdKill);

          // Kill-related messages are written here via .previousSibling
          JX.DOM.appendContent(tdKill, JX.$N('span'));
          // Render the "kill" link -- when clicked, sends an AJAX request
          JX.DOM.appendContent(tdKill, JX.$N(
            'a',
            { title: 'Kill task',
              sigil: 'bistro-kill-task',
              meta: { scheduler: hp, job: job, node: node }
            },
            '[KILL]'
          ));
        }

      }
    }
  };
}
