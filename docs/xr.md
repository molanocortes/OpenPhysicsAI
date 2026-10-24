# The headset link: results in a Meta Quest, questions by voice

Up: [AGENTS.md](../AGENTS.md)

The owner, 2026-10-06: "once I run a simulation I can see it as a 3D object on my VR headset; bring FEA and voice
control through AI prompts into an AR environment." The headset is a Meta Quest 3S.

What it does: whatever the OpenPhysicsAI app on the Mac is showing stands in front of you in passthrough AR (or in a
studio in VR), at a size you can hold or at true size, in the same colours as on the Mac:

- a finite-element result (a printed part's temperature, stress, distortion) with its stored times;
- a lab simulation with all of its frames: vortices shed by a flag, a fire's plume, the pressure pulse in a concert
  hall, a dam break's water surface, a motor's field, a laser melting steel, an impact, a cloth on a body;
- a planar lab result (a 2D flow, the radar slice) as an upright picture of its plane at true size, its values still
  read by pointing, and a picture result (the black hole) 40 cm wide.

A card beside it shows the field, its unit and legend, and the value under your pointer: point at the result with a
controller or a hand and read the stored number there, its part and its place in the result's own coordinates (a
part's build frame in mm, a lab result's axes in m); tap to pin it. The card plays and scrubs the frames, steps the
stored times, switches the field and cuts the result across x, y or z; the app on the Mac does the same at once, and
what you change on the Mac reaches the headset. Hold a grip and speak (or press the card's microphone): the question
goes to Ask AI on the Mac (your own Claude Code or Codex) together with what you are looking at and pointing at, its
answer appears on the card and is spoken in the Mac's voice, and its permission prompts come with **Allow** and
**Deny**. The AI can see what you see: a picture of your view, taken as you ask (or whenever it asks for one), with
the result, the card, your pointers and pins, and never the room. It can act on it: it changes the view (a frame, a
field, a cut) and places labelled marks on the result ("the peak is here"), and it can read the points you pinned. Ask it to run a simulation and the result
appears in front of you when it is done. A plain command ("next frame", "show the temperature", "cut along z", "true
size") is done at once, without waiting for the AI.

## Start it

On the Mac, once:

```bash
make xr                                     # the app, the frame exporter (build/labxr), the voice and the listener
python3 ~/.localhost-router/register.py add openphysics-xr --proxy 127.0.0.1:8791
python3 tools/xr/xr_bridge.py --check-voice # macOS asks once whether "OpenPhysicsAI Listen" may recognise speech: OK
```

For the provider's voice (GPT-Live, below), give the bridge an OpenAI key once; it is read from the Keychain (or from
`OPENAI_API_KEY` in the bridge's environment) and never leaves the Mac:

```bash
security add-generic-password -a "$USER" -s "OpenPhysicsAI OpenAI" -w
```

Without it, the Mac's own voice is used. Speech is recognised on the Mac by **OpenPhysicsAI Listen** (`build/OpenPhysicsAIListen.app`, from
`tools/xr/listen.swift`), a small background app the bridge starts once and which ends with it. It has its own name so
that macOS asks once whether it may recognise speech, whoever started the bridge: a program started from Terminal is
refused otherwise, because Terminal declares no use of speech. It keeps the recogniser loaded: after its start (about a
second), a sentence comes back as words as fast as the recogniser makes them (the folder it is handed through adds
0.04 s). Answers are spoken by `build/openphysics-voice` (`tools/xr/voice.swift`); it picks the best voice installed for the
language, so a Premium or Enhanced voice from System Settings > Accessibility > Spoken Content makes the AI sound far
more natural.

Each time:

```bash
python3 tools/xr/xr_bridge.py               # or: make xr-bridge (--voice NAME picks the voice of the answers)
~/Library/Android/sdk/platform-tools/adb reverse tcp:8080 tcp:8080   # the Quest on USB, developer mode on
./openphysics                                    # open a result: a gallery card, your own part or a lab simulation
```

In the Quest's browser open `http://openphysics-xr.localhost:8080`, press **Enter AR** (or **Enter VR** for a large scene you want to stand in) and allow the
microphone when the browser asks; from then on, just speak. `*.localhost` is a secure origin, so WebXR and the microphone work without certificates. Over Wi-Fi instead, serve
the page over https with a certificate the Quest trusts.

## In the headset

You talk; nothing has to be pressed. The headset has the app's minimal look (the owner, 2026-10-07: "minimize the UI,
make the app and the VR have the same high quality minimalistic UI"; the window's design is docs/app/DESIGN.md "Two
buttons and one bar"): beside the result stands a small dock with the legend, the latest words, and one round button.
The headset listens all the time (`tools/xr/web/vad.js` takes a sentence when you pause for 0.7 s); your words appear,
then the AI's answer as it is written and spoken, and the line fades 8 s after the answer ends. The round button is a
black microphone; it turns orange, with a ring that breathes with your voice, while it listens (press it to stop
listening); while the AI works it is a stop square. Talk over the AI, or say "stop", and it stops. "Show the controls"
(or **Manual** beside the button) brings the controls card back. When the AI has started a simulation, the line says
how far along it is ("Computing fire 200kw: 45 %", from the last N/M its solver printed) and "Ready" when the app opens
the result in front of you.

| You do | It does |
|---|---|
| Speak | a plain command ("next frame", "show the temperature", "cut along z", "true size", "stop") is done at once; anything else goes to the AI with what you look at, point at and see |
| Point at the result | reads the value there, its part and its coordinates (on the card under Manual) |
| Trigger, or pinch, and hold on the result | carry it and turn it with your hand |
| A short tap on the result | pins the value (a tap on a pin removes it); the AI is told the pins |
| Both triggers on the result | bigger or smaller with the distance between your hands, turned with them |
| Thumbstick | up or down: bigger or smaller; sideways: turn |
| A or X | place it on a real surface: a ring follows your ray on the table or floor, the trigger sets it down |
| B or Y | true size (a 24 mm part is 24 mm, a 3 m fire plume is 3 m) and back; at true size a result taller than a table or wider than a desk stands on the real floor, so you can walk round it or into it |
| Hold the grip and speak, release | the same as speaking, for a noisy room (the headset does not listen while the grip is held) |
| The AI's marks | orange points with a label, placed by the AI on the result; **Clear marks** on the card takes them away |
| Walk round the result | the dock and the card turn to face you once you have gone a good way round |
| The round button | listening on or off; stop while the AI works |
| The card (Manual) | play, pause, step and scrub the frames; choose the field; cut across x, y, z and slide the cut; true size; place; reset; the voice of the answers on or off; Allow or Deny |

On a computer the same page looks as the app's window does: the name at the top left, Agentic and Manual at the top
right, the legend at the bottom left, and one bar with a text field and the round button, the AI's latest words above
it. Drag to turn, the wheel zooms, pointing reads values, a click pins one, space plays, the arrows step. The round button
there listens through the computer's microphone. It is the quickest way to check the link before putting the headset on.

## How it works

| Piece | File | Job |
|---|---|---|
| App | [src/xr.c](../src/xr.c) | while the headset folder exists (`~/.openphysicsai/run/xr`, or `$OPENPHYSICS_XR_DIR`): writes `scene.glb` for a finite-element result (the displayed surface in metres, Y up, resting on its lowest point; each vertex its value, `_VALUE`, and its place in the embedded colour map; extras with the field, range, stored time and the matrix back to the part's build coordinates in mm), at most once a second when it changes; writes `state.json` (what is shown; for a lab result its file, frame, playback and the view the window draws it with, `labapp_xr_view`; the AI's lines, its permission prompt and its last answer); takes `ask-*.txt` to Ask AI, `answer.txt` to the waiting prompt, and `cmd-*.txt` view commands, which it runs only when `xrlink_command_allowed` ([src/core/xrlink.c](../src/core/xrlink.c), shared with openphysics-mcp) accepts them. A headless run uses the folder only with `OPENPHYSICS_XR=1` |
| Lab frames | [src/lab/labxr.c](../src/lab/labxr.c), `build/labxr` ([tools/labxr.c](../tools/labxr.c)) | one frame of a lab result as a GLB, built from `labscene_build`, the same code the window draws with: surfaces in the legend's palette with `_VALUE` per vertex and `_PART`, material looks (copper, steel, fabric...), particles with their radius, lines, and the computed volume as half floats of `(value - lo) / (hi - lo)` with its mask (`labscene_volume_mask`, shared with `src/labgpu.c`: iso surfaces, water coverage, a body's occupancy) |
| GLB writer | [src/core/glb.c](../src/core/glb.c) | glTF 2.0 binaries from a JSON DOM; half floats rounded to nearest even |
| Bridge | [tools/xr/xr_bridge.py](../tools/xr/xr_bridge.py) | serves the page, the state, `scene.glb`, `/lab/info` and `/lab/frame.glb` (frames drawn by `build/labxr` on demand with the view of the state, at most two at once, kept in memory up to 160 MB, never on disk; a stale view key gets 409), `/speech.wav` (each new answer said by `say` as it appears); passes questions with their context, answers and view commands (the list in [tools/xr/commands.json](../tools/xr/commands.json)); carries the AI's request for a picture to the page (`look_want` in the state) and keeps the page's pictures (`/look`: `look-now.jpg`, `look-question.jpg`, each with its facts) |
| The AI's headset tools | [src/bin/openphysics_mcp.c](../src/bin/openphysics_mcp.c), [src/core/xrlink.c](../src/core/xrlink.c) | `headset_status` (what is shown, whether the page is connected, the person's pins, the marks), `headset_look` (a picture of what the person sees, as an MCP image: a new one, or the one taken at their last question), `headset_view` (view commands, checked by the same rule as the app's and withdrawn if the app does not take them in 4 s) and `headset_mark` (labelled points in the result's own coordinates); offered with `--lab`, so the app's Ask AI has them in Claude Code and Codex alike ([mcp-clients.md](mcp-clients.md)) |
| Speech | [tools/xr/listen.swift](../tools/xr/listen.swift) | the Mac's own speech recognition (Apple Speech, on the device when the language allows); the Quest's browser has none of its own |
| Page | [tools/xr/web/](../tools/xr/web/) | WebXR `immersive-ar` and `immersive-vr` with three.js r165 ([THIRD_PARTY.md](THIRD_PARTY.md)): `app.js` (scene, controllers, hands, placement, the computer's mouse), `scene.js` (loading and playing frames, particles as lit spheres), `volume.js` (the app's volume shader, `src/labvolume_shader.h`, in WebGL2, and the same ray read on the CPU for a probe), `card.js`, `probe.js`, `link.js`, `voice.js`, `commands.js` (the plain spoken and typed commands) |

