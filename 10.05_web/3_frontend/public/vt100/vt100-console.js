/* vt100-console.js: connects PCjs's VT100 to one of the board's console channels.

   The VT100's serial port finds what it is wired to through the machine's
   "connection" setting, serialPort->qunilator, by looking for a component of
   that id in PCjs.components. This puts one there before vt100.js loads: what
   the terminal transmits goes out on the console WebSocket as a binary frame,
   and BREAK as the channel's {"break":true} control message. What the guest
   prints comes back as binary frames and is handed to the serial port.

   The port's receive queue holds 4096 bytes and drops what overflows it, and
   the first frame after connecting is the channel's retained history, which
   can be larger. So received bytes wait here and are handed over a few at a
   time. The VT100 sends XOFF when its own input buffer fills and XON when it
   has caught up, as it does to a host, and this queue obeys them: bytes
   already on their way when the guest stops are held here instead of
   overrunning the terminal. While the history is being played in, what the
   terminal transmits goes no further: those are its answers to queries the
   guest made in the past, and they would land as stray input at the guest's
   current prompt.
*/
(function () {
  'use strict';

  var path = new URLSearchParams(location.search).get('path') || '/ws/console/0';
  var ws = null;
  var queue = [];      // received bytes not yet handed to the serial port
  var head = 0;
  var replayLeft = 0;  // bytes of replayed history still in the queue
  var port = null;     // the VT100's serial port, once the machine has built it
  var stopped = false; // the VT100 has sent XOFF

  function state(text, ok) {
    var el = document.getElementById('state');
    if (!el) return;
    el.textContent = text;
    el.className = ok ? 'ok' : 'down';
  }

  function send(data) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(data);
  }

  // What the VT100 transmits: a byte, or a string when text is pasted into it.
  function transmit(data) {
    var s = typeof data === 'number' ? String.fromCharCode(data & 0xff) : String(data);
    if (s === '\x13') stopped = true;
    else if (s === '\x11') stopped = false;
    if (replayLeft > 0) return true;
    var bytes = new Uint8Array(s.length);
    for (var i = 0; i < s.length; i++) bytes[i] = s.charCodeAt(i) & 0xff;
    send(bytes);
    return true;
  }

  var pcjs = window.PCjs = window.PCjs ||
      { 'machines': {}, 'components': [], 'commands': {}, 'files': null };
  pcjs['components'].push({
    'id': 'qunilator',
    'exports': {
      'connect': function () {},
      'receiveData': transmit,
      'receiveStatus': function () { return true; },
      // called with true when BREAK starts and false when it ends
      'receiveBreak': function (on) {
        if (on) send(JSON.stringify({ 'break': true }));
        return true;
      }
    }
  });

  function serialPort() {
    var vt = pcjs['machines']['vt100'];
    var dev = vt && vt['serialPort'];
    return dev && dev['exports'] && dev['exports']['receiveData'] ? dev : null;
  }

  // Bytes go to the port at the line rate, 9600 baud with a little to spare,
  // and no more than PORT_MAX of them wait in the port's own queue. That queue
  // drains into the terminal whether or not it has sent XOFF, so what waits in
  // it is what overruns the terminal while it scrolls. The allowance is capped
  // so a tab the browser has slowed down does not catch up in one burst.
  var BYTES_PER_MS = 0.94;
  var PORT_MAX = 16;
  var allowance = 0;
  var last = Date.now();
  function waiting(dev) {
    return dev['abReceived'] ? dev['abReceived'].length - dev['iReceived'] : 0;
  }
  setInterval(function () {
    var now = Date.now();
    allowance = Math.min(allowance + (now - last) * BYTES_PER_MS, 32);
    last = now;
    if (!port) port = serialPort();
    if (!port || stopped) return;
    var take = port['exports']['receiveData'];
    while (allowance >= 1 && head < queue.length && waiting(port) < PORT_MAX) {
      if (!take.call(port, queue[head])) break;   // its queue is full: try again later
      head++;
      allowance--;
      if (replayLeft > 0) replayLeft--;
    }
    if (head === queue.length) {
      queue = [];
      head = 0;
    } else if (head > 65536) {
      queue = queue.slice(head);
      head = 0;
    }
  }, 10);

  // The last screenful of the history is enough to show where the guest is.
  // A VT100 scrolls a few lines a second, so replaying all of it would keep
  // the screen busy for minutes.
  var REPLAY_LINES = 24;
  function lastLines(bytes, n) {
    for (var i = bytes.length - 1; i >= 0; i--)
      if (bytes[i] === 10 && n-- === 0) return bytes.subarray(i + 1);
    return bytes;
  }
  var replayNext = false;

  function connect() {
    var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + path;
    var sock = new WebSocket(url);
    ws = sock;
    sock.binaryType = 'arraybuffer';
    sock.onopen = function () {
      queue = [];
      head = 0;
      replayLeft = 0;
      stopped = false;
      replayNext = true;
      state('connected to ' + path, true);
    };
    sock.onmessage = function (e) {
      if (typeof e.data === 'string') {
        try {
          var msg = JSON.parse(e.data);
          // the end of the replayed history; with none, nothing is replayed
          if (msg.live === true) replayNext = false;
          if (msg.transfer === true) state('a file transfer is using the console', false);
          else if (msg.transfer === false) state('connected to ' + path, true);
        } catch (err) { /* not a control message */ }
        return;
      }
      var bytes = new Uint8Array(e.data);
      if (replayNext) {
        replayNext = false;
        bytes = lastLines(bytes, REPLAY_LINES);
        replayLeft = bytes.length;
      }
      for (var i = 0; i < bytes.length; i++) queue.push(bytes[i]);
    };
    sock.onclose = function () {
      if (ws === sock) ws = null;
      state('disconnected, retrying', false);
      setTimeout(connect, 2000);
    };
  }
  connect();
})();
