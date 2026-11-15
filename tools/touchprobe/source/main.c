// azahar-touchprobe - reads the emulated touch panel and buttons and prints what it sees.
//
// Minimal responder for the split-screen input spike: no game, no savedata, so a
// tap or a key either shows up here or the path under test is not delivering input.
// Top screen: last key-down mask, how many keys were seen, and the circle pad's
// position (so a forwarded stick can be checked for magnitude, not just direction).
// Bottom screen: last touch position, whether it is held and how many presses.
// START quits.
#include <3ds.h>
#include <stdio.h>

int main(int argc, char** argv) {
    gfxInitDefault();
    PrintConsole top_console;
    PrintConsole bottom_console;
    consoleInit(GFX_TOP, &top_console);
    consoleInit(GFX_BOTTOM, &bottom_console);

    consoleSelect(&top_console);
    printf("\x1b[8;6HAZAHAR SPLIT-SEAT PROBE");
    printf("\x1b[10;6Hkeys=0 last=00000000");
    printf("\x1b[11;6Hcircle x=  0 y=  0");

    consoleSelect(&bottom_console);
    printf("\x1b[2;2HAZAHAR TOUCHPROBE");
    printf("\x1b[4;2Htap the bottom screen");
    printf("\x1b[6;2HSTART quits");

    u32 presses = 0;
    u32 keypresses = 0;
    u32 last_key = 0;
    touchPosition last = {0};
    bool was_pressed = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (down & ~KEY_START) {
            keypresses++;
            last_key = down;
            consoleSelect(&top_console);
            printf("\x1b[10;6Hkeys=%lu last=%08lx   ", (unsigned long)keypresses,
                   (unsigned long)last_key);
            consoleSelect(&bottom_console);
        }
        consoleSelect(&top_console);
        circlePosition circle;
        hidCircleRead(&circle);
        printf("\x1b[11;6Hcircle x=%3d y=%3d   ", (int)circle.dx, (int)circle.dy);
        consoleSelect(&bottom_console);

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