The bridge is glue: nothing physical is computed outside the app and `build/labxr`, which reads the stored result.

**Frames.** The page asks for the frame being shown first, then the ones ahead of it, two at a time, and keeps up to
320 MB of them; the result's box from `/lab/info` places every frame alike. While the app plays, the headset plays its
own copy at the app's pace; paused, it shows the app's frame. What the person does in the headset (play, pause, a
frame, the slider) happens there at once and is sent to the app; a pause in the headset stops the app on the frame the
headset shows. A new field, range or cut gives the view a new key, and every frame is drawn again with it.

**What the AI is told.** A question asked in the headset carries one sentence after it, in brackets as the window's own
context is: what the headset shows (field, unit, frame and time, true size), what the person points at (part, place,
value) if they pointed in the last four seconds, and the pinned points. Example: "the headset shows T [K], t = 0 ms
(1 of 3); the person points at part 'block', x 0.498, y 0.536, z 1 m, where T is 6.79 K; pinned points: 1) ...". The
conversation on the card and in the window shows only the words that were said.

**What the AI can do there.** The question from the headset ends by naming the headset tools. With `headset_status`
the AI reads what is shown and the pins (part, place, value, frame), with `headset_view` it changes the view for the
person, and with `headset_mark` it puts labelled points where it means: "the temperature peaks here", with a mark on
the spot. Marks belong to the result they were made for and are kept in the headset folder (`marks.json`) until they
are replaced or cleared.

