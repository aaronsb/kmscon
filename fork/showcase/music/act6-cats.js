// Act 6, the cats: five bars of 138 BPM in G flat major, the relative major
// of the video's E flat minor. A square-wave tune whose long notes meow (a
// pitch envelope that rises and falls back), bloops for the cats popping in
// faster and faster, and a run up to D flat, the dominant of G flat, into
// the interlude of the cats in sunglasses (act6-shades).
// @bpm 138
// @bars 5
// @tail 1
setcpm(138/4)

const tr909 = p => p.bank("tr8")

// penv rises by that many semitones over pattack and falls back over pdecay
const tune = note(`<
    [db5 bb4 gb4 bb4 db5@2 eb5 db5]
    [bb4@2 gb4 bb4 eb5@3 ~]
    [eb5 db5 b4 db5 eb5 gb5@3]
    [f5@2 ab5@2 gb5 f5 eb5 db5]
    [db5 eb5 f5 gb5 ab5@4]
  >`)
  .penv(`<
    [0 0 0 0 5@2 0 0]
    [0@2 0 0 5@3 0]
    [0 0 0 0 0 5@3]
    [0@2 4@2 0 0 0 0]
    [0 0 0 0 7@4]
  >`)
  .panchor(0).psustain(0).pattack(.08).pdecay(.3)
  .s("square").lpf(3500).decay(.25).sustain(.5).release(.1)
  .delay(.2).delaytime(.217).delayfeedback(.25).room(.3)
  .gain(.22)

const chords = "<Gb Ebm B Db Db>"
const arp = n("[0 1 2 3]*4").chord(chords).anchor("gb5").voicing()
  .s("square").decay(.08).sustain(0).lpf(2500).gain(.12)

const bass = n("<[0 7]*4 [-2 5]*4 [-4 3]*4 [-3 4]*4 [4 11 4 11 4 ~ ~ ~]>")
  .scale("Gb2:major")
  .s("square").lpf(900).decay(.12).sustain(.2).gain(.3)

const drums = tr909(stack(
  s("<[bd ~ ~ bd ~ ~ bd ~]!4 [bd ~ ~ ~ [bd,cr] ~ ~ ~]>"),
  s("<[~ cp]*2!4 ~>").gain(.5),
  s("<[~ hh]*4!4 ~>").gain(.3),
))

// a bloop for each cat, denser as they pile in
const pops = s("sine*16").degradeBy("<.8 .6 .35 .15 1>")
  .note(irand(12).add(84)).penv(-7).pattack(.03)
  .decay(.05).sustain(0).pan(rand).gain(.18)

stack(tune, arp, bass, drums, pops)
