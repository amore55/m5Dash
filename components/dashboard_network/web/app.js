/* Shared browser-side helpers for the device's configuration pages.
 *
 * AUTHENTICATION
 *
 * The device has no session store and no room for one. Instead the PIN travels in an X-Dash-Pin
 * header on every request and is verified per request against the salted hash in NVS. That is no
 * weaker than a session cookie would be here — both cross the same plain-HTTP LAN hop — and it
 * removes an entire class of state from the firmware.
 *
 * The PIN is held in sessionStorage: it survives moving between the pages, and is gone when the
 * browser tab closes. It is never written to localStorage, so it does not outlive the visit. */

var Dash = (function () {
  var PIN_KEY = 'dash-pin';

  function pin() { return sessionStorage.getItem(PIN_KEY) || ''; }
  function setPin(value) { sessionStorage.setItem(PIN_KEY, value); }
  function clearPin() { sessionStorage.removeItem(PIN_KEY); }

  function headers(extra) {
    var h = extra || {};
    var p = pin();
    if (p) { h['X-Dash-Pin'] = p; }
    return h;
  }

  /* Turn a non-OK response into a rejection carrying the device's own message, so callers can
   * show what actually went wrong rather than a generic failure. */
  function unwrap(res) {
    if (res.status === 401) {
      clearPin();
      var e = new Error('PIN required');
      e.needsPin = true;
      throw e;
    }
    return res.json().catch(function () {
      throw new Error('The dashboard sent a reply we could not read.');
    }).then(function (body) {
      if (!res.ok || (body && body.ok === false)) {
        throw new Error((body && body.error) || 'The dashboard rejected that.');
      }
      return body;
    });
  }

  function get(url) {
    return fetch(url, { headers: headers() }).then(unwrap);
  }

  function postForm(url, fields) {
    var body = Object.keys(fields).map(function (k) {
      return encodeURIComponent(k) + '=' + encodeURIComponent(fields[k]);
    }).join('&');
    return fetch(url, {
      method: 'POST',
      headers: headers({ 'Content-Type': 'application/x-www-form-urlencoded' }),
      body: body
    }).then(unwrap);
  }

  function say(text, kind, id) {
    var el = document.getElementById(id || 'msg');
    if (!el) { return; }
    el.textContent = text;
    el.className = 'msg ' + kind;
  }

  /* A row in a tappable list — used for both scanned networks and geocoding results. Built with
   * DOM calls rather than innerHTML because the text is a network name or a place name from an
   * external API, and neither is ours to trust. */
  function pickerItem(text, meta, onClick) {
    var b = document.createElement('button');
    b.type = 'button';

    var name = document.createElement('span');
    name.className = 'grow';
    name.textContent = text;
    b.appendChild(name);

    if (meta) {
      var m = document.createElement('span');
      m.className = 'meta';
      m.textContent = meta;
      b.appendChild(m);
    }

    b.addEventListener('click', onClick);
    var li = document.createElement('li');
    li.appendChild(b);
    return li;
  }

  return {
    get: get,
    postForm: postForm,
    say: say,
    pickerItem: pickerItem,
    pin: pin,
    setPin: setPin,
    clearPin: clearPin
  };
})();
