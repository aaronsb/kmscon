// Act 3's opening, "this has got to beat ascii line art... how about ...
// AALIB?": a soft square-wave arpeggio thinking it over in E flat minor
// under a rimshot, a question after the first "...", a run up to a chime
// as AALIB is typed on bar 3's third beat, and a build for the rest of the
// bar and a half into the video's soundtrack. The typing takes 7.80 s,
// 4.5 bars of 138 BPM.
// @bpm 138
// @bars 4.5
// @tail .6
setcpm(138/4)

const tr909 = p => p.bank("tr8")

const think = n("[0 2 4 2]*2").chord("<Ebm B Abm ~ ~>").anchor("eb5").voicing()
  .s("square").decay(.15).sustain(0).lpf(1300).delay(.3).delaytime(.326).delayfeedback(.35)
  .gain(.17)

const ground = stack(
  note("<[eb2 ~ bb1 ~] [b1 ~ gb1 ~] [ab1 ~ eb2 ~] ~ ~>").s("triangle").decay(.5).sustain(.2).gain(.5),
  tr909(s("<[~ rim ~ rim]!3 ~ ~>")).gain(.4),
)

// a question after "...ascii line art..."
const hmm = note("<~ ~ [bb5 b5 ~ ~] ~ ~>")
  .s("square").penv(2).pattack(.05).decay(.3).sustain(.2).release(.2)
  .lpf(2400).delay(.3).delaytime(.326).delayfeedback(.35).gain(.16)

// AALIB: a run up into an E flat minor chime and a crash on bar 3's third beat
const idea = stack(
  n("<~ ~ ~ [~ [0 1 2 3 4 5 6 7] ~ ~] ~>").scale("Eb5:minor")
    .s("square").decay(.05).sustain(0).gain(.18),
  note("<~ ~ ~ [~ ~ [eb6,gb6,bb6,f6] ~] ~>").s("triangle").decay(1.4).sustain(0).room(.5).gain(.35),
  tr909(s("<~ ~ ~ [~ ~ [bd,cr] ~] ~>")).gain(.7),
)

// then a build on a B flat pedal into the video
const build = stack(
  tr909(s("<~ ~ ~ [~ ~ sd*4 sd*4] [sd*16 ~]>")).gain("<0 0 0 [0 0 .35 .5] [.65 .8]>"),
  tr909(s("<~ ~ ~ [~ ~ bd bd] [bd*8 ~]>")),
  note("<~ ~ ~ [~ ~ [bb1 bb2]*2] [[bb1 bb2]*4 ~]>")
    .s("sawtooth").lpf(800).lpq(6).decay(.12).sustain(.3).gain(.4),
)

stack(think, ground, hmm, idea, build)