**What the AI sees.** `headset_look` gives the AI a picture (JPEG, 960 x 720) of what the person sees: the result, the
card with its field and legend, the pointers, pins and marks. The page draws the scene once more into a picture of its
own, from between the person's eyes with a 70 degree view (on a computer, the window's view), through the same shaders
as the headset's own frames: tone-mapped and sRGB-encoded as on screen (three r165 does that only for its XR target,
so the picture's target carries that flag, with plain 8-bit storage). The room is never in it: the headset gives the
page no camera picture, and where passthrough shows the room the picture has the studio's colour. With `moment` "now"
the AI asks the page for a new one (a request file the bridge puts in the state; the page answers after its next
frame, within half a second; the AI waits up to 6 s and withdraws the request if no picture comes, a sleeping headset
for example). With "question" it gets the picture the page took as the person asked: when a typed question is sent,
or when the grip is released after speaking, before the words are recognised, so it shows what they were looking at
while they asked. A question waits at most 1.5 s for its picture.

**How the headset runs.** Every 5 s, at the start and end of a session and soon after any error, the page reports to
the bridge how it runs: in AR, VR or on a screen; frames a second over its last frames, the lowest 5-second rate and
the longest frame of the session; the session's features (hit test, hand tracking), blend mode, display rate and the
rates the headset offers, the pixels per eye, the foveation and scale it was given; the result drawn (triangles,
particles, volume, megabytes) and the volume's ray-march steps; its last ten errors; the browser and the GPU. The bridge
keeps the latest in `health.json`, which `headset_status` gives the AI while it is fresh, and writes the last report of
every headset session as a line of `~/.openphysicsai/run/xr-sessions.jsonl`, beside the headset folder, which stays when the
bridge stops: a trial in the Quest leaves its numbers behind for whoever looks after the link.

