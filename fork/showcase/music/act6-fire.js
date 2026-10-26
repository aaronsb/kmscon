// Act 6, the fire wipe and the CJK stream: three bars of 138 BPM that burn
// hotter, a rumble under crackle and an arpeggio that doubles its speed
// each bar, with data bleeps thickening as the CJK lines rise behind the
// flames; a hit on bar 3 as the fire goes out and the stream fills the
// screen, two driving bars under it, a stop on bar 5 as the screen clears,
// then "hold on, we also have kitty" types over the stop's ring and a
// little waiting figure in G flat major, until the cats come and their own
// cue takes over.
// @bpm 138
// @bars 8
// @tail 0
setcpm(138/4)

const tr909 = p => p.bank("RolandTR909")
const storm = "<0!3 1 1 0!3>"

const fire = stack(
  s("brown").attack(.8).release(.3).lpf(400).gain("<.35 .45 .55 0!5>"),
  note("<eb1 eb1 eb1 ~!5>").s("sine").attack(1).release(.2).gain(.45),
  tr909(s("hh*16")).degradeBy("<.7 .5 .3 1!5>").speed(rand.range(.6, 2)).gain(.22),
  n("<[0 2 4 7]*2 [0 2 4 7]*4 [0 2 4 7 9 11 14 11]*4 ~!5>").scale("Eb3:minor")
    .s("sawtooth").decay(.1).sustain(0).lpf("<600 1500 4000 600>").lpq(10)
    .shape(.3).gain(.2),
  tr909(s("<[lt ~ lt ~]*2 [lt mt]*4 [lt mt ht ht]*4 ~!5>")).gain(.45),
  tr909(s("<~ ~ bd*4 ~!5>")),
)

// the CJK lines: random bleeps, thicker as more of the screen streams
const data = n(irand(16).segment(32)).scale("Eb5:minor")
  .s("square").decay(.03).sustain(0).lpf(5000).pan(rand)
  .degradeBy("<1 .75 .5 .25 .1 1!3>").gain(.12)

// the hits: bar 3 as the fire goes out, bar 5 as the screen clears, its
// chord ringing on under the typing
const hits = stack(
  tr909(s("<~!3 [bd,cr] ~ [bd,cr] ~ ~>")),
  note("<~!3 [eb2,bb2,eb3,gb3,bb3] ~ [eb2,bb2,eb3,gb3,bb3] ~ ~>")
    .s("sawtooth").shape(.3).decay("<0!3 .8 0 2 0 0>").sustain(0).lpf(3000).room(.7).size(.9).gain(.22),
  note("<~!3 eb1 ~ eb1 ~ ~>").s("sine").penv(-24).pattack(.06).decay(1.4).sustain(0).gain(.7),
)

// the full-screen stream: two bars at full tilt, E flat minor to B flat
const drive = stack(
  tr909(stack(
    s("bd*4"),
    s("hh*16").gain("[.2 .3 .45 .3]*4"),
    s("<[~ cp ~ cp] [~ cp ~ [cp sd sd sd]]>"),
  )),
  n("<[0 7]*8 [4 11]*8>").scale("Eb2:minor")
    .s("sawtooth").lpf(900).lpq(6).decay(.1).sustain(.3).gain(.38),
  n("[0 1 2 3 4 5 6 7]*2").chord("<Ebm Bb>").anchor("eb6").voicing()
    .s("square").decay(.07).sustain(0).lpf(4000).delay(.2).delaytime(.109).delayfeedback(.3)
    .gain(.16),
).mask(storm)

// hold on: a music box waiting in the relative major
const waiting = note("<~!6 [gb5 bb5 db6 bb5 ab5 bb5 db6 bb5] [gb5 bb5 eb6 db6 bb5 ab5 gb5 ~]>")
  .s("triangle").decay(.3).sustain(0).delay(.35).delaytime(.326).delayfeedback(.4)
  .room(.5).gain(.55)

stack(fire, data, hits, drive, waiting)
