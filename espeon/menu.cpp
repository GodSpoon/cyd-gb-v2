#include <vector>
#include <Arduino.h>
#include <TFT_eSPI.h>

#include "menu.h"
#include "espeon.h"

// TFT_eSPI instance (declared in espeon.cpp)
extern TFT_eSPI tft;

// Touch screen pin for CYD
#define TOUCH_CS 33
// Alternative touch implementation using analog read
#define TOUCH_THRESHOLD 100  // Adjust based on hardware

// File browser for CYD
static std::vector<String> romFiles;
static String rompath = "";
static const char* rompath_c = nullptr;
static bool running;
static int selectedIndex = 0;
static int scrollOffset = 0;
static const int maxVisibleFiles = 8;
static bool touchPressed = false;
static unsigned long lastTouchTime = 0;

// Get ROM files from espeon (avoids SPI conflicts)
void scanForRomFiles() {
	Serial.println("Getting ROM file list from espeon...");
	
	// Get the ROM file list from espeon.cpp (populated during SD initialization)
	const std::vector<String>& availableFiles = espeon_get_rom_files();
	
	// Copy to our local vector
	romFiles.clear();
	romFiles.reserve(availableFiles.size());
	for (const String& file : availableFiles) {
		romFiles.push_back(file);
	}
	
	Serial.printf("Retrieved %d ROM files from espeon\n", romFiles.size());
	
	// List them for debugging
	for (size_t i = 0; i < romFiles.size(); i++) {
		Serial.printf("  ROM %d: %s\n", i + 1, romFiles[i].c_str());
	}
}

// Improved touch detection using analog reading
bool isTouchPressed() {
	// Method 1: Digital read (basic)
	pinMode(TOUCH_CS, INPUT_PULLUP);
	bool digitalTouch = !digitalRead(TOUCH_CS);
	
	// Method 2: Check for GPIO interrupt (if touch generates interrupt)
	// This would need to be set up in init
	
	// For now, use digital method with some debouncing
	static bool lastTouch = false;
	static unsigned long lastChangeTime = 0;
	unsigned long currentTime = millis();
	
	if (digitalTouch != lastTouch) {
		if ((currentTime - lastChangeTime) > 20) { // 20ms debounce
			lastTouch = digitalTouch;
			lastChangeTime = currentTime;
		}
	}
	
	return lastTouch;
}

// Enhanced touch position detection (future improvement)
void getTouchPosition(int* x, int* y) {
	// This is a placeholder for proper XPT2046 touch controller reading
	// The CYD typically uses XPT2046 which requires SPI communication
	// For now, we'll simulate center position
	*x = 160; // Center of 320px width
	*y = 120; // Center of 240px height
	
	// TODO: Implement proper XPT2046 touch controller communication
	// This would involve SPI reads to get actual X,Y coordinates
}

