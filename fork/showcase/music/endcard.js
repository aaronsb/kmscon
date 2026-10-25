// The end card: an epilogue under the turning torus that loops until a key.
// Sixteen bars of 138 BPM in E flat minor, the eight-bar progression twice:
// pad and arpeggio, half-time drums from bar 4, and from bar 8 a distorted
// saw lead that glides and bends, then back to the quiet start.
// @bpm 138
// @bars 16
// @loop
setcpm(138/4)

const prog = "<Ebm B Gb Db Abm B Bbsus4 Bb>"
const tr909 = p => p.bank("RolandTR909")

const pad = chord(prog).voicing()
  .s("sawtooth").attack(.6).release(1.5)
  .lpf(sine.range(600, 1500).slow(16))
  .vib(4).vibmod(.06).room(.8).size(.9).gain(.13)

const arp = n("[0 1 2 3 4 3 2 1]*2").chord(prog).anchor("eb5").voicing()
  .s("square").attack(.002).decay(.12).sustain(0)
  .lpf(sine.range(700, 3800).slow(8)).lpq(8)
  .delay(.3).delaytime(.326).delayfeedback(.35).room(.4)
  .pan(sine.slow(4).range(.3, .7))
  .gain(.2)

const bass = n("[0 ~ ~ 0 ~ ~ 7 ~]".add("<0 -2 2 -1 3 -2 4 4>"))
  .scale("Eb2:minor")
  .s("sawtooth").lpf(600).lpq(4).decay(.3).sustain(.4).gain(.38)
  .mask("<0!2 1!14>")

const drums = tr909(stack(
  s("[bd ~ ~ ~ ~ ~ ~ ~ ~ ~ bd ~ ~ ~ ~ ~]"),
  s("[~ sd]"),
  s("[~ hh]*4").gain(.25),
  s("<cr ~!7>").gain(.45),
)).mask("<0!4 1!12>")

// the lead: penv is the semitones each note glides from
const lead = note(`<
    ~ ~ ~ ~ ~ ~ ~ ~
    [bb4@3 eb5@3 gb5 f5]
    [eb5@4 db5@2 b4@2]
    [db5@3 bb4 gb4@2 bb4 db5]
    [f5@6 eb5 db5]
    [eb5@3 gb5@3 ab5 gb5]
    [gb5@4 eb5@2 db5 eb5]
    [f5@4 eb5@4]
    [d5@8]
  >`)
  .penv(`<
    0!8
    [0@3 5@3 0 0]
    [-2@4 0@2 0@2]
    [2@3 0 0@2 0 0]
    [4@6 0 0]
    [2@3 3@3 0 0]
    [0@4 0@2 0 0]
    [2@4 0@4]
    [4@8]
  >`)
  .pattack("<.07!15 .5>")
  .s("sawtooth").shape(.5).lpf(2800).lpq(3)
  .attack(.01).decay(.5).sustain(.6).release(.4)
  .vib(5).vibmod(.2)
  .room(.6).size(.8).delay(.35).delaytime(.326).delayfeedback(.4)
  .gain(.28)

stack(pad, arp, bass, drums, lead)
