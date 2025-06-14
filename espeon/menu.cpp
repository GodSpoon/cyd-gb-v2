#include <vector>
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <FS.h>

#include "menu.h"

// TFT_eSPI instance (declared in espeon.cpp)
extern TFT_eSPI tft;

// Simplified menu system for CYD
static String rompath = "";
static const char* rompath_c = nullptr;
static bool running;

// Simplified menu callbacks
void cb_menu_newrom(void* sender)
{
	if (rompath != "") {
		rompath_c = rompath.c_str();
		running = false;
	}
}

void cb_menu_loadrom(void* sender)
{
	// Simplified implementation
	// TODO: Implement file browser for CYD
}

void cb_menu_lastrom(void* sender)
{
	// Try to load last ROM from flash
	rompath_c = nullptr; // For now, just indicate no ROM
	running = false;
}

const char* menu_get_rompath()
{
	return rompath_c;
}

void menu_init()
{
	// Simplified menu initialization for CYD
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	
	running = true;
	rompath_c = nullptr;
}

void menu_loop()
{
	// Simplified menu loop for CYD
	tft.fillScreen(TFT_BLACK);
	tft.setCursor(10, 10);
	tft.print("Espeon v1.0 (CYD)");
	tft.setCursor(10, 40);
	tft.print("Menu - Simplified");
	tft.setCursor(10, 70);
	tft.print("File browser TODO");
	tft.setCursor(10, 100);
	tft.print("Touch screen TODO");
	
	// For now, just wait and exit menu
	// TODO: Implement proper touch-based file browser
	delay(3000); // Show message for 3 seconds
	running = false;
}