// Draw the file browser menu
void drawFileBrowser() {
	// Begin TFT transaction to ensure exclusive SPI access
	tft.startWrite();
	
	tft.fillScreen(TFT_BLACK);
	
	// Title
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.setCursor(10, 10);
	tft.print("ROM Browser");
	
	// Instructions
	tft.setTextSize(1);
	tft.setCursor(10, 35);
	tft.setTextColor(TFT_YELLOW);
	tft.print("Touch: navigate | Long press: load");
	tft.setCursor(10, 45);
	tft.setTextColor(TFT_CYAN);
	tft.print("Auto-selects in 10s if no touch");
	
	// File list
	tft.setTextSize(1);
	int startY = 65;
	int lineHeight = 18;
	
	if (romFiles.empty()) {
		tft.setTextColor(TFT_RED);
		tft.setCursor(10, startY);
		tft.print("No .gb files found!");
		tft.setCursor(10, startY + 20);
		tft.print("Place ROM files on SD card");
		tft.setCursor(10, startY + 40);
		tft.setTextColor(TFT_WHITE);
		tft.print("Supported: .gb and .GB files");
		tft.endWrite(); // End transaction before returning
		return;
	}
	
	// Show ROM count
	tft.setTextSize(1);
	tft.setTextColor(TFT_WHITE);
	tft.setCursor(200, 35);
	tft.printf("ROMs: %d", romFiles.size());
	
	// Draw visible files
	for (int i = 0; i < maxVisibleFiles && (i + scrollOffset) < romFiles.size(); i++) {
		int fileIndex = i + scrollOffset;
		int y = startY + (i * lineHeight);
		
		// Highlight selected file
		if (fileIndex == selectedIndex) {
			tft.fillRect(5, y - 2, 310, lineHeight - 2, TFT_BLUE);
			tft.setTextColor(TFT_WHITE);
		} else {
			tft.setTextColor(TFT_GREEN);
		}
		
		// Show selection indicator
		tft.setCursor(5, y);
		if (fileIndex == selectedIndex) {
			tft.print(">");
		} else {
			tft.print(" ");
		}
		
		tft.setCursor(20, y);
		String displayName = romFiles[fileIndex];
		// Remove path and extension for display
		int lastSlash = displayName.lastIndexOf('/');
		if (lastSlash >= 0) {
			displayName = displayName.substring(lastSlash + 1);
		}
		int lastDot = displayName.lastIndexOf('.');
		if (lastDot >= 0) {
			displayName = displayName.substring(0, lastDot);
		}
		
		// Truncate long names
		if (displayName.length() > 30) {
			displayName = displayName.substring(0, 27) + "...";
		}
		
		tft.print(displayName);
	}
	
	// Scroll indicators
	if (scrollOffset > 0) {
		tft.setTextColor(TFT_YELLOW);
		tft.setCursor(300, 70);
		tft.print("^");
	}
	if ((scrollOffset + maxVisibleFiles) < romFiles.size()) {
		tft.setTextColor(TFT_YELLOW);
		tft.setCursor(300, 210);
		tft.print("v");
	}
	
	// Show current selection info at bottom
	if (romFiles.size() > 0) {
		tft.setTextColor(TFT_WHITE);
		tft.setTextSize(1);
		tft.setCursor(10, 225);
		tft.printf("Selection: %d/%d", selectedIndex + 1, romFiles.size());
	}
	
	// End TFT transaction
	tft.endWrite();
}

// Handle touch input for navigation
void handleTouchInput() {
	bool currentTouch = isTouchPressed();
	unsigned long currentTime = millis();
	
	// Detect touch press (edge detection)
	if (currentTouch && !touchPressed) {
		touchPressed = true;
		lastTouchTime = currentTime;
	}
	
	// Detect touch release
	if (!currentTouch && touchPressed) {
		touchPressed = false;
		unsigned long touchDuration = currentTime - lastTouchTime;
		
		if (touchDuration > 50 && touchDuration < 1000) {
			// Short press - navigate
			selectedIndex++;
			if (selectedIndex >= romFiles.size()) {
				selectedIndex = 0;
				scrollOffset = 0;
			} else if (selectedIndex >= (scrollOffset + maxVisibleFiles)) {
				scrollOffset++;
			}
			drawFileBrowser();
		} else if (touchDuration >= 1000) {
			// Long press - select ROM
			if (selectedIndex < romFiles.size()) {
				rompath = romFiles[selectedIndex];
				rompath_c = rompath.c_str();
				running = false;
				
				// Show selection confirmation
				tft.startWrite();
				tft.fillRect(0, 220, 320, 20, TFT_GREEN);
				tft.setTextColor(TFT_BLACK);
				tft.setTextSize(1);
				tft.setCursor(10, 225);
				tft.print("Selected: ");
				String displayName = rompath;
				int lastSlash = displayName.lastIndexOf('/');
				if (lastSlash >= 0) {
					displayName = displayName.substring(lastSlash + 1);
				}
				tft.print(displayName);
				tft.endWrite();
				delay(1000);
			}
		}
	}
}