**Talking with the AI.** The owner, 2026-10-07: "talk in the AR and VR with Claude or Codex and give verbal instructions
that perform changes in the simulation and I can see them ... the model should be extremely fast". What makes it fast,
each part measured:

- One conversation, one process. With the Claude Code command the app detects, the window keeps one Claude Code process
  for the whole conversation (`claude -p --input-format stream-json --include-partial-messages`) instead of starting one
  per question: each question is a message on its input, the conversation is remembered, and a process that ends (a
  crash, a STOP it would not hear) is resumed with `--resume` and its session id. Measured 2026-10-07 on the same
  question: 9.4 s to the first word and 15.4 s to the end with a new process per question on the default model; 2.0 s
  and 3.3 s in one session on Haiku 4.5.
- A small prompt. A conversation runs `openphysics-mcp --lean`: 16 tools in about 5 400 tokens (the lab, the headset, the
  printed-part geometry of [cad.md](cad.md), and
  `engine_ops`, `engine_op` and `engine_call`, through which every engine operation is still reached), instead of 83
  tools in about 81 000; no built-in Claude Code tools (`--tools ""`) and no tool search. The first answer of a cold
  session that had to call a tool took 9 to 10 s with the full set; a question needing no tool now takes 4.1 s from a
  cold start, process included. The conversation is started when the headset page connects, so the first question
  does not wait for it.
- A fast model for speech. Questions from the headset run with the voice model: Haiku 4.5 unless `agent_voice_model`
  in `~/.openphysicsai/app/ui-state` (or `$OPENPHYSICS_VOICE_MODEL`) names another, or `same` to keep the model picked for typed
  questions; the session switches model per question (`set_model`), so typed questions in the window keep theirs.
- Words as they come, speech by the sentence. The app writes the answer's words into the state as they stream (every
  80 ms while the AI answers); the bridge turns each finished sentence into sound with `build/openphysics-voice`, one macOS
  synthesizer kept warm (`tools/xr/voice.swift`: 45 to 120 ms a sentence, against 1.0 to 1.5 s for `say`), and the
  page plays the sentences one after another without a gap. Markdown, code and paths are not read out, and a unit after
  a number is said in words. With the stand-in agent a sentence is in the state 0.02 to 0.05 s after the agent writes
  it and speech 0.11 to 0.24 s after.
- A change in one step. When the headset shows a lab result, the question names it and lists its scenario's values by
  path (from the scenario in the result's own header, shallow ones first, source texts left out, at most 1 200
  characters), and the AI changes it with `lab_change` ([mcp-clients.md](mcp-clients.md)): only the values it names,
  each checked to be in the scenario and of the same kind, saved as NAME_v2 and computed, the change noted in the title
  and in the source beside each changed value. Measured 2026-10-08 with Haiku 4.5 in a warm session, "Make the wind
  twice as strong." with the flag shown, one run of each at load averages of 14 to 23: before, three tool calls
  (`lab_scenarios`, `lab_scenario_get` with the solver's documentation, `lab_run` with the whole scenario written out
  again), the run started 14.4 s after the question and the turn took 16.7 s, $0.049 and 1 438 output tokens; with
  `lab_change`, one call, the run started after 4.2 and 4.6 s, the turn took 6.6 and 6.3 s, $0.027 and $0.018 and
  about 420 output tokens, and the answer was "Doubling the wind speed from six to twelve meters per second. It's
  computing." The AI waits a second on the run, so a value the solver refuses at once is said; a run that stops later
  (a refusal after the answer, a crash) is in the headset's line with labrun's own words ("flag 3d v2 stopped: ..."),
  and the provider's voice is told, instead of "Computing" just going away. "Ready" comes when the app shows the result.
