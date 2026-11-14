# azahar-touchprobe

A two-screen homebrew that prints what the emulated touch panel reports, used to
prove that a forwarded touch reaches a guest: bottom screen shows the last `x`/`y`
in panel pixels, whether it is held, and how many presses it has seen. `START` quits.

No game needed, no savedata, so a tap either shows up here or the input path under
test is not delivering touch.

```bash
tools/touchprobe/build.sh                       # needs docker, devkitPro image
QT_QPA_PLATFORM=xcb ./build/bin/Release/azahar --windowed tools/touchprobe/out/touchprobe.3dsx
```

Click the bottom screen: `x=`/`y=` change to the clicked point and `presses` increments.