// Fallback navigation without touch (for testing/when touch doesn't work)
void handleButtonNavigation() {
	// Use the existing joypad GPIO if available for navigation
	// This allows navigation even when touch isn't working
	static unsigned long lastButtonTime = 0;
	unsigned long currentTime = millis();
	
	if ((currentTime - lastButtonTime) > 300) { // 300ms button repeat
		// Check if joypad button is pressed (from espeon.cpp JOYPAD_INPUT)
		// For now, just auto-advance every few seconds if no touch
		if (!touchPressed && romFiles.size() > 0) {
			static unsigned long autoAdvanceTime = 0;
			if ((currentTime - autoAdvanceTime) > 2000) { // Auto advance every 2 seconds
				selectedIndex = (selectedIndex + 1) % romFiles.size();
				if (selectedIndex < scrollOffset || selectedIndex >= (scrollOffset + maxVisibleFiles)) {
					scrollOffset = selectedIndex - (maxVisibleFiles / 2);
					if (scrollOffset < 0) scrollOffset = 0;
					if (scrollOffset > (romFiles.size() - maxVisibleFiles)) {
						scrollOffset = romFiles.size() - maxVisibleFiles;
						if (scrollOffset < 0) scrollOffset = 0;
					}
				}
				drawFileBrowser();
				autoAdvanceTime = currentTime;
			}
		}
		lastButtonTime = currentTime;
	}
}

// Auto-select mechanism (if touch doesn't work)
void handleAutoSelect() {
	static unsigned long menuStartTime = 0;
	static bool autoSelectTriggered = false;
	
	if (menuStartTime == 0) {
		menuStartTime = millis();
	}
	
	// Auto-select the first ROM after 10 seconds if no interaction
	if (!autoSelectTriggered && (millis() - menuStartTime) > 10000 && romFiles.size() > 0) {
		autoSelectTriggered = true;
		selectedIndex = 0;
		rompath = romFiles[selectedIndex];
		rompath_c = rompath.c_str();
		running = false;
		
		// Show auto-selection message
		tft.startWrite();
		tft.fillRect(0, 220, 320, 20, TFT_ORANGE);
		tft.setTextColor(TFT_BLACK);
		tft.setTextSize(1);
		tft.setCursor(10, 225);
		tft.print("Auto-selected: ");
		String displayName = rompath;
		int lastSlash = displayName.lastIndexOf('/');
		if (lastSlash >= 0) {
			displayName = displayName.substring(lastSlash + 1);
		}
		tft.print(displayName);
		tft.endWrite();
		delay(1500);
	}
}

const char* menu_get_rompath()
{
	return rompath_c;
}

void menu_init()
{
	// Initialize touch pin
	pinMode(TOUCH_CS, INPUT_PULLUP);
	
	// Initialize variables
	running = true;
	rompath_c = nullptr;
	selectedIndex = 0;
	scrollOffset = 0;
	touchPressed = false;
	
	// Clear display with proper SPI transaction
	tft.startWrite();
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.endWrite();
	
	// Scan for ROM files
	scanForRomFiles();
}

void menu_loop()
{
	drawFileBrowser();
	
	// Main menu loop
	while (running) {
		handleTouchInput();
		handleButtonNavigation(); // Check for button navigation
		handleAutoSelect(); // Check for auto-select
		delay(50); // Small delay to prevent excessive polling
		
		// Handle case where no ROMs are found
		if (romFiles.empty()) {
			delay(3000);
			running = false;
			rompath_c = nullptr;
			break;
		}
	}
	
	// Ensure TFT SPI transaction is properly ended before returning
	tft.endWrite();
	delay(100);  // Allow SPI bus to settle completely
	
	Serial.println("Menu loop completed, SPI resources cleaned up");
}
