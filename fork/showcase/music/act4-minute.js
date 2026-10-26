// Act 4, "wait a minute... don't we have ... LIBSIXEL?": the music stops
// like a tape, a clock ticks under a sneaking bass, a question after each
// "...", then LIBSIXEL lands on bar 3 on the dominant and builds for half a
// bar more into act 5, where the video's soundtrack comes back. The typing
// takes 7.82 s, 4.5 bars of 138 BPM.
// @bpm 138
// @bars 4.5
// @tail .3
setcpm(138/4)

const tr909 = p => p.bank("tr8")

// the tape stops: a chord whose pitch falls two octaves
const tape = note("<[eb3,gb3,bb3,eb4] ~!4>")
  .s("sawtooth").lpf(2500).penv(-24).panchor(0).pattack(.7)
  .decay(.8).sustain(0).room(.3).gain(.28)

const clock = tr909(s("<[~ ~ rim rim] [rim rim rim rim]!2 ~ ~>"))
  .speed("[1.25 .9]*2").gain("[.5 .3]*2")

const sneak = note("<[~ ~ eb2 gb2] [a2 ~ bb2 ~ b2 ~ bb2 ~] [a2 ~ gb2 ~ f2 ~ e2 ~] ~ ~>")
  .s("triangle").decay(.25).sustain(0).lpf(1800).gain(.55)

// a question after "wait a minute..." and after "don't we have ..."
const hmm = note("<[~ ~ bb5 b5] ~ [bb5 b5 ~ ~] ~ ~>")
  .s("square").penv(2).pattack(.05).decay(.3).sustain(.2).release(.2)
  .lpf(2400).delay(.3).delaytime(.326).delayfeedback(.35).gain(.16)

// LIBSIXEL: a run up into a B flat major chime and a crash
const idea = stack(
  n("<~ ~ [~ ~ ~ [0 1 2 3 4 5 6 7]] ~ ~>").scale("Bb4:major")
    .s("square").decay(.06).sustain(0).gain(.18),
  note("<~ ~ ~ [bb5,d6,f6,bb6] ~>").s("triangle").decay(1.4).sustain(0).room(.5).gain(.35),
  tr909(s("<~ ~ ~ [bd,cr] ~>")).gain(.7),
)

// half a bar and a bar of build on a B flat pedal
const build = stack(
  tr909(s("<~ ~ ~ sd*16 [sd*16 ~]>")).gain("<0 0 0 [.25 .35 .45 .55] [.65 .8]>"),
  tr909(s("<~ ~ ~ bd*4 [bd*8 ~]>")),
  s("white*16").mask("<0 0 0 1 [1 0]>")
    .lpf("<0 0 0 [800 1200 1800 2600] [4000 8000]>").attack(.005).release(.05).gain(.12),
  note("<~ ~ ~ [bb1 bb2]*4 [[bb1 bb2]*4 ~]>")
    .s("sawtooth").lpf(800).lpq(6).decay(.12).sustain(.3).gain(.4),
)

stack(tape, clock, sneak, hmm, idea, build)
