#!/usr/bin/env node
// Renders the showcase's music cues, Strudel files, to the .ogg files the
// show plays. Strudel's own offline renderer (renderPatternAudio) runs in
// headless Chromium, so a cue sounds as it does on strudel.cc; ffmpeg then
// brings it to LUFS loudness under a limiter, keeps a loop's second pass,
// and encodes Opus.
//
//   npm install && node render.mjs [CUE.js ...]     (default: every cue here)
//
// Each cue declares its length in header comments:
//   // @bpm 138    tempo; a cycle is one 4-beat bar
//   // @bars 10    the bars to render
//   // @tail 1.7   then this many seconds for reverb and delay to ring out
//   // @loop       render the bars twice and keep the second pass, which
//                  starts with its own tails, so the file loops without a seam
//
// CHROMIUM names the browser (default /usr/bin/chromium); LUFS the loudness
// (default -8, a little under the video's soundtrack at about -6.7).
import { createServer } from 'node:http';
import { readFileSync, readdirSync, createReadStream, statSync, mkdtempSync, rmSync } from 'node:fs';
import { extname, join, dirname, basename, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';

const here = dirname(fileURLToPath(import.meta.url));
const lufs = Number(process.env.LUFS ?? -8);
const cues = process.argv.slice(2).length
  ? process.argv.slice(2).map((f) => resolve(f))
  : readdirSync(here).filter((f) => f.endsWith('.js')).map((f) => join(here, f));

function header(code, name, fallback) {
  const m = code.match(new RegExp(`^//\\s*@${name}\\b[ \\t]*([\\d.]*)`, 'm'));
  return m ? Number(m[1] || 1) : fallback;
}

function ffmpeg(args) {
  const r = spawnSync('ffmpeg', ['-hide_banner', '-nostats', ...args], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`ffmpeg ${args.join(' ')}\n${r.stderr}`);
  return r.stderr;
}

// The drums: the 909 voices of a Roland TR-8, recorded one hit at a time by
// MckAudio and dedicated to the public domain (CC0 1.0), from
// MckAudio/MckSamplePacks at a pinned commit. A cue plays them as the bank
// "tr8" (bd, sd, hh, oh, cp, cr, rim, lt, mt, ht). Each recording runs
// 2.2 s, most of it its noise floor, so end cuts it 30 ms after it falls
// to that floor; level sets its loudness to that of the 909 sample it
// replaced, through velocity, so a cue's .gain() still works as it did.
const DRUM_BASE = 'https://raw.githubusercontent.com/MckAudio/MckSamplePacks/5db40e8fe26785c256845a5bb38921b2654a887f/TR8/';
const DRUMS = {
  bd: { file: 'BD/004_909_Bass_Drum_1.wav', level: 1.33, end: 0.286 },
  sd: { file: 'SD/005_909_Snare_Drum_1.wav', level: 1.19, end: 0.262 },
  hh: { file: 'HATS/008_909_Closed_HiHat.wav', level: 0.75, end: 0.099 },
  oh: { file: 'HATS/007_909_Open_HiHat.wav', level: 1.14, end: 0.305 },
  cp: { file: 'PERC/019_909_Hand_Clap.wav', level: 1.02, end: 0.268 },
  cr: { file: 'HATS/020_909_Crash_Cymbal.wav', level: 1.1, end: 0.422 },
  rim: { file: 'PERC/009_909_Rim_Shot.wav', level: 1.23, end: 0.086 },
  lt: { file: 'TOMS/010_909_Low_Tom.wav', level: 1.19, end: 0.329 },
  mt: { file: 'TOMS/011_909_Mid_Tom.wav', level: 1.16, end: 0.217 },
  ht: { file: 'TOMS/012_909_High_Tom.wav', level: 1.16, end: 0.297 },
};
const drumMap = Object.fromEntries(Object.entries(DRUMS).map(([name, d]) => [`tr8_${name}`, d.file]));

// The page: Strudel's browser bundle, served from node_modules.
const page = `<!doctype html><script type="module">
import * as S from '/node_modules/@strudel/web/dist/index.mjs';
const drums = ${JSON.stringify(DRUMS)};
window.renderCue = async (code, cps, end) => {
  await S.initStrudel({
    prebake: () => S.samples(${JSON.stringify(drumMap)}, '${DRUM_BASE}'),
  });
  const pattern = (await S.evaluate(code, false)).fmap((v) => {
    const d = v?.bank === 'tr8' && drums[v.s];
    return d ? { ...v, velocity: (v.velocity ?? 1) * d.level, end: d.end } : v;
  });
  await S.renderPatternAudio(pattern, cps, 0, end, 48000, 256, false, 'cue');
};
window.ready = true;
</script>`;
const server = createServer((req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  if (path === '/') return res.writeHead(200, { 'content-type': 'text/html' }).end(page);
  const file = join(here, path);
  if (!file.startsWith(join(here, 'node_modules'))) return res.writeHead(404).end();
  try {
    statSync(file);
  } catch {
    return res.writeHead(404).end();
  }
  res.writeHead(200, { 'content-type': extname(file) === '.mjs' || extname(file) === '.js' ? 'text/javascript' : 'application/octet-stream' });
  createReadStream(file).pipe(res);
}).listen(0, '127.0.0.1');
await new Promise((r) => server.once('listening', r));

let browser, work;
let failed = 0;
try {
  work = mkdtempSync(join(tmpdir(), 'showcase-music-'));
  browser = await chromium.launch({
    executablePath: process.env.CHROMIUM ?? '/usr/bin/chromium',
    args: ['--autoplay-policy=no-user-gesture-required'],
  });
  for (const cue of cues) {
    const code = readFileSync(cue, 'utf8');
    const bpm = header(code, 'bpm', NaN);
    const bars = header(code, 'bars', NaN);
    const tail = header(code, 'tail', 0);
    const loop = header(code, 'loop', 0) > 0;
    if (!(bpm > 0 && bars > 0)) {
      console.error(`${cue}: needs "// @bpm N" and "// @bars N"`);
      failed++;
      continue;
    }
    const cps = bpm / 4 / 60;
    const end = loop ? 2 * bars : bars + tail * cps;
    const context = await browser.newContext({ acceptDownloads: true });
    const tab = await context.newPage();
    const errors = [];
    tab.on('pageerror', (e) => errors.push(e.message));
    tab.on('console', (m) => m.type() === 'error' && !/Failed to load resource/.test(m.text()) && errors.push(m.text()));
    await tab.goto(`http://127.0.0.1:${server.address().port}/`);
    await tab.waitForFunction(() => window.ready);
    const download = tab.waitForEvent('download', { timeout: 600000 });
    try {
      await tab.evaluate(([c, cps, end]) => window.renderCue(c, cps, end), [code, cps, end]);
    } catch (e) {
      download.catch(() => {});
      await context.close();
      console.error(`${basename(cue)}: ${e.message.split('\n')[0]}`);
      failed++;
      continue;
    }
    const raw = join(work, `${basename(cue, '.js')}.wav`);
    await (await download).saveAs(raw);
    await context.close();

    // Loudness of the part kept, then gain, a limiter that holds its
    // timing (a loop must not shift) 2 dB down, as Opus overshoots by
    // about that, the loop's second pass, and Opus.
    const keep = loop ? ['-ss', String(bars / cps), '-t', String(bars / cps)] : [];
    const measured = ffmpeg(['-i', raw, ...keep, '-af', 'ebur128', '-f', 'null', '-']);
    // the summary's integrated loudness is the last "I:" ebur128 prints
    const integrated = Number([...measured.matchAll(/I:\s+(-?[\d.]+) LUFS/g)].at(-1)[1]);
    const gain = (lufs - integrated).toFixed(2);
    const out = cue.replace(/\.js$/, '.ogg');
    ffmpeg(['-y', '-i', raw, '-af', `volume=${gain}dB,alimiter=limit=0.79:level=false:latency=1`, ...keep,
      '-c:a', 'libopus', '-b:a', '160k', '-map_metadata', '-1', out]);
    const seconds = end / cps - (loop ? bars / cps : 0);
    console.log(`${basename(out)}: ${seconds.toFixed(2)} s, ${bpm} BPM, ${bars} bars${loop ? ', loops' : ''}; ${integrated} LUFS, ${gain} dB to ${lufs}`);
    for (const e of errors) console.error(`  ${basename(cue)}: ${e}`);
    failed += errors.length ? 1 : 0;
  }
} finally {
  await browser?.close();
  server.close();
  if (work) rmSync(work, { recursive: true, force: true });
}
process.exit(failed ? 1 : 0);
