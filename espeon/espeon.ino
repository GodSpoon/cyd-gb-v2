#include <TFT_eSPI.h>
#include "timer.h"
#include "rom.h"
#include "mem.h"
#include "cpu.h"
#include "lcd.h"
#include "espeon.h"
#include "menu.h"

#include "gbfiles.h"

// External reference to TFT (defined in espeon.cpp)
extern TFT_eSPI tft;

void setup()
{
	espeon_init();
	
	menu_init();
	menu_loop();
	
	// Give extra time for SPI bus to settle after menu operations
	Serial.println("Preparing SPI bus for ROM loading...");
	tft.endWrite();  // Ensure all TFT operations are complete
	delay(200);      // Extended delay for complete SPI bus settling
	
	// Check memory before ROM loading
	espeon_check_memory();
	
	Serial.println("Loading ROM...");
	const char* selected_rom_path = menu_get_rompath();
	if (selected_rom_path) {
		Serial.printf("User selected ROM: %s\n", selected_rom_path);
	} else {
		Serial.println("No ROM selected from menu, trying first available ROM");
		// Try to get the first available ROM
		if (espeon_get_rom_count() > 0) {
			const auto& rom_files = espeon_get_rom_files();
			selected_rom_path = rom_files[0].c_str();
			Serial.printf("Using first ROM: %s\n", selected_rom_path);
		}
	}
	
	const uint8_t* rom = espeon_load_rom(selected_rom_path);
	if (!rom) {
		Serial.println("Failed to load ROM from SD, checking for internal ROM");
		rom = (const uint8_t*)gb_rom;
		if (!rom) {
			// Show error message on screen for user feedback
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("ROM Load Failed!");
			tft.setTextSize(1);
			tft.setCursor(10, 80);
			tft.print("No .gb files found on SD card");
			tft.setCursor(10, 100);
			tft.print("and no internal ROM available.");
			tft.setCursor(10, 130);
			tft.setTextColor(TFT_WHITE);
			tft.print("Please:");
			tft.setCursor(10, 150);
			tft.print("1. Insert SD card with .gb files");
			tft.setCursor(10, 170);
			tft.print("2. Reset the device");
			delay(5000);
			espeon_faint("No ROM available (SD failed, no internal ROM)");
		}
	} else {
		Serial.println("ROM loaded successfully!");
	}
	
	Serial.println("Loading Boot ROM...");
	const uint8_t* bootrom = espeon_load_bootrom("/gb_bios.bin");
	if (!bootrom) {
		Serial.println("Failed to load bootrom from SD, using internal BIOS");
		bootrom = (const uint8_t*)gb_bios;
		if (!bootrom) {
			Serial.println("Warning: No bootrom available, will skip boot sequence");
		}
	}
	
	Serial.println("Initializing ROM...");
	if (!rom_init(rom))
		espeon_faint("rom_init failed.");
	
	Serial.println("Initializing MMU...");
	if (!mmu_init(bootrom))
		espeon_faint("mmu_init failed.");
	
	Serial.println("Initializing LCD...");
	if (!lcd_init())
		espeon_faint("lcd_init failed.");
	
	cpu_init();
	
	espeon_render_border((const uint8_t*)gb_border, gb_border_size);
	
	while(true) {
		uint32_t cycles = cpu_cycle();
		espeon_update();
		lcd_cycle(cycles);
		timer_cycle(cycles);
		
		// Yield occasionally to prevent watchdog timeouts
		static uint32_t yield_counter = 0;
		if (++yield_counter >= 100) {
			yield_counter = 0;
			yield();
		}
	}
}

void loop()
{
	uint32_t cycles = cpu_cycle();
	espeon_update();
	lcd_cycle(cycles);
	timer_cycle(cycles);
}
