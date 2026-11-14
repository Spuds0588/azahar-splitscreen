// azahar-touchprobe - reads the emulated touch panel and prints what it sees.
//
// Minimal responder for the split-screen input spike: no game, no savedata, so a
// tap either shows up here or the path under test is not delivering touch at all.
// Bottom screen: last touch position, whether it is held and how many presses.
// START quits.
#include <3ds.h>
#include <stdio.h>

int main(int argc, char** argv) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    printf("\x1b[2;2HAZAHAR TOUCHPROBE");
    printf("\x1b[4;2Htap the bottom screen");
    printf("\x1b[6;2HSTART quits");
    u32 presses = 0;
    touchPosition last = {0};
    bool was_pressed = false;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        touchPosition touch;
        hidTouchRead(&touch);
        const bool pressed = (hidKeysHeld() & KEY_TOUCH) != 0;
        if (pressed) {
            last = touch;
            if (!was_pressed) {
                presses++;
            }
        }
        was_pressed = pressed;
        printf("\x1b[9;2Hx=%3u y=%3u      ", last.px, last.py);
        printf("\x1b[11;2Hheld=%d presses=%lu", pressed ? 1 : 0, (unsigned long)presses);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
