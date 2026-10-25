// Act 2, "we can do better!": the dramatic pause, the letters, the plasma
// and the shadebobs, one bar each of 138 BPM in E flat minor (the key and
// tempo of the video's soundtrack, which picks up again in act 3).
// The pause is 19 sixteenths, so the letters land on the sixteenths after it
// and the "!" on bar 2's downbeat; the plasma starts on bar 3, fading in over
// it, the shadebobs on bar 7, and bar 10 is the last hit.
// @bpm 138
// @bars 10
// @tail 1.7
setcpm(138/4)

const prog = "<Ebm!3 Ebm B Gb Db Abm B Bb Ebm>"
const band = "<0!3 1!7 0>"
const tr909 = p => p.bank("RolandTR909")

// the pause: a sub swell and stepped noise climbing to the first letter
const pause = stack(
  note("<eb1 ~!10>").s("sine").attack(1.5).release(.2).gain(.5),
  s("white*16").mask("<1 [1@3 0@13] 0!9>")
    .lpf(saw.range(200, 9000).slow(19/16)).gain(saw.range(.03, .3).slow(19/16))
    .attack(.005).release(.05),
)

// one rising blip and tom per letter, then the "!" on bar 2
const letters = stack(
  n("<~ [~ ~ ~ 0 1 2 3 4 5 6 7 8 9 10 11 12] ~!9>").scale("Eb3:minor")
    .s("square").decay(.12).sustain(0).lpf(3200).room(.3).gain(.32),
  tr909(s("<~ [~ ~ ~ lt lt lt lt lt mt mt mt mt ht ht ht ht] ~!9>")).gain(.6),
)
const slam = stack(
  tr909(s("<~ ~ [bd,cr] ~!7 [bd,cr]>")),
  note("<~ ~ [eb2,bb2,eb3,gb3,bb3] ~!7 [eb2,bb2,eb3,gb3,bb3]>")
    .s("sawtooth").shape(.3).attack(.003).decay(1.6).sustain(.1).release(.6)
    .lpf(3500).room(.5).gain(.22),
  note("<~ ~ eb1 ~!7 eb1>").s("sine").penv(-24).pattack(.06).decay(1.2).sustain(0).gain(.7),
)

const drums = tr909(stack(
  s("<~!3 bd*4!7 ~>"),
  s("<~!3 [~ oh]*4!7 ~>").gain(.35),
  s("<~!5 hh*16!5 ~>").gain("[.2 .3 .45 .3]*4"),
  s("<~!4 [~ cp ~ cp]!5 ~ ~>").gain(.6),
  s("<~ ~ [~@12 sd sd sd sd] ~!6 [~ sd ~ sd [sd sd] [sd sd] [sd sd sd sd] [sd sd sd sd]] ~>").gain(.55),
  s("<~!7 cr ~!3>").gain(.6),
))

const bass = n("<~!3 [0 7]*4 [-2 5]*4 [2 9]*4 [-1 6]*4 [3 10]*4 [-2 5]*4 [4 11]*4 ~>")
  .scale("Eb2:minor")
  .s("sawtooth").lpf(700).lpq(6).decay(.15).sustain(.3).gain(.4)

const arp = n("[0 1 2 3 4 3 2 1]*2").chord(prog).anchor("eb5").voicing()
  .s("square").attack(.002).decay(.12).sustain(0)
  .lpf("<400!3 [500 1000 2000 3500] 4000!6 400>").lpq(8)
  .delay(.25).delaytime(.217).delayfeedback(.3).room(.3)
  .gain(.24).mask(band)

const pad = chord(prog).voicing()
  .s("sawtooth").attack(.4).release(1.2).lpf(1100)
  .vib(4).vibmod(.06).room(.7).size(.9).gain(.1).mask(band)

// the shadebobs' lead: a distorted saw that glides into its leaps (penv:
// the semitones it slides from) and ends on a long bend up
const lead = note("<~!7 [ab4@2 b4 eb5@3 gb5 f5] [gb5@3 eb5 db5@2 b4 db5] [f5@4 d5@2 bb4 d5] eb5>")
  .penv("<0!7 [0@2 0 4@3 0 0] [1@3 0 0@2 0 0] [2@4 0@2 0 4] 12>")
  .pattack("<.07!10 .3>")
  .s("sawtooth").shape(.55).lpf(3000).lpq(3)
  .attack(.005).decay(.4).sustain(.65).release(.3)
  .vib(5.5).vibmod(.15)
  .room(.45).size(.6).delay(.3).delaytime(.326).delayfeedback(.35)
  .gain(.3)

stack(pause, letters, slam, drums, bass, arp, pad, lead)
