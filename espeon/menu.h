#ifndef MENU_H
#define MENU_H

void menu_init();
void menu_loop();
const char* menu_get_rompath();

// Internal functions
void scanForRomFiles();
bool isTouchPressed();
void getTouchPosition(int* x, int* y);
void drawFileBrowser();
void handleTouchInput();
void handleButtonNavigation();
void handleAutoSelect();

#endif
