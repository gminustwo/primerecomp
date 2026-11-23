# Coming next release

Reported issues still being worked on. Each says what we know so far and what would help, so if you have the save
or the screenshot we're asking for, please add it to the issue.

### Two button prompts may still show Switch buttons ([#52](https://github.com/gminustwo/primerecomp/issues/52))

v0.1.0 draws the hints that show two buttons in one picture ("X + d-pad", "ZR / A") with your buttons, which may
cover the Charge Beam's "Press ZR / A to charge". If the Plated Beetle fight's lock-on hint or the Charge Beam's still
shows Switch buttons, a save right before the Plated Beetle, run once with `LARECOMP_LOG_GLYPHS=1`, plus
`primerecomp.log` and a screenshot of the hint, would help.

### Dark blocks around the map on AMD RX 7000 cards on Linux ([#53](https://github.com/gminustwo/primerecomp/issues/53))

On an RX 7600 XT with Linux's RADV driver, the map screen showed dark 8 x 8-pixel blocks along the map's edges. v0.1.2
leaves out the map's edge-smoothing pass on RX 7000 cards with RADV, which made the map right for the reporter (its
edges are a little rougher there). The cause in the driver is still unknown. If you still see the blocks on v0.1.2,
please add your `primerecomp.log` to the issue.

### Rising lava in Magmoor Caverns doesn't stop or burn ([#54](https://github.com/gminustwo/primerecomp/issues/54))

In the narrow section on the way to the Phendrana Drifts elevator, the lava keeps rising out of bounds and touching it
does nothing. We're aware of it and know where it happens, so there's no need to send anything for this one.

### Audio pop going in and out of water ([#55](https://github.com/gminustwo/primerecomp/issues/55))

Since v0.0.11 the underwater sound filter fades in and out over 5 ms instead of switching instantly, which should
remove the pop. We couldn't hear or measure the pop in our own recordings, so please tell us whether it's gone.
