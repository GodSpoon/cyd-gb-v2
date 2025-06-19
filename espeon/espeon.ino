#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include "timer.h"
#include "rom.h"
#include "mem.h"
#include "cpu.h"
#include "lcd.h"
#include "interrupt.h"
#include "espeon.h"
// DISABLED: #include "menu.h"  // Touch menu system disabled to avoid SPI conflicts

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
	
	// DISABLED: Touch-based menu system to avoid SPI conflicts
	// menu_init();
	// menu_loop();
	
	// Display loading message on screen
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.setCursor(10, 50);
	tft.print("Auto-Loading ROM");
	tft.setTextSize(1);
	tft.setCursor(10, 80);
	tft.print("Touch disabled - loading first ROM");
	
	// Show 3-second countdown
	for (int countdown = 3; countdown > 0; countdown--) {
		tft.fillRect(10, 100, 300, 20, TFT_BLACK); // Clear countdown area
		tft.setCursor(10, 100);
		tft.setTextColor(TFT_YELLOW);
		tft.setTextSize(2);
		tft.printf("Starting in %d...", countdown);
		delay(1000);
	}
	
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.setCursor(10, 50);
	tft.print("Loading ROM...");
	tft.setTextSize(1);
	tft.setCursor(10, 80);
	tft.print("Scanning SD card for .gb files");
	
	espeon_check_memory();
	
	Serial.println("Auto-loading first ROM from SD card...");
	const char* selected_rom_path = nullptr;
	
	// Automatically select the first available ROM from SD card
	if (espeon_get_rom_count() > 0) {
		const auto& rom_files = espeon_get_rom_files();
		selected_rom_path = rom_files[0].c_str();
		Serial.printf("Auto-selected first ROM: %s\n", selected_rom_path);
		
		// Show selected ROM on screen
		tft.fillRect(10, 100, 300, 40, TFT_BLACK); // Clear previous text
		tft.setCursor(10, 100);
		tft.setTextColor(TFT_GREEN);
		tft.print("Found ROM:");
		tft.setCursor(10, 120);
		// Extract just the filename for display
		String displayName = String(selected_rom_path);
		int lastSlash = displayName.lastIndexOf('/');
		if (lastSlash >= 0) {
			displayName = displayName.substring(lastSlash + 1);
		}
		tft.printf("%s", displayName.c_str());
	} else {
		Serial.println("No ROM files found on SD card");
		tft.fillRect(10, 100, 300, 40, TFT_BLACK); // Clear previous text
		tft.setCursor(10, 100);
		tft.setTextColor(TFT_YELLOW);
		tft.print("No .gb files found on SD");
		tft.setCursor(10, 120);
		tft.print("Trying internal ROM...");
	}
	
	// Give user time to see the selected ROM
	delay(1500);

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
		
		// Force VBlank interrupt occasionally to help break infinite loops, but let LCD timing control LY
		static uint32_t last_vblank_time = 0;
		static uint32_t frame_counter = 0;
		uint32_t current_time = millis();
		
		// Generate VBlank every ~16.7ms but don't interfere with natural LY progression
		uint32_t vblank_interval = 16 + (frame_counter % 4 == 0 ? 1 : 0);
		if (current_time - last_vblank_time >= vblank_interval) {
			// Only force VBlank interrupt, let lcd_cycle() handle LY updates naturally
			interrupt(0x01); // Force VBlank interrupt
			last_vblank_time = current_time;
			frame_counter++;
		}
		
		// Yield occasionally to prevent watchdog timeouts
		static uint32_t yield_counter = 0;
		if (++yield_counter >= 50) { // More frequent yielding
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
