#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
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
	
	// **CRITICAL MEMORY ALLOCATION STRATEGY**
	// Allocate large memory blocks EARLY before fragmentation occurs
	Serial.println("=== EARLY MEMORY ALLOCATION ===");
	size_t initial_heap = ESP.getFreeHeap();
	Serial.printf("Initial free heap: %d bytes\n", initial_heap);
	
	// Pre-allocate Game Boy main memory (64KB) - most critical allocation
	Serial.println("Pre-allocating Game Boy main memory (64KB)...");
	uint8_t* early_main_mem = (uint8_t*)calloc(1, 0x10000);
	if (early_main_mem) {
		Serial.printf("SUCCESS: Pre-allocated 64KB main memory at %p\n", early_main_mem);
		espeon_set_preallocated_main_mem(early_main_mem);
	} else {
		Serial.println("CRITICAL: Failed to pre-allocate main memory!");
		Serial.printf("Available heap: %d, largest block: %d\n", 
		              ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		// Continue anyway, but this will likely fail later
	}
	
	// Pre-allocate MBC RAM (32KB max for largest cartridges)
	Serial.println("Pre-allocating MBC RAM (32KB)...");
	size_t mbc_ram_needed = 32*1024; 
	uint8_t* early_mbc_ram = (uint8_t*)calloc(1, mbc_ram_needed);
	if (early_mbc_ram) {
		Serial.printf("SUCCESS: Pre-allocated %d bytes for MBC RAM at %p\n", mbc_ram_needed, early_mbc_ram);
		espeon_set_preallocated_mbc_ram(early_mbc_ram, mbc_ram_needed);
	} else {
		Serial.println("WARNING: Could not pre-allocate MBC RAM");
		Serial.printf("Available heap: %d, largest block: %d\n", 
		              ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
	}
	
	size_t post_alloc_heap = ESP.getFreeHeap();
	Serial.printf("Heap after early allocation: %d bytes (used %d bytes)\n", 
	              post_alloc_heap, initial_heap - post_alloc_heap);
	Serial.println("=== EARLY ALLOCATION COMPLETE ===");
	
	menu_init();
	menu_loop();
	
	// Give extra time for SPI bus to settle after menu operations
	Serial.println("Preparing SPI bus for ROM loading...");
	tft.endWrite();  // Ensure all TFT operations are complete
	
	// Gentle SPI coordination (no aggressive reset to avoid callback duplication)
	delay(50);       // Brief delay for SPI bus to settle
	
	Serial.println("SPI bus ready for ROM loading");
	
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
	// In streaming mode, we need to pass bank 0 for ROM header parsing
	const uint8_t* rom_data_for_init = rom ? rom : espeon_get_rom_bank(0);
	if (!rom_init(rom_data_for_init))
		espeon_faint("rom_init failed.");
	
	Serial.println("Initializing MMU...");
	if (!mmu_init(bootrom))
		espeon_faint("mmu_init failed.");
	Serial.println("MMU initialization complete!");
	
	Serial.println("Initializing LCD...");
	if (!lcd_init())
		espeon_faint("lcd_init failed.");
	Serial.println("LCD initialization complete!");
	
	Serial.println("Initializing CPU...");
	cpu_init();
	Serial.println("CPU initialization complete!");
	
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
	// Empty loop function - all main logic is in setup()
	delay(1000);
}
