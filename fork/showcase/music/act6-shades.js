// The interlude between act 6 and act 7: the cats in sunglasses. Four bars
// of 138 BPM in G flat major. A bloop as each face pops in on the first six
// eighths; then, one a beat from bar 1, a slide whistle as each pair of
// sunglasses falls and a synth meow and a click as it lands, over a laid
// back half-time beat; a cool major seventh once all six are on; and on
// bar 3, as the row slides off to the left, a gust panning right to left
// over a B flat chord, the dominant of the video's E flat minor, which
// comes back fading in. Times match fork/showcase/shades.py.
// @bpm 138
// @bars 4
// @tail .6
setcpm(138/4)

const tr909 = p => p.bank("tr8")

const pops = n("<[0 2 4 7 9 11 ~ ~] ~!3>").scale("Gb4:major")
  .s("sine").penv(-5).pattack(.03).decay(.1).sustain(0).gain(.4)

// each pair falls for three quarters of a beat: a whistle gliding down an
// octave onto the note its meow lands on
const whistle = note(`<
    [~@13 gb6 ~ ~]
    [~ bb6 ~ ~ ~ db7 ~ ~ ~ eb7 ~ ~ ~ db7 ~ ~]
    [~ gb7 ~ ~ ~@12]
    ~
  >`).s("sine").penv(-12).pattack(.32).decay(.33).sustain(0).gain(.12)

const meows = note("<~ [gb5 bb5 db6 eb6] [db6 gb6 ~ ~] ~>")
  .s("square").penv(5).panchor(0).psustain(0).pattack(.07).pdecay(.25)
  .decay(.4).sustain(.2).release(.1).lpf(3000)
  .delay(.25).delaytime(.217).delayfeedback(.3).gain(.22)

const clicks = tr909(s("<~ [rim rim rim rim] [rim rim ~ ~] ~>")).gain(.5)

const groove = stack(
  tr909(stack(
    s("<~ [bd ~ ~ bd ~ ~ ~ ~]!2 ~>"),
    s("<~ [~ ~ ~ ~ sd ~ ~ ~]!2 ~>").gain(.6),
    s("<~ hh*8!2 ~>").gain(.25),
  )),
  note("<~ [gb1 ~ ~ gb1 ~ ~ db2 ~] [eb2 ~ ~ eb2 ~ ~ db2 ~] ~>")
    .s("sawtooth").lpf(500).decay(.3).sustain(.4).gain(.38),
)

// all six on: a cool major seventh, held
const cool = note("<~ ~ [~ ~ [gb3,bb3,db4,f4] ~] ~>")
  .s("sawtooth").attack(.02).decay(1.2).sustain(.3).release(.4).lpf(1800)
  .vib(4).vibmod(.08).room(.6).size(.8).gain(.16)

// bar 3: the row slides off left on a gust over B flat
const away = stack(
  s("<~!3 white*16>")
    .lpf("[600 1000 1600 2400 3200 4000 4600 4800 4600 4000 3200 2400 1700 1100 700 400]")
    .gain("[.15 .3 .45 .6 .75 .9 1 1 .95 .85 .7 .55 .4 .28 .16 .06]")
    .pan("[.9 .85 .8 .75 .7 .65 .6 .55 .45 .4 .35 .3 .25 .2 .15 .1]")
    .attack(.01).release(.08),
  note("<~!3 [bb2,d3,f3,bb3]>").s("sawtooth").attack(.6).release(.3).lpf(1200).gain(.14),
)

stack(pops, whistle, meows, clicks, groove, cool, away)
