// The session window's menu bar.
//
// A couch session hides the picker, so this is the only place the player can
// ask what the controls are or change the layout mid-game: View (both screens /
// top only, fullscreen), Controls (the mapping dialog and the reference) and
// Session (quit).

#pragma once

class QMainWindow;
class Viewer;

void attach_menus(QMainWindow& window, Viewer& viewer);
