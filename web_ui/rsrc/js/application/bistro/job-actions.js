/**
 * @provides javelin-behavior-bistro-job-actions-menu
 * @requires javelin-behavior
 *           javelin-dom
 */

JX.behavior('bistro-job-actions-menu', function(config) {
  JX.DOM.listen(
    JX.$(config.dropdownID),
    'change',
    null,
    function(e) {
      var v = JX.$(config.dropdownID).value;
      var cfg = JX.$(config.configID);
      var iframeDiv = JX.$(config.iframeDivID);
      JX.DOM.hide(cfg);
      JX.DOM.hide(iframeDiv);
      if (v === 'cfg') {
        JX.DOM.show(cfg);
      } else if (v.indexOf('iframe_') === 0) {
        var iframeURL = config.iframeURLs[parseInt(v.substr(7))];
        iframeDiv.getElementsByTagName('iframe')[0].src = iframeURL;
        iframeDiv.getElementsByTagName('a')[0].href = iframeURL;
        JX.DOM.show(iframeDiv);
      } else if (v.indexOf('link_') === 0) {
        window.location.href = config.linkURLs[parseInt(v.substr(5))];
      }
    });
});
