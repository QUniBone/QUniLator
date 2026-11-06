/* vt100-nvr.js: keeps the VT100's NVR in the browser, as the terminal keeps it
   in its ER1400 chip.

   The firmware reads its settings from the 100 NVR words at power-up and
   writes them back when SET-UP saves them (SHIFT-S). PCjs holds the words in
   VT100Chips.aNVRWords and sets them to DEC's factory values on every reset.
   This puts the saved words back after each reset, and saves them whenever
   the firmware writes or erases a word, so settings saved in SET-UP are there
   the next time a VT100 window opens in this browser for this board.

   Loaded after vt100-uncompiled.js, which defines VT100Chips, and before the
   machine is built.
*/
(function () {
  'use strict';

  var KEY = 'qunilator.vt100.nvr';
  var WORDS = 100;
  var MASK = VT100Chips.NVR.WORDMASK;

  function load() {
    try {
      var words = JSON.parse(window.localStorage.getItem(KEY));
      if (!Array.isArray(words) || words.length !== WORDS) return null;
      for (var i = 0; i < WORDS; i++)
        if (typeof words[i] !== 'number' || words[i] < 0 || words[i] > MASK) return null;
      return words;
    } catch (err) {
      return null;
    }
  }

  // The firmware writes the words one at a time; saving once they have all
  // gone in keeps it to one write per SET-UP save.
  var pending = null;
  function save(chips) {
    if (pending) clearTimeout(pending);
    pending = setTimeout(function () {
      pending = null;
      try {
        window.localStorage.setItem(KEY, JSON.stringify(chips.aNVRWords.slice(0, WORDS)));
      } catch (err) { /* no storage: the settings last until the window closes */ }
    }, 500);
  }

  var proto = VT100Chips.prototype;
  var onReset = proto.onReset;
  proto.onReset = function () {
    onReset.apply(this, arguments);
    var words = load();
    if (words) this.aNVRWords = words;
  };

  var doNVRCommand = proto.doNVRCommand;
  proto.doNVRCommand = function () {
    var cmd = (this.bNVRLatch >> 1) & 0x7;
    doNVRCommand.apply(this, arguments);
    if (cmd === VT100Chips.NVR.CMD.WRITE || cmd === VT100Chips.NVR.CMD.ERASE) save(this);
  };
})();
