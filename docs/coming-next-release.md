# Coming next release

Reported issues still being worked on. Each says what we know so far and what would help, so if you have the save
or the screenshot we're asking for, please add it to the issue.

### Unlock Hard difficulty without finishing the game ([#64](https://github.com/gminustwo/primerecomp/issues/64))

Finishing the game records "Metroid Prime completed" as a gallery unlock, but the new-game Difficulty screen decides
whether to offer Hard somewhere in its own menu scripts. Setting the completed flags alone didn't make Hard appear.
A save from a game that has been finished (so Hard shows up) would make the flag easy to find.

### Two button prompts still show Switch buttons ([#52](https://github.com/gminustwo/primerecomp/issues/52))

The Plated Beetle fight's lock-on hint and the Charge Beam's "Press ZR / A to charge" use the game's normal button
tags, so their icons should change like everywhere else. Our guess is that in-play hints draw their icons from a copy
the game makes before PrimeRecomp swaps them. A save right before the Plated Beetle, run once with
`LARECOMP_LOG_GLYPHS=1`, plus `primerecomp.log` and a screenshot of the hint, would help.

### Visor steam and fog only cover the middle 16:9 on ultrawide screens ([#46](https://github.com/gminustwo/primerecomp/issues/46))

The X-ray visor, fades, flashes and the visor's water drops already fill a 21:9 window, so this is a specific steam
or fog effect. We need to know where it shows (a room, or what Samus is doing) to reproduce it: a screenshot or an F8
position there.

### Artifacts at the sides of the map screen ([#53](https://github.com/gminustwo/primerecomp/issues/53))

The screenshot attached so far looks clean, apart from a 2-pixel strip at the left edge that seems to be the window
border. A circled screenshot or a short video, plus whether it was windowed or fullscreen and the Video tab's Aspect
ratio and Pause screen settings, would help.

### Rising lava in Magmoor Caverns doesn't stop or burn ([#54](https://github.com/gminustwo/primerecomp/issues/54))

In the narrow section on the way to the Phendrana Drifts elevator, the lava keeps rising out of bounds and touching it
does nothing. Likely the lava's damage volume isn't following its surface, or a script message is lost. A save near
that room would let us test it.

### Audio pop going in and out of water ([#55](https://github.com/gminustwo/primerecomp/issues/55))

This release fades the underwater sound filter in and out over 5 ms instead of switching it instantly, which should
remove the pop. We couldn't hear or measure the pop in our own recordings, so please tell us whether it's gone.