- Watching it compute. A run the AI started is shown while it computes, in the window and the headset alike: the app
  opens its result once it holds two frames (labio's `lab_refresh` reads the frames written since, complete ones only)
  and adds each frame as it is written; a view on the newest frame stays on the newest; the colour range only widens
  while it grows, with a quarter of the span to spare, so the legend and the headset's frames drawn with it change a
  few times, not with every frame; the range over all frames is taken once when the run ends, and it then plays. The
  headset keys its frames by the result as the app opened it, not by the file's time, so the frames it has stay while
  new ones come. A run counts as computing while its process lives, is not a zombie (openphysics-mcp collects a finished
  labrun only when asked about it) and started before its pid file was written (a reused process number is not it);
  while the headset link runs, results the AI makes open whatever the window was showing.
- Stop and talk over it. "Stop", or a new question said while the AI answers, interrupts the answer (Claude Code's own
  interrupt: the turn ends in about 0.3 s and the conversation stays); what is not yet spoken of it is not played.
- What you see goes with what you say. The picture the page takes as you ask is sent inside the question (the model sees
  it without calling a tool), and the question says what you point at and what the headset shows.

A command other than the detected Claude Code one (Codex, or a command of your own) runs as before, one process per
question; its finished answer is still spoken sentence by sentence.

**The provider's voice.** The owner, 2026-10-08: "connect with the voice from the original intelligence provider ...
they have better voice than what we can build". With an OpenAI key on the Mac, the headset talks through **GPT-Live**
(`gpt-live-1`, the full-duplex voice behind ChatGPT Voice: it listens while it speaks, you can talk over it, and it
answers small things itself at once), and every request about the simulation is delegated to the agent the app
already runs. Anthropic offers no voice to developers (Claude's apps speak through ElevenLabs; Claude Code's `/voice`
is dictation only, over a private stream), so with Claude Code this is OpenAI's voice carrying Claude's answers, and
with Codex it is the provider's own. How it fits ([tools/xr/live.py](../tools/xr/live.py),
[tools/xr/web/live.js](../tools/xr/web/live.js)):

- The headset holds the audio: its microphone goes to OpenAI over WebRTC and the voice comes back on a media track.
  The Mac holds the key: the bridge trades the headset's SDP offer for OpenAI's answer (`POST /v1/live/sessions`) and
  attaches a sideband WebSocket to the same session (`/v1/live/sessions/{id}/attach`). No key reaches the headset.
- When the voice delegates (`session.delegation.created`), the person's words since the last delegation become one
  question to the app's conversation (with what the headset shows and the person points at); while the agent works
  longer than 3 s the voice is told quietly what it does (`session.thinking.append`: the step, or the simulation's
  progress), and the finished answer goes back to be said (`session.commentary.append`, matched to its question by
  its words). The voice is also told what the headset shows when it changes.
- Plain view commands are done by the headset from the live transcript at once (a phrase ends after 0.8 s of no
  words); the voice is told, and a delegation of six words or fewer waits up to 1.2 s for that, then is answered
  "Done" without the agent, so "next frame" never moves two frames.
- The round button mutes the microphone (nothing is sent) or, while the agent works, stops: the agent's turn is
  interrupted, the voice is told to stop and goes quiet at once, and the answer awaited is not spoken.
- It is billed while it is open ($0.05 a minute for the voice, plus the agent's own usage), so it opens on entering
  AR or VR (or with the round button on a computer) and closes on leaving, and after five quiet minutes.
- Without a key, or when OpenAI cannot be reached, the headset uses the Mac's own voice (the listening and speech
  above) and says why. The microphone's sound goes to OpenAI while the voice is open; the room's picture never does.

**View commands.** The headset and the AI can play, pause, step, choose a frame, a stored time, a field, a cut and the
deformation scale. Nothing else: no file, no run, no other terminal command. The bridge checks every command against
the same rule as the app and openphysics-mcp (`src/core/xrlink.c`), and both rules are tested on the same list. A plain
command said or typed in the headset is matched whole by a small grammar (`commands.js`); a sentence it does not match
is a question for the AI.

## Quality and cost

One frame is small: 0.26 MB for the fire's 40 x 40 x 80 volume, 0.64 MB for the flag (100 x 40 x 40 volume and two
bodies), 2.5 MB for the hall's 98 000 particles, 2.3 MB for the motor's flat-shaded hexahedra, 2.4 MB for the Taylor
impact; `build/labxr` draws one in well under a tenth of a second when it is given the view, as the bridge gives it. Faces of hexahedra carry no normals (they are shaded flat from the
screen, as in the app), which welds their corners and saves 40 %. The finite-element scene of the Thermal Corolla is
1.6 MB (51 166 triangles). The page draws at 1.5 times the headset's default resolution with no foveation for parts
and particles; with a volume it keeps the default resolution, a light foveation, and lowers the ray-marching steps
when frames come late (64 to 360 steps a ray).

## Verified, and not yet

