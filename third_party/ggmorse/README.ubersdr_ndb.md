# ggmorse (vendored)

Copied from `ka9q_ubersdr/audio_extensions/morse/external/ggmorse`, which is
itself the copy UberSDR's `cw-decoder` is built from. Upstream:
https://github.com/ggerganov/ggmorse (MIT licence, Georgi Gerganov).

Local change: the per-character debug echo to stderr in `GGMorse::decode()` is
removed. The NDB decoder runs one instance per beacon, and with a dozen of them
the echo interleaves into an unreadable stream on the container log.

Local fix: `setParametersDecode()` re-initialised the high- and low-pass
filters from the *previous* range limits rather than the new ones, so a range
change took effect one call late. It now uses the incoming parameters.

Local addition: `ParametersDecode` gained `speedRangeMin_wpm` /
`speedRangeMax_wpm` (default 5 / 55, i.e. upstream behaviour). The automatic
speed search spans that range in about five coarse steps instead of the fixed
5, 15, ... 55 wpm grid. NDB idents are sent at roughly 6-12 wpm, which that
grid straddles; with the long silences between idents the unconstrained search
wandered to 35-55 wpm and decoded nothing on some beacons.

Local fix (memory): the constructor no longer reserves 100 `Interval`s in each
of the 100x100 `intervalsAll` slots (~28 MB per instance, almost all unused).

Not a change to ggmorse, but a requirement on its caller: `thresholdF` (and
`signalF`) grow by one entry per decoded frame until drained with
`takeThresholdF()` / `takeSignalF()`. A long-running decoder must drain them;
`NdbChannel::run_morse()` does.
