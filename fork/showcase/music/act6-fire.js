// Act 6, the fire wipe and the CJK stream: three bars of 138 BPM that burn
// hotter, a rumble under crackle and an arpeggio that doubles its speed
// each bar, with data bleeps thickening as the CJK lines rise behind the
// flames; a hit on bar 3 as the fire goes out and the stream fills the
// screen, two driving bars under it, then on bar 5 a gust of wind, panning
// left to right, as the lines blow away. A music box in G flat major fades
// in under "hold on, we also have kitty" until the cats come and their own
// cue takes over.
// @bpm 138
// @bars 9
// @tail 0
setcpm(138/4)

const tr909 = p => p.bank("tr8")
const storm = "<0!3 1 1 0!4>"

const fire = stack(
  s("brown").attack(.8).release(.3).lpf(400).gain("<.35 .45 .55 0!6>"),
  note("<eb1 eb1 eb1 ~!6>").s("sine").attack(1).release(.2).gain(.45),
  tr909(s("hh*16")).degradeBy("<.7 .5 .3 1!6>").speed(rand.range(.6, 2)).gain(.22),
  n("<[0 2 4 7]*2 [0 2 4 7]*4 [0 2 4 7 9 11 14 11]*4 ~!6>").scale("Eb3:minor")
    .s("sawtooth").decay(.1).sustain(0).lpf("<600 1500 4000 600>").lpq(10)
    .shape(.3).gain(.2),
  tr909(s("<[lt ~ lt ~]*2 [lt mt]*4 [lt mt ht ht]*4 ~!6>")).gain(.45),
  tr909(s("<~ ~ bd*4 ~!6>")),
)

// the CJK lines: random bleeps, thicker as more of the screen streams
const data = n(irand(16).segment(32)).scale("Eb5:minor")
  .s("square").decay(.03).sustain(0).lpf(5000).pan(rand)
  .degradeBy("<1 .75 .5 .25 .1 1!4>").gain(.12)

// bar 3, as the fire goes out
const hit = stack(
  tr909(s("<~!3 [bd,cr] ~!5>")),
  note("<~!3 [eb2,bb2,eb3,gb3,bb3] ~!5>")
    .s("sawtooth").shape(.3).decay(.8).sustain(0).lpf(3000).room(.7).size(.9).gain(.22),
  note("<~!3 eb1 ~!5>").s("sine").penv(-24).pattack(.06).decay(1.4).sustain(0).gain(.7),
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

// bar 5, the wind: noise that swells and opens, then falls and closes,
// travelling from left to right with the lines
const wind = s("<~!5 white*16 ~!3>")
  .lpf("<0!5 [500 900 1600 2600 3800 4800 5200 5000 4400 3600 2800 2000 1400 900 600 400] 0!3>")
  .gain("<0!5 [.15 .3 .45 .62 .8 .95 1.05 1.05 1 .9 .75 .6 .45 .32 .2 .08] 0!3>")
  .pan("<0!5 [.1 .15 .2 .25 .3 .35 .4 .45 .55 .6 .65 .7 .75 .8 .85 .9] 0!3>")
  .attack(.01).release(.08)
  .room(.4)

// hold on: a music box in the relative major, fading in over its first bar
const waiting = note(`<
    ~!6
    [gb5 bb5 db6 bb5 ab5 bb5 db6 bb5]
    [gb5 bb5 eb6 db6 bb5 ab5 gb5 ~]
    [gb5 bb5 db6 bb5 ab5 bb5 db6 bb5]
  >`)
  .s("triangle").decay(.3).sustain(0).delay(.35).delaytime(.326).delayfeedback(.4)
  .room(.5).gain("<0!6 [.03 .08 .14 .21 .29 .37 .45 .52] .55 .55>")

stack(fire, data, hit, drive, wind, waiting)
