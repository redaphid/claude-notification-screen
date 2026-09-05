import { readFileSync } from 'fs';

// Pull the REAL encodeFrame out of the deployed page - not a copy of it.
const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const m = html.match(/const FRAME_BYTES = 8;[\s\S]*?\n  }\n\n  const hex/);
if (!m) { console.log('FAIL: could not extract encoder from page'); process.exit(1); }
const src = m[0].replace(/\n  const hex$/, '');
const encodeFrame = new Function(src + '\n return encodeFrame;')();

const hex = (b) => Array.from(b).map(x => x.toString(16).padStart(2, '0')).join(' ');
let pass = 0, fail = 0;
function check(name, got, want) {
  if (got === want) { pass++; console.log('  ok   ' + name + ' = ' + got); }
  else { fail++; console.log('  FAIL ' + name + ' got ' + got + ' want ' + want); }
}

console.log('frame length + layout:');
const f = encodeFrame({ bass: 1, mid: 0, treble: 0.5, energy: 0.25, beat: 1, shader: 3, seq: 1 });
check('byte length', f.length, 8);
check('bass 1.0 -> 255', f[0], 255);
check('mid 0.0 -> 0', f[1], 0);
check('treble 0.5 -> 128', f[2], 128);
check('energy 0.25 -> 64', f[3], 64);
check('beat', f[4], 1);
check('shader', f[5], 3);

console.log('seq is little-endian uint16 (badge reads it LE):');
const s = encodeFrame({ bass: 0, mid: 0, treble: 0, energy: 0, beat: 0, shader: 0, seq: 0x0102 });
check('seq low byte first', s[6], 0x02);
check('seq high byte second', s[7], 0x01);

console.log('clamping - out of range must not corrupt neighbouring bytes:');
const c = encodeFrame({ bass: 5, mid: -3, treble: 0, energy: 0, beat: 0, shader: 0, seq: 0 });
check('bass 5.0 clamps to 255', c[0], 255);
check('mid -3.0 clamps to 0', c[1], 0);

console.log('seq wrap at uint16:');
const w = encodeFrame({ bass: 0, mid: 0, treble: 0, energy: 0, beat: 0, shader: 0, seq: 0x1FFFF });
check('0x1FFFF masks to 0xFFFF lo', w[6], 0xff);
check('0x1FFFF masks to 0xFFFF hi', w[7], 0xff);

console.log('negative control (prove these assertions can fail):');
const n = encodeFrame({ bass: 0, mid: 0, treble: 0, energy: 0, beat: 0, shader: 0, seq: 0 });
console.log('  all-zero frame = ' + hex(n) + (hex(n) === '00 00 00 00 00 00 00 00' ? '  (as expected)' : '  UNEXPECTED'));
const bad = (n[0] === 255);
console.log('  asserting zero-bass==255 would ' + (bad ? 'WRONGLY PASS' : 'correctly fail') + ' -> checks are real');

console.log('\n' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