- The provider's voice (2026-10-08), `talk_test.py` criteria 13 to 16 (written before their first run), against a
  stand-in for OpenAI's live sessions ([tools/xr/live_standin.py](../tools/xr/live_standin.py): no key, no network):
  without a key `/live/start` says so and the Mac's voice is named; with one, OpenAI receives the documented session
  (gpt-live-1, client delegation, voice marin) and the headset's offer, the key only in the Authorization header, and
  the sideband attaches; the person's words and a delegation reach the agent with the headset's context and its
  answer goes back to be said 0.08 s after it ended, with quiet progress while it works over 3 s, and the bridge's
  own voice silent; a view command the headset did is answered "Done" without the agent; both sides' words reach the
  page. By hand in the app's browser pane the same day, with a synthetic microphone: the page's real WebRTC offer
  (1 469 bytes) traded through the bridge, the browser accepting the answer and going live; a spoken question handed
  to the agent and its answer handed to the voice; the voice's words in the line as it spoke; "next frame" done by
  the headset (frame 0 to 1) and its delegation answered "Done" while the agent got only the real question; mute and
  unmute; stop during a long answer (the agent interrupted, the voice told to stop, the half-answer not spoken). Not
  yet: OpenAI itself (no key on this Mac), and the voice in the Quest.
- The headset's minimal interface and hands-free listening (2026-10-07), `make test-xr` part 5 and 7, criterion 10 of
  `xr_test.py`, written before its first automated run: the voice-activity detector on synthetic sound (one sentence
  from a 0.8 s voice 25 dB above the room, none from a 50 ms click or noise, none from the AI's own voice 15 dB above
  while it speaks, one from a person 25 dB above it, checked first by hand: a 1.22 s sentence, 1.06 s over the AI); the
  page opening as the window does and Manual showing the card (both passed in the automated run); the rest of the run
  was cut short when Chrome went away under it at a load average of 30, as on the earlier stalls. By hand in the app's
  browser pane with the stand-in agent, the same day: a question heard by the page answered by the stand-in, its words
  streaming into the line and both sentences made into speech about 0.1 s after each was complete; in AR with the
  stand-in WebXR runtime, the dock to the result's right at reading height, turned to the head, with the legend, the
  question and the round button (the picture the page took from the head shows it), the button turning to stop while a
  long answer streamed and stopping it at once ("Stopped."). Not yet: the microphone itself in the Quest (the pane
  blocks microphones) and the AI's voice coming back through the headset's speakers into its microphone.
- OpenPhysicsAI Listen (2026-10-07), `talk_test.py` criterion 11, in its stand-in mode (no permission asked): three
  WAVs into words in 1.14, 0.04 and 0.04 s from one app process, and the app ending with the bridge (by hand); in the
  automated run at a load average of 30 the first took 9.6 s (the launch) and that check failed; at 32 it passed
  (0.24, 0.02, 0.03 s). Real recognition needs the one OK in macOS's dialog (`--check-voice`), not given yet.
- What is computing (2026-10-07), `talk_test.py` criterion 12: a run the AI started, still going with its log at frame
  3 of 40, is in the state as 0.08 done within 2 s and gone when its process ends (passed). The test gives its app a
  home of its own, which found a first-run fault: the agent's channel could not be opened where `~/.openphysicsai` did not
  exist yet (fixed in `src/app.c`).
