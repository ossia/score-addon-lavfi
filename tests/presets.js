// Every shipped preset, in a real score.
//
//   ossia-score --script tests/presets.js
//   LAVFI_PRESET_DIR=/path/to/Presets/FFmpeg\ filter ossia-score --script tests/presets.js
//
// tests/GraphTest.cpp checks the same files against libavfilter alone. This one
// checks what the user actually does with them: the preset is loaded into a
// real FFmpeg filter process in a real document, which is where the graph text
// is parsed, the ports are built from its open pads and its filters' options,
// and (in the second phase) where the executor and the renderer run.
//
// Exit code 0 = every preset created its process and its ports; 1 = at least
// one did not, with the reason on stderr.

function presetDir()
{
  const env = Util.environmentVariable("LAVFI_PRESET_DIR");
  if(env && Util.isDir(env))
    return env;
  // Run from the addon: tests/presets.js -> ../Presets/FFmpeg filter
  const here = Util.environmentVariable("PWD");
  const candidates = [
    here + "/Presets/FFmpeg filter",
    here + "/../Presets/FFmpeg filter",
    here + "/src/addons/score-addon-lavfi/Presets/FFmpeg filter"
  ];
  for(const c of candidates)
    if(Util.isDir(c))
      return c;
  return "";
}

const UUID = "3f0a1b6e-6a6b-4d0c-9d8f-2b3c1e7a9f10";
const dir = presetDir();
if(!dir)
{
  console.error("presets: no preset directory found. Set LAVFI_PRESET_DIR.");
  Qt.exit(1);
}

// listFiles returns absolute paths and does not recurse; the presets are
// filed by kind (Video effects, Audio generators, ...).
let files = Util.listFiles(dir, "*.scp");
for(const sub of Util.listDirectories(dir))
  files = files.concat(Util.listFiles(sub, "*.scp"));
files.sort();
if(files.length === 0)
{
  console.error("presets: no .scp files in " + dir);
  Qt.exit(1);
}

// Filters that only exist in an FFmpeg built for that hardware. A preset using
// one is not broken when the FFmpeg score was linked against does not have it:
// the SDK build does, a distro build may not (v360_vulkan for instance only
// appeared in FFmpeg 7). Anything else that fails to configure is a real
// failure.
const OPTIONAL = ["_vulkan", "_cuda", "libplacebo", "_vt", "coreimage", "_qsv", "_amf", "_d3d11"];
function optionalFilters(text)
{
  return OPTIONAL.filter(function(f) { return text.indexOf(f) >= 0; });
}

const root = Score.rootInterval();
const failures = [];
const skipped = [];
const made = [];
let controls = 0;

for(const path of files)
{
  const file = path.substring(path.lastIndexOf("/") + 1);
  let preset;
  try
  {
    preset = JSON.parse(Score.readFile(path));
  }
  catch(e)
  {
    failures.push(file + ": not valid JSON (" + e + ")");
    continue;
  }

  if(!preset.Key || preset.Key.Uuid !== UUID)
  {
    failures.push(file + ": Key.Uuid is not the FFmpeg filter process");
    continue;
  }
  if(!preset.Name)
    failures.push(file + ": no Name");
  const effect = preset.Key.Effect;
  if(!effect)
  {
    failures.push(file + ": no Key.Effect (the filtergraph)");
    continue;
  }

  // What the library does when the preset is dropped on an interval.
  const proc = Score.createProcess(root, UUID, effect);
  if(!proc)
  {
    failures.push(file + ": the process could not be created");
    continue;
  }

  // The ports ARE the graph: no outlet means the text was not understood.
  // Score.inlets()/outlets() return counts, not lists.
  const ins = Score.inlets(proc);
  const outs = Score.outlets(proc);
  if(outs < 1)
  {
    const optional = optionalFilters(effect);
    if(optional.length > 0)
    {
      skipped.push(file);
      console.info("  skip " + file + ": needs " + optional.join(", ")
                   + ", which this FFmpeg does not have");
      Score.remove(proc);
      continue;
    }
    failures.push(file + ": no outlet -- the graph did not configure");
  }
  else
    console.info("  ok   " + file + ": " + ins + " inlet(s), " + outs + " outlet(s)");
  controls += ins;
  made.push(proc);
}

// A second pass over the same processes: a preset must survive being saved and
// read back, which is what happens to every score that uses one. loadPreset
// goes through Process::Preset::fromJson -- the same call the library makes on
// every .scp it finds -- and then through the process's own loadPreset, which
// for a script process restores the control values (the graph itself travels
// in Key.Effect and is applied when the process is created from the preset,
// exactly as in the Faust and JS processes).
for(const proc of made)
{
  const json = Score.savePreset(proc);
  if(!json || json.indexOf(UUID) < 0)
  {
    failures.push("savePreset produced nothing usable for one of the processes");
    continue;
  }
  Score.loadPreset(proc, json);
  if(Score.savePreset(proc) !== json)
    failures.push("a preset did not survive save -> load -> save");
}

console.info(
    "presets: " + (files.length - failures.length - skipped.length) + "/"
    + files.length + " created, " + skipped.length + " skipped, " + controls
    + " ports built");
for(const f of failures)
  console.error("FAIL " + f);

// -- Phase 2: run them ------------------------------------------------------
// Creating the process only exercises the model: the graph is described and
// the ports are built. Playing exercises the executor -- one ossia node per
// process, the audio graphs ticking, the gfx graphs building their renderer --
// which is where a preset that describes cleanly can still take the whole
// engine down. Every process stays on the root interval for this.
function finish(code)
{
  Score.stop();
  console.info(code === 0 ? "presets: play ok" : "presets: play FAILED");
  Qt.exit(code);
}

let timer = null;
try
{
  timer = Qt.createQmlObject(
      "import QtQuick; Timer { interval: 2000; running: false; repeat: false }",
      Score.document(), "presetPlayTimer");
}
catch(e)
{
  timer = null;
}

if(failures.length > 0)
{
  Qt.exit(1);
}
else if(!timer)
{
  // No QtQuick in this build: the model half still ran.
  console.info("presets: no timer available, skipping the play phase");
  Qt.exit(0);
}
else
{
  timer.triggered.connect(function() { finish(0); });
  Score.play();
  timer.running = true;
}
