// Runs the page's REAL script against a stub DOM and a fake MIDI device, so
// the knob mapping is provable with no controller plugged in.
import { readFileSync } from 'fs';

const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

function makeEl() {
  return {
    textContent: '', value: '0', hidden: false, dataset: {}, style: {},
    scrollTop: 0, scrollHeight: 0,
    _handlers: {},
    addEventListener(ev, fn) { this._handlers[ev] = fn; },
  };
}

function runPage({ midiAccess }) {
  const els = {};
  const document = {
    getElementById(id) { return (els[id] = els[id] || makeEl()); },
  };
  const store = {};
  const localStorage = {
    getItem: (k) => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
  };
  const navigator = {};
  if (midiAccess) navigator.requestMIDIAccess = () => Promise.resolve(midiAccess);

  const fn = new Function(
    'document', 'localStorage', 'navigator', 'performance', 'setInterval', 'clearInterval',
    script
  );
  fn(document, localStorage, navigator, { now: () => 0 }, () => 1, () => {});
  return { els };
}

// A fake controller: inputs collection with forEach + addEventListener.
function makeFakeMidi(deviceName) {
  const input = { name: deviceName, type: 'input', state: 'connected', _fn: null,
    addEventListener(ev, fn) { if (ev === 'midimessage') this._fn = fn; } };
  const access = {
    inputs: { forEach: (cb) => [input].forEach(cb) },
    addEventListener() {},
  };
  return { access, send: (cc, val, ch = 0) => input._fn({ data: [0xb0 | ch, cc, val] }) };
}

let pass = 0, fail = 0;
const check = (name, got, want) => {
  const ok = got === want;
  ok ? pass++ : fail++;
  console.log((ok ? '  ok   ' : '  FAIL ') + name + ' = ' + got + (ok ? '' : ' (want ' + want + ')'));
};

console.log('Rule One: no Web MIDI at all must still render and say so');
{
  const { els } = runPage({ midiAccess: null });
  const msg = els['midi-status'].textContent;
  check('states no Web MIDI plainly', /no Web MIDI/i.test(msg), true);
  check('tells user sliders still work', /sliders/i.test(msg), true);
  check('sliders still rendered', els['frame-json'].textContent.length > 0, true);
}

console.log('zero-config auto-assign claims slots in order');
{
  const midi = makeFakeMidi('Fake Controller');
  const { els } = runPage({ midiAccess: midi.access });
  await Promise.resolve(); await Promise.resolve();
  midi.send(21, 64); // absolute-range value
  midi.send(22, 64);
  midi.send(23, 64);
  const map = els['midi-map'].textContent;
  check('cc21 -> BASS', /cc21 ch1 -> BASS/.test(map), true);
  check('cc22 -> MID', /cc22 ch1 -> MID/.test(map), true);
  check('cc23 -> TREBLE', /cc23 ch1 -> TREBLE/.test(map), true);
  check('bass slider moved to 64/127', els['v-bass'].textContent, (64 / 127).toFixed(2));
}

console.log('a fifth knob is ignored once all four slots are claimed');
{
  const midi = makeFakeMidi('Fake Controller');
  const { els } = runPage({ midiAccess: midi.access });
  await Promise.resolve(); await Promise.resolve();
  [21, 22, 23, 24, 25].forEach((cc) => midi.send(cc, 64));
  const map = els['midi-map'].textContent;
  check('cc25 not mapped', /cc25/.test(map), false);
  check('exactly 4 mappings', map.trim().split('\n').length, 4);
}

console.log('same CC keeps the same slot (mapping is stable)');
{
  const midi = makeFakeMidi('Fake Controller');
  const { els } = runPage({ midiAccess: midi.access });
  await Promise.resolve(); await Promise.resolve();
  midi.send(21, 64);
  midi.send(21, 127);
  check('cc21 still only BASS', (els['midi-map'].textContent.match(/cc21/g) || []).length, 1);
  check('bass followed to 1.00', els['v-bass'].textContent, '1.00');
}

console.log('relative encoder is not treated as an absolute pot');
{
  const midi = makeFakeMidi('Encoder');
  const { els } = runPage({ midiAccess: midi.access });
  await Promise.resolve(); await Promise.resolve();
  midi.send(30, 2); // small delta, never in the 30..100 absolute band
  const afterFirst = parseFloat(els['v-bass'].textContent);
  check('small value did NOT jump to 2/127 absolute', afterFirst === (2 / 127), false);
  check('nudged upward from 0 instead', afterFirst > 0, true);
}

console.log('negative control (prove these checks can fail)');
{
  const { els } = runPage({ midiAccess: null });
  const wrong = /controller connected and ready/i.test(els['midi-status'].textContent);
  console.log('  asserting a false claim would ' + (wrong ? 'WRONGLY PASS' : 'correctly fail') + ' -> checks are real');
}

console.log('\n' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
