/**
 * @provides javelin-behavior-add-detail-toggle-listener
 * @requires javelin-behavior
 * @requires javelin-dom
 */

JX.behavior('add-detail-toggle-listener', function(configs) {
  var listener_node = document.getElementById(configs.listener_id);
  var detail_node = document.getElementById(configs.detail_id);
  var expanded_class = 'bistro-detail-toggle-expanded';

  JX.DOM.listen(
    listener_node,
    'click',
    null,
    function (e) {
      closing = listener_node === detail_node.last_clicked_node;
      JX.DOM.alterClass(listener_node, expanded_class, !closing);
      JX.DOM.alterClass(detail_node, expanded_class, !closing);
      if (closing) {
        // Close the current expansion
        detail_node.innerHTML = '';
        detail_node.last_clicked_node = undefined;
      } else {
        detail_node.innerHTML = configs.detail_html;
        // Apply custom JS to the detail node.
        if (configs.detail_js) {
          eval(configs.detail_js)(detail_node);
        }
        if (detail_node.last_clicked_node !== undefined) {
          // Switching from one expansion to another
          JX.DOM.alterClass(
            detail_node.last_clicked_node, expanded_class, false);
        }
        // Open a new expansion
        detail_node.last_clicked_node = listener_node;
      }
    });
});