- 2026-10-08, at load averages of 22 to 30: the voice helper warmed itself with "." only, so the first real answer
  still paid for the rest of the voice (its first sentence spoken 3.55 s after it was written); it now warms with a
  real sentence (0.46 and 0.51 s in the next two runs). Criterion 8 was amended openly the same day: the helper's speed
  is judged against `say` timed in the same run (42 to 120 ms at load 5 to 15 and up to 984 ms at 27 to 45, with no
  change of code: a fixed limit judged the laptop's load). At those loads two checks still fail now and then on time
  alone, never on behaviour: OpenPhysicsAI Listen's first launch through macOS (3.2 and 6.9 s against 3 s) and one
  sentence of the voice helper over a quarter of `say`'s time (891 against 889 ms).
- The whole loop with a real model (2026-10-08, [tools/xr/loop_bench.py](../tools/xr/loop_bench.py), one run, no
  page and no speech recognition: the sentence posted to a bridge of the bench's own, everything else real). "Make the
  fire twice as strong." with a small fire shown: first words after 3.6 s, the run started (`lab_change`, burner 100 to
  200 kW) after 4.7 s, shown in the app while it computes after 5.4 s, the answer done after 8.0 s, then a new frame
  every 1 to 26 s up to frame 17 at 214 s; Haiku 4.5, $0.028. No frame came after that because the Mac itself went to
  sleep: macOS's power log has "Entering Sleep state due to 'Low Power Sleep' ... Using Batt (Charge:1%)" at 02:11:36,
  right after frame 17, and the wake when the charger was attached at 04:57:58; the bench's 480 s had passed by then.
  The same run with the same scenario and the real model, at 09:20 at a load of 15 to 23, went to its end: run started
  after 4.4 s, shown while it computes after 5.0 s, first words after 7.2 s, answer done after 8.4 s, all 25 frames in
  the app as each was written, the run done after 432 s; $0.028. A run of the doubled fire without the model, followed
  by a headless app, showed the same (25 frames, none missed, "growing" off at the end). The bench now prints, every
  2 s, the frames in the file, how old the app's state is and whether the app and the run live, and keeps its folders
  and the logs' ends when it does not see the run to its end.
- Watching it compute (2026-10-08), each criterion written before its first run: `tools/labtest.c` L4 (a file read
  while written: one frame at the open, two after one more, still two with half of the third, three once whole) passed
  at its first run; `tools/labapptest.c`'s growing run (opened at its second frame, paused on the newest, the count and
  the view following, the range widened and never narrowed, the same serial, the end with the range over all frames and
  playing, a person who opened another result not taken back) failed twice at its first run, on a second run written in
  the same second as the first and on its own files disturbing an older check; the follower now looks for computing
  runs by their pid files, and the case removes its files (amended openly); then 463 passed, three runs in a row.
  `talk_test.py` criteria 18 (a run ended but not reaped is no longer computing and is listed as stopped: this found
  that the "Computing" line of 2026-10-07 would have stayed after every spoken run, since openphysics-mcp leaves a finished
  labrun unreaped) and 19 (real frames of a short flow_volume run written one at a time: shown at two frames, growing
  with the same version) pass with the other seventeen at a load of 2 to 5; 19's view-key part was amended openly after
  its first run, the key now following the view (a wider frame widens the range, which is part of it). The window's
  checks pass after it: `tools/minimal_window.py` 17 of 17, `tools/uicheck.py` 173 of 173. The page in headless Chrome
  (xr_test part 5) did not run to its end on that day in eight tries (see the header of `tools/xr/pagetest.mjs`).
- A change in one step (2026-10-08), [tools/xr/change_test.py](../tools/xr/change_test.py) in `make test-xr`, nine
  criteria written before their first run: `lab_change` offered in both profiles; its base the scenario in the shown
  result's header before a version made here and the example; exactly the named values changed (a nested key, an
  element of a list of objects, a list of numbers, a text); ten refusals that leave nothing saved and nothing started;
  the source notes, the title and the names (`_v2`, `_v3`, a change of `_v3` is `_v4`); the run with a stand-in labrun
  and once with the real solver (flow_volume shortened to 0.05 s: computed in 0.2 s, its header holding the change);
  and the headset's question naming the result shown with its values by path, and neither without one. The first run
  found the general instruction naming `lab_change` when no lab result was shown; the code was changed, not the
  criterion. `talk_test.py` criterion 17 (written before its first run): runs that ended without labrun's record are in
  the state with why, a finished one is not; passed with the other sixteen at a load average of 15.
- The whole of `talk_test.py` at a load average of 28 to 32 (other sessions' simulations running): all checks pass
  except, in one run or another, the voice helper's speed (0.37 to 0.98 s a sentence against 0.2 s; 42 to 120 ms on a
  quieter machine); the test now waits for the bridge's voice to be loaded before its first question, as it is long
  before anyone speaks in the headset.
- The conversation (2026-10-07), [tools/xr/talk_test.py](../tools/xr/talk_test.py) in `make test-xr`, ten criteria
  written before their first run, with a stand-in for Claude Code's session protocol
  ([tools/xr/claude_standin.py](../tools/xr/claude_standin.py): no model, nothing spent): one process for every question,
  started with the session flags before the first question and sent nothing; the words growing in the state; the first
  sentence spoken before the second is written and within 0.8 s of the first (0.11 to 0.24 s seen); sentences in order
  and once; `/stop` ending an answer by an interrupt in 0.31 s with the same process answering next; a question said over
  an answer; the voice model; a killed session resumed; the picture sent as the question's first block; the voice
  helper under 0.2 s a sentence. Passing; once, under a load average of 15, the voice took 289 ms for one sentence and
  that check failed, and once the first sentence took 0.95 s while the voice was still loading at the bridge's start.
  With real Claude Code on Haiku 4.5 through a headless app and the bridge (not the page): a question needing no tool
  had its first words 4.1 s after it was asked, process start included. Not yet: the whole loop in the headset with
  real speech in and out; Codex (not installed on this Mac).

