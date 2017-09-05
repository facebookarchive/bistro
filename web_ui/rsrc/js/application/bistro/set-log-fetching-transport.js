/**
 * @provides javelin-behavior-set-log-fetching-config
 */

JX.behavior('set-log-fetching-config', function(config) {
  // Don't know how else to pass this to bistroRenderNodeGroupDetails
  window.bistroLogFetchingConfig = config;

  JX.Stratcom.listen('click', 'bistro-kill-task', function(e) {
    e.kill();
    var message_elt = e.getNode('bistro-kill-task').previousSibling;
    JX.DOM.setContent(message_elt, '- killing, this may take a minute... ');
    new JX.Request('/bistro/kill/task', function(r) {
      JX.DOM.setContent(message_elt, JX.$H(r));
    })
      .addData(e.getNodeData('bistro-kill-task'))
      .send();
  });

});
