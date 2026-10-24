// commands.js - plain view commands said or typed in the headset, done at once without asking the AI: "play", "next
// frame", "show the temperature", "cut along z", "true size", "clear the pins". The whole sentence must be one of these;
// anything else ("show me where it is hottest") goes to the AI, which has the headset's tools for the rest.
//
// parse(words, fields) -> an action, or null. fields: [{id, label}] of the result shown (lab fields are names such as
// p, T, Q, B; a finite-element field has a label such as "von Mises stress").

const NUMBERS = { zero: 0, one: 1, two: 2, three: 3, four: 4, five: 5, six: 6, seven: 7, eight: 8, nine: 9, ten: 10,
  eleven: 11, twelve: 12, thirteen: 13, fourteen: 14, fifteen: 15, sixteen: 16, seventeen: 17, eighteen: 18,
  nineteen: 19, twenty: 20, thirty: 30, forty: 40, fifty: 50, sixty: 60, seventy: 70, eighty: 80, ninety: 90 };
// what a field is called aloud, for the names the lab gives its fields
const SPOKEN = { p: ['pressure'], T: ['temperature', 'heat'], Q: ['q', 'q criterion', 'vortices', 'vortex cores'],
  B: ['magnetic field', 'flux density', 'field strength', 'b'], hrr: ['heat release', 'heat release rate'],
  speed: ['speed', 'velocity', 'flow speed'], vorticity: ['vorticity', 'swirl'], ux: ['x velocity', 'u x'],
  uy: ['y velocity', 'u y'], uz: ['z velocity', 'u z'], ep: ['plastic strain', 'strain'], vm: ['stress', 'von mises'],
  von_mises: ['stress', 'von mises', 'von mises stress'], displacement: ['displacement', 'deformation', 'distortion'],
  temperature: ['temperature', 'heat'], damage: ['damage', 'cracks'], melted: ['melt', 'melted', 'melt pool'],
  current: ['current', 'current density'], stretch: ['stretch'], ez: ['electric field', 'e z'] };

function number(words) { // "12", "twelve", "twenty one"
  if (/^\d+$/.test(words)) return parseInt(words, 10);
  const parts = words.split(' ');
  let n = 0;
  for (const w of parts) {
    if (!(w in NUMBERS)) return null;
    n += NUMBERS[w];
  }
  return parts.length ? n : null;
}

export function normalise(text) {
  return String(text).toLowerCase().replace(/[.,!?;:"“”']/g, ' ').replace(/\s+/g, ' ').trim()
    .replace(/^(please |ok |okay |hey |now |and )+/, '').replace(/( please| now| for me)+$/, '');
}

export function parse(text, fields = []) {
  const s = normalise(text);
  if (!s || s.split(' ').length > 6) return null;
  const is = (...forms) => forms.includes(s);
  if (is('play', 'start', 'resume', 'play it', 'start playing', 'animate')) return { do: 'play' };
  // "stop" ends the AI's answer while it answers, else the playback (app.js decides)
  if (is('stop', 'stop it', 'cancel', 'never mind', 'nevermind', 'enough', 'that is enough', 'that s enough', 'thats enough', 'stop talking')) return { do: 'stop' };
  if (is('pause', 'freeze', 'hold', 'stop playing', 'pause it')) return { do: 'pause' };
  if (is('show the controls', 'show controls', 'manual', 'controls', 'open the controls')) return { do: 'manual', on: true };
  if (is('hide the controls', 'hide controls', 'close the controls', 'agentic')) return { do: 'manual', on: false };
  if (is('next', 'next frame', 'forward', 'step forward', 'next step')) return { do: 'next' };
  if (is('previous', 'previous frame', 'back', 'go back', 'step back', 'previous step')) return { do: 'prev' };
  if (is('first frame', 'start over', 'beginning', 'go to the start', 'the start', 'first step')) return { do: 'first' };
  if (is('last frame', 'end', 'go to the end', 'the end', 'last step', 'final frame')) return { do: 'last' };
  let m = s.match(/^(?:go to |show )?(?:frame|step) (.+)$/);
  if (m && number(m[1]) !== null) return { do: 'frame', frame: number(m[1]) };
  m = s.match(/^(?:cut|section|slice)(?: it)?(?: (?:along|across|through|on|in|at))?(?: the)? (x|y|z)(?: axis)?$/);
  if (m) return { do: 'cut', axis: m[1] };
  if (is('remove the cut', 'no cut', 'cut off', 'uncut', 'whole', 'show the whole thing', 'clear the cut', 'remove cut', 'no section'))
    return { do: 'cut', axis: 'off' };
  if (is('flip the cut', 'flip', 'other side', 'flip it')) return { do: 'cut', axis: 'flip' };
  if (is('true size', 'real size', 'actual size', 'life size', 'full size')) return { do: 'truesize', on: true };
  if (is('make it bigger', 'bigger', 'larger', 'zoom in', 'enlarge it', 'enlarge')) return { do: 'zoom', by: 1.5 };
  if (is('make it smaller', 'smaller', 'zoom out', 'shrink it', 'shrink')) return { do: 'zoom', by: 1 / 1.5 };
  if (is('reset', 'reset the view', 'reset view', 'put it back')) return { do: 'reset' };
  if (is('clear the pins', 'clear pins', 'remove the pins', 'remove pins')) return { do: 'clearpins' };
  if (is('clear the marks', 'clear marks', 'remove the marks', 'remove marks')) return { do: 'clearmarks' };
  if (is('be quiet', 'mute', 'voice off', 'speech off')) return { do: 'speech', on: false };
  if (is('unmute', 'voice on', 'speech on', 'talk to me')) return { do: 'speech', on: true };
  // a field, by its name or by what it is called: "show the temperature", "pressure", "switch to vorticity"
  m = s.match(/^(?:show(?: me)?|switch to|change to|colour by|color by|display)?(?: the)? ?(.+?)(?: field)?$/);
  if (m) {
    const want = m[1].trim();
    for (const f of fields) {
      const names = [f.id.toLowerCase(), (f.label || '').toLowerCase(), ...(SPOKEN[f.id] || [])];
      if (names.includes(want)) return { do: 'field', field: f.id };
    }
  }
  return null;
}
