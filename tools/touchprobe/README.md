# azahar-touchprobe

A two-screen homebrew that prints what the emulated input reports, used to prove that a
forwarded key or touch reaches a guest: bottom screen shows the last `x`/`y` in panel
pixels, whether it is held, and how many presses it has seen; top screen shows how many
key-down events arrived and the last key mask. `START` quits.

No game needed, no savedata, so a tap either shows up here or the input path under
test is not delivering touch.

```bash
tools/touchprobe/build.sh                       # needs docker, devkitPro image
QT_QPA_PLATFORM=xcb ./build/bin/Release/azahar --windowed tools/touchprobe/out/touchprobe.3dsx
```

Click the bottom screen: `x=`/`y=` change to the clicked point and `presses` increments.
Press a key: `keys=` increments and `last=` shows its mask. Both screens stay still
between inputs, which makes the title usable for byte-identical per-seat comparisons.