- `make test-xr` (2026-10-07, about a minute, passing): `build/labxrtest`, whose criteria are in its header: on a
  synthetic result with every kind of part, in six views, the GLB's triangles, particles, lines, values, palette
  coordinates, looks, volume samples (within half precision) and mask are exactly what `labscene_build` and
  `labscene_volume_mask` give the app; half-float rounding agrees with an independent reference and the hardware's
  `__fp16` on two million floats; every result in `~/OpenPhysicsAI-Projects/lab` converts (39 frames of 18 results, each
  under a tenth of a second; the 5 planar results are refused with their reason). Then
  [tools/xr/xr_test.py](../tools/xr/xr_test.py), whose criteria are in its header: the app's and the bridge's command
  rules agree with the list; frames served by the bridge are byte for byte `build/labxr`'s and pass `xr_check.py`;
  view commands reach the headless app; questions carry their context; answers are spoken; the Thermal Corolla's
  scene passes `xr_check.py` against its STL (28.00 x 18.00 x 28.00 mm); and the page, in headless Chrome with real
  mouse events and synthetic controller events, loads every frame, draws what the files hold, reads on the block
  exactly the stored field there (T is linear in position, so the reading is checked against a closed form), pins,
  plays, pauses, steps, scrubs, cuts and switches fields in the app, and resamples the voice (1 kHz passes at
  -0.00 dB, a 12 kHz tone is gone).
- What the AI sees (2026-10-07), `make test-xr` part 8, criteria written before the first run: `headset_look` fails
  in 6.3 s with no page answering and leaves no request behind; with a stand-in page it returns the page's JPEG byte
  for byte, and the picture taken at a question for moment "question"; `/look` refuses another type (415), a cut JPEG
  (400), a bad id (400) and another site's page (403) and writes nothing for them. The page's own checks (the colour
  at the block equal to the window's, the picture from the head in AR, the health report in a session) are written in
  parts 5 and 7 but have not yet run to their end: headless Chrome stalled on this laptop that evening (66 MB free,
  4 of 5 GB of swap in use). By hand in the app's browser pane, the real `headset_look` returned the page's 960 x 720
  picture of the synthetic result in 0.7 s, card and legend legible; to be run in full on a quiet machine.
- The AI's headset tools (2026-10-07), in `make test-xr` part 6 against a headless app: the three tools are offered,
  an allowed view command is taken by the app and a refused one never reaches it, a mark reaches the state the page
  reads, the status names the result. The page's later steps (the mark drawn, plain commands typed into the question
  box reaching the app, the grammar's sample sentences, the pins reported back) passed in separate runs that day; the
  whole page run then stalled inside headless Chrome, with no JavaScript running and the page's own measures flat,
  while the laptop's load average was 8 to 12 and its swap nearly full; it had passed twice in full earlier, before
  these steps were added. To be run again on a quiet machine.
- Planar results (2026-10-07): `build/labxrtest` (its criterion 8, amended that day when planes became pictures) checks
  the quad against the plane's extent and its value grid against a closed form (2 x + 3 y + 1, exact under bilinear
  sampling, within half precision), and draws the 5 planar results on this computer; in the page, the values read on
  the radar slice at frame 40 match `build/labprobe --line` at seven points (for example -0.60361 against -0.60361 V/m
  and 0.0541 against 0.0542 V/m), and the black hole shows with its 60 frames.
- By eye in a browser on the Mac, 2026-10-07, with the headless app: the flag's vortex cores (as in the app's gallery
  picture), the fire's plume, the hall's pressure pulse, the dam break's water surface, the motor's field, the laser
  tracks and the Thermal Corolla, with probing, field change, play, the cut and the time slider.
- `python3 tools/xr/xr_check.py scene.glb --stl part.stl` (2026-10-06, extended 2026-10-07 to lab frames): valid
  glTF 2.0, one value per vertex, true size against the STL.
- The headset's own code, 2026-10-07, with a stand-in WebXR runtime ([tools/xr/web/test/fakexr.js](../tools/xr/web/test/fakexr.js),
  loaded only with `?fakexr=1`; it emulates no Quest, it makes the code run): entering AR, 2 100 headset frames without
  an error, the placement 0.5 m in front of the head and 0.42 m below the eyes with the card to its left facing the head,
  a controller pointed at the block reading its stored value (19), the thumbstick, true size standing a wide result on
  the floor and back, placing on a found surface facing the head, two hands from 0.2 m to 0.8 m apart scaling it
  exactly four times, the card turning toward a head that walked round, and the session's end; written as `make
  test-xr` part 7.
- Not yet run: anything inside the Quest itself: its WebXR session, real controllers and hands, its hit testing on a
  real table, the frame rate with a volume, the answers spoken in the headset; and a spoken question end to end (macOS
  has not yet been allowed to use speech recognition here). Lab
  results that are neither 3D nor a plane cannot be drawn (none on this computer now); the card says so.
