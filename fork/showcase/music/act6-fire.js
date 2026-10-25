// Act 6, the fire wipe: three bars of 138 BPM that burn hotter, a rumble
// under crackle and an arpeggio that doubles its speed each bar, then a hit
// on bar 3 as the fire goes out. "hold on, we also have kitty" types over
// the hit's ring and a little waiting figure in G flat major, until the cats
// come and their own cue takes over.
// @bpm 138
// @bars 6
// @tail 0
setcpm(138/4)

const tr909 = p => p.bank("RolandTR909")

const fire = stack(
  s("brown").attack(.8).release(.3).lpf(400).gain("<.35 .45 .55 0!3>"),
  note("<eb1 eb1 eb1 ~!3>").s("sine").attack(1).release(.2).gain(.45),
  tr909(s("hh*16")).degradeBy("<.7 .5 .3 1!3>").speed(rand.range(.6, 2)).gain(.22),
  n("<[0 2 4 7]*2 [0 2 4 7]*4 [0 2 4 7 9 11 14 11]*4 ~!3>").scale("Eb3:minor")
    .s("sawtooth").decay(.1).sustain(0).lpf("<600 1500 4000 600>").lpq(10)
    .shape(.3).gain(.2),
  tr909(s("<[lt ~ lt ~]*2 [lt mt]*4 [lt mt ht ht]*4 ~!3>")).gain(.45),
  tr909(s("<~ ~ bd*4 ~!3>")),
)

const out = stack(
  tr909(s("<~!3 [bd,cr] ~ ~>")),
  note("<~!3 [eb2,bb2,eb3,gb3,bb3] ~ ~>")
    .s("sawtooth").shape(.3).decay(2).sustain(0).lpf(3000).room(.7).size(.9).gain(.22),
  note("<~!3 eb1 ~ ~>").s("sine").penv(-24).pattack(.06).decay(1.4).sustain(0).gain(.7),
)

// hold on: a music box waiting in the relative major
const waiting = note("<~!4 [gb5 bb5 db6 bb5 ab5 bb5 db6 bb5] [gb5 bb5 eb6 db6 bb5 ab5 gb5 ~]>")
  .s("triangle").decay(.3).sustain(0).delay(.35).delaytime(.326).delayfeedback(.4)
  .room(.5).gain(.55)

stack(fire, out, waiting)
