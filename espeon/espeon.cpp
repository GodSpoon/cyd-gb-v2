#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <FS.h>
#include <Wire.h>
#include <SPI.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>

#include "espeon.h"
#include "interrupt.h"
#include "mbc.h"
#include "rom.h"

// TFT_eSPI instance for CYD display
TFT_eSPI tft = TFT_eSPI();

// Static storage for ROM file list (populated during SD initialization)
static std::vector<String> availableRomFiles;

// SPI resource management
static SemaphoreHandle_t spi_mutex = nullptr;
static SPIClass* sdSPI = nullptr;
static bool spi_initialized = false;

#define PARTITION_ROM (esp_partition_subtype_t(0x40))
#define MAX_ROM_SIZE (8*1024*1024)
#define ROM_BANK_SIZE (16*1024)  // 16KB ROM banks for Game Boy
#define MAX_ROM_BANKS (4)        // Reduce to 4 banks (64KB total) for better memory efficiency

#define JOYPAD_INPUT 5
#define JOYPAD_ADDR  0x88

#define GETBIT(x, b) (((x)>>(b)) & 0x01)

#define GAMEBOY_WIDTH 160
#define GAMEBOY_HEIGHT 144

#define CENTER_X ((320 - GAMEBOY_WIDTH)  >> 1)
#define CENTER_Y ((240 - GAMEBOY_HEIGHT) >> 1)

// Backlight control for CYD
#define TFT_BL_PIN 21
#define PWM_CHANNEL 0
#define PWM_FREQ 5000
#define PWM_RESOLUTION 8

// SD Card pins for CYD
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18

static fbuffer_t* pixels;

volatile int spi_lock = 0;
volatile bool sram_modified = false;

uint16_t palette[] = { 0x0000, 0x5555, 0xAAAA, 0xFFFF };

void espeon_render_border(const uint8_t* img, uint32_t size)
{
	tft.fillScreen(TFT_BLACK);
	
	/* For now, just draw a simple border frame */
	/* TODO: Add JPEG support or convert border to bitmap format */
	tft.drawRect(0, 0, 320, 240, TFT_WHITE);
	tft.drawRect(1, 1, 318, 238, TFT_WHITE);
}

static void espeon_request_sd_write()
{
	spi_lock = 1;
}

// SPI resource management functions
static bool spi_acquire_lock(TickType_t timeout_ms = 1000) {
	if (!spi_mutex) {
		Serial.println("ERROR: SPI mutex not initialized!");
		return false;
	}
	
	if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
		return true;
	}
	
	Serial.println("ERROR: Failed to acquire SPI lock");
	return false;
}

static void spi_release_lock() {
	if (spi_mutex) {
		xSemaphoreGive(spi_mutex);
	}
}

static bool spi_init_sd_interface() {
	if (spi_initialized) {
		return true;
	}
	
	Serial.println("Initializing SD SPI interface...");
	
	// Create dedicated SPI instance for SD card if not already created
	if (!sdSPI) {
		sdSPI = new SPIClass(VSPI);
		if (!sdSPI) {
			Serial.println("ERROR: Failed to create SPI instance");
			return false;
		}
	}
	
	// Initialize with CYD SD card pins at optimized speed
	sdSPI->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
	
	// Initialize SD with dedicated SPI instance and progressive speed fallback
	// Try highest speed first, fall back if needed - optimized for 240MHz CPU
	if (SD.begin(SD_CS, *sdSPI, 40000000U)) { // 40MHz - aggressive for 240MHz CPU
		Serial.println("SD initialized at 40MHz (high performance)");
	} else if (SD.begin(SD_CS, *sdSPI, 25000000U)) { // 25MHz fallback
		Serial.println("SD initialized at 25MHz (performance)");
	} else if (SD.begin(SD_CS, *sdSPI, 10000000U)) { // 10MHz conservative
		Serial.println("SD initialized at 10MHz (fallback)");
	} else if (SD.begin(SD_CS, *sdSPI, 4000000U)) { // 4MHz last resort
		Serial.println("SD initialized at 4MHz (low speed)");
	} else {
		Serial.println("ERROR: SD card initialization failed at all speeds");
		return false;
	}
	
	spi_initialized = true;
	Serial.println("SD SPI interface initialized successfully");
	return true;
}

void espeon_init(void)
{
	// Initialize Serial communication first
	Serial.begin(115200);
	delay(1000); // Give time for serial to initialize
	Serial.println("Espeon v1.0 - CYD GameBoy Emulator");
	Serial.println("Initializing...");
	
	// Set CPU frequency to 240MHz for maximum performance
	setCpuFrequencyMhz(240);
	Serial.printf("CPU frequency set to: %d MHz\n", getCpuFrequencyMhz());
	Serial.printf("APB frequency: %d Hz\n", getApbFrequency());
	
	// Initialize SPI mutex for resource management
	spi_mutex = xSemaphoreCreateMutex();
	if (!spi_mutex) {
		Serial.println("FATAL: Failed to create SPI mutex!");
		ESP.restart();
	}
	Serial.println("SPI mutex created successfully");
	
	// Initialize backlight PWM BEFORE TFT to ensure PWM control
	ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
	ledcAttachPin(TFT_BL_PIN, PWM_CHANNEL);
	// Set brightness to 80% initially for better visibility
	ledcWrite(PWM_CHANNEL, 204); // 80% of 255
	
	// Initialize TFT display (uses HSPI by default)
	tft.init();
	tft.setRotation(1); // Landscape orientation for CYD
	tft.fillScreen(TFT_BLACK);
	
	// Re-establish PWM control after TFT init (TFT may override pin)
	ledcDetachPin(TFT_BL_PIN);
	ledcAttachPin(TFT_BL_PIN, PWM_CHANNEL);
	// Set brightness to 80% again
	espeon_set_brightness(80);
	
	// Show startup message on screen
	tft.setCursor(10, 10);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.print("Espeon v1.0");
	tft.setCursor(10, 30);
	tft.setTextSize(1);
	tft.print("Initializing...");
	
	// Initialize SD card with dedicated SPI interface
	Serial.println("Initializing SD card...");
	
	// Allow extra time for TFT SPI operations to complete
	delay(100);
	
	if (!spi_init_sd_interface()) {
		Serial.println("SD Card initialization failed!");
		tft.setCursor(10, 50);
		tft.setTextColor(TFT_RED);
		tft.print("SD Card Error!");
		tft.setCursor(10, 70);
		tft.print("Check connections");
		delay(2000);
	} else {
		Serial.println("SD Card initialized successfully");
		tft.setCursor(10, 50);
		tft.setTextColor(TFT_GREEN);
		tft.print("SD Card OK");
		
		// List root directory to verify card is working and collect ROM files
		if (spi_acquire_lock()) {
			File root = SD.open("/");
			if (root) {
				Serial.println("SD Card contents:");
				availableRomFiles.clear(); // Clear any previous ROM file list
				
				File entry = root.openNextFile();
				while (entry) {
					String fileName = entry.name();
					Serial.printf("  %s (%d bytes)\n", fileName.c_str(), entry.size());
					
					// Check if this is a .gb ROM file
					if (fileName.endsWith(".gb") || fileName.endsWith(".GB")) {
						String fullPath = "/" + fileName;
						availableRomFiles.push_back(fullPath);
						Serial.printf("    -> Added ROM: %s\n", fullPath.c_str());
					}
					
					entry = root.openNextFile();
				}
				root.close();
				
				Serial.printf("Found %d ROM files total\n", availableRomFiles.size());
			}
			spi_release_lock();
		}
	}
	
	// Initialize I2C for joypad communication
	Wire.begin();
	
	pinMode(JOYPAD_INPUT, INPUT_PULLUP);
	// Note: BUTTON_C_PIN and SPEAKER_PIN are M5Stack specific, 
	// CYD uses different GPIO pins that would need to be defined
	
	pixels = (fbuffer_t*)calloc(GAMEBOY_HEIGHT * GAMEBOY_WIDTH, sizeof(fbuffer_t));
	
	const uint32_t pal[] = {0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF}; // Game Boy palette: darkest to lightest (inverted for correct display)
	espeon_set_palette(pal);
	
	// Set final brightness after all initialization is complete
	Serial.println("Setting final brightness...");
	espeon_set_brightness(75); // Set to 75% brightness by default
}

void espeon_update(void)
{
	if(!((GPIO.in >> JOYPAD_INPUT) & 0x1)) {
		Wire.requestFrom(JOYPAD_ADDR, 1);
		if (Wire.available()) {
			uint8_t btns = Wire.read();
			btn_faces = (btns >> 4);
			btn_directions = (GETBIT(btns, 1) << 3) | (GETBIT(btns, 0) << 2) | (GETBIT(btns, 2) << 1) | (GETBIT(btns, 3));
			if (!btn_faces || !btn_directions)
				interrupt(INTR_JOYPAD);
		}
	}
}

void espeon_faint(const char* msg)
{
	tft.fillScreen(TFT_BLACK);
	tft.setCursor(2, 2);
	tft.setTextColor(TFT_WHITE);
	tft.printf("Espeon fainted!\n%s", msg);
	while(true);
}

fbuffer_t* espeon_get_framebuffer(void)
{
	return pixels;
}

void espeon_clear_framebuffer(fbuffer_t col)
{
	for (int i = 0; i < GAMEBOY_HEIGHT * GAMEBOY_WIDTH; i++) {
		pixels[i] = col;
	}
}

void espeon_clear_screen(uint16_t col)
{
	tft.fillScreen(col);
}

void espeon_set_palette(const uint32_t* col)
{
	/* RGB888 -> RGB565 conversion (proper order: R5G6B5) */
	for (int i = 0; i < 4; ++i) {
		uint8_t r = (col[i] >> 16) & 0xFF; // Red component
		uint8_t g = (col[i] >> 8) & 0xFF;  // Green component  
		uint8_t b = col[i] & 0xFF;         // Blue component
		
		// Convert to RGB565: RRRRR GGGGGG BBBBB
		palette[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
}

void espeon_end_frame(void)
{
	if (spi_lock) {
		const s_rominfo* rominfo = rom_get_info();
		if (rominfo->has_battery && rom_get_ram_size())
			espeon_save_sram(mbc_get_ram(), rom_get_ram_size());
		spi_lock = 0;
	}
	tft.pushImage(CENTER_X, CENTER_Y, GAMEBOY_WIDTH, GAMEBOY_HEIGHT, pixels);
}

void espeon_save_sram(uint8_t* ram, uint32_t size)
{
	if (!ram) {
		Serial.println("SRAM save: NULL RAM pointer");
		return;
	}
	
	static char path[20];
	sprintf(path, "/%.8s.bin", rom_get_title());
	
	Serial.printf("Saving SRAM to: %s (%d bytes)\n", path, size);
	
	// Acquire SPI lock before SD operations
	if (!spi_acquire_lock()) {
		Serial.println("Failed to acquire SPI lock for SRAM save");
		return;
	}
	
	// Ensure SD is in good state for writing
	if (!SD.exists("/")) {
		Serial.println("SD card not accessible, reinitializing...");
		SD.end();
		delay(50);
		if (!SD.begin(SD_CS, *sdSPI, 4000000U)) {
			Serial.println("Failed to reinitialize SD for SRAM save");
			spi_release_lock();
			return;
		}
	}
	
	File sram = SD.open(path, FILE_WRITE);
	if (sram) {
		sram.seek(0);
		size_t written = sram.write(ram, size);
		sram.close();
		Serial.printf("SRAM saved: %d bytes written\n", written);
	} else {
		Serial.printf("Failed to open SRAM file for writing: %s\n", path);
	}
	
	spi_release_lock();
}

void espeon_load_sram(uint8_t* ram, uint32_t size)
{
	if (!ram) {
		Serial.println("SRAM load: NULL RAM pointer");
		return;
	}
	
	static char path[20];
	sprintf(path, "/%.8s.bin", rom_get_title());
	
	Serial.printf("Loading SRAM from: %s\n", path);
	
	// Acquire SPI lock before SD operations
	if (!spi_acquire_lock()) {
		Serial.println("Failed to acquire SPI lock for SRAM load");
		return;
	}
	
	// Ensure SD is in good state for reading
	if (!SD.exists("/")) {
		Serial.println("SD card not accessible, reinitializing...");
		SD.end();
		delay(50);
		if (!SD.begin(SD_CS, *sdSPI, 4000000U)) {
			Serial.println("Failed to reinitialize SD for SRAM load");
			spi_release_lock();
			return;
		}
	}
	
	if (!SD.exists(path)) {
		Serial.printf("SRAM file does not exist: %s\n", path);
		spi_release_lock();
		return;
	}
	
	File sram = SD.open(path, FILE_READ);
	if (sram) {
		size_t fileSize = sram.size();
		Serial.printf("SRAM file size: %d bytes\n", fileSize);
		
		sram.seek(0);
		size_t bytesRead = sram.read(ram, size);
		sram.close();
		Serial.printf("SRAM loaded: %d bytes read\n", bytesRead);
	} else {
		Serial.printf("Failed to open SRAM file for reading: %s\n", path);
	}
	
	spi_release_lock();
}

const uint8_t* espeon_load_bootrom(const char* path)
{
	static uint8_t bootrom[256];
	
	Serial.printf("Attempting to load bootrom from: %s\n", path);
	
	// Acquire SPI lock before SD operations
	if (!spi_acquire_lock()) {
		Serial.println("Failed to acquire SPI lock for bootrom load");
		return nullptr;
	}
	
	// Ensure SD is in good state for reading
	if (!SD.exists("/")) {
		Serial.println("SD card not accessible, reinitializing...");
		SD.end();
		delay(50);
		if (!SD.begin(SD_CS, *sdSPI, 4000000U)) {
			Serial.println("Failed to reinitialize SD for bootrom load");
			spi_release_lock();
			return nullptr;
		}
	}
	
	if (!SD.exists(path)) {
		Serial.printf("Bootrom file does not exist: %s\n", path);
		spi_release_lock();
		return nullptr;
	}
	
	File bf = SD.open(path, FILE_READ);
	if (bf) {
		size_t fileSize = bf.size();
		Serial.printf("Bootrom file size: %d bytes\n", fileSize);
		
		if (fileSize > sizeof(bootrom)) {
			Serial.printf("Warning: Bootrom file too large (%d > %d), truncating\n", fileSize, sizeof(bootrom));
		}
		
		bf.seek(0);
		size_t bytesRead = bf.read(bootrom, sizeof(bootrom));
		bf.close();
		
		Serial.printf("Successfully loaded %d bytes from bootrom\n", bytesRead);
		spi_release_lock();
		return bootrom;
	} else {
		Serial.printf("Failed to open bootrom file: %s\n", path);
	}
	
	spi_release_lock();
	return nullptr;
}

static inline const uint8_t* espeon_get_last_rom(const esp_partition_t* part)
{
	spi_flash_mmap_handle_t hrom;
	const uint8_t* romdata;
	esp_err_t err;
	err = esp_partition_mmap(part, 0, MAX_ROM_SIZE, SPI_FLASH_MMAP_DATA, (const void**)&romdata, &hrom);
	if (err != ESP_OK)
		return nullptr;
	return romdata;
}

// ROM streaming system for memory-efficient loading
static uint8_t* rom_bank_cache[MAX_ROM_BANKS];
static uint16_t cached_bank_numbers[MAX_ROM_BANKS];
static uint8_t cache_lru_counter[MAX_ROM_BANKS];
static uint8_t cache_head = 0;
static bool rom_streaming_mode = false;
static File rom_stream_file;
static String current_rom_path;
static size_t total_rom_size = 0;
static uint16_t total_rom_banks = 0;

// Always keep bank 0 in memory for initialization
static uint8_t* rom_bank0_permanent = nullptr;

// Initialize ROM streaming system
static bool init_rom_streaming() {
	// Initialize cache arrays
	for (int i = 0; i < MAX_ROM_BANKS; i++) {
		rom_bank_cache[i] = nullptr;
		cached_bank_numbers[i] = 0xFFFF;  // Invalid bank number
		cache_lru_counter[i] = 0;
	}
	
	// Check available memory before pre-allocation
	size_t free_heap = ESP.getFreeHeap();
	size_t cache_memory_needed = MAX_ROM_BANKS * ROM_BANK_SIZE;
	Serial.printf("ROM Cache: Available heap: %d, need %d bytes for cache\n", free_heap, cache_memory_needed);
	
	// Only pre-allocate cache if we have plenty of memory
	if (free_heap > cache_memory_needed + 100*1024) {
		Serial.printf("Pre-allocating %d ROM bank cache slots (%d bytes each)\n", MAX_ROM_BANKS, ROM_BANK_SIZE);
		for (int i = 0; i < MAX_ROM_BANKS; i++) {
			rom_bank_cache[i] = (uint8_t*)malloc(ROM_BANK_SIZE);
			if (!rom_bank_cache[i]) {
				Serial.printf("WARNING: Failed to pre-allocate ROM bank cache slot %d\n", i);
				// Clean up any successful allocations
				for (int j = 0; j < i; j++) {
					if (rom_bank_cache[j]) {
						free(rom_bank_cache[j]);
						rom_bank_cache[j] = nullptr;
					}
				}
				break; // Don't fail completely, just use on-demand allocation
			}
			cached_bank_numbers[i] = 0xFFFF; // Mark as empty but allocated
		}
		Serial.printf("Successfully pre-allocated %d ROM bank cache slots\n", MAX_ROM_BANKS);
	} else {
		Serial.println("ROM Cache: Insufficient memory for pre-allocation, will use on-demand allocation");
	}
	
	cache_head = 0;
	return true; // Always succeed, cache pre-allocation is optional
}

// Cleanup ROM streaming system
static void cleanup_rom_streaming() {
	// Free all cached banks
	for (int i = 0; i < MAX_ROM_BANKS; i++) {
		if (rom_bank_cache[i]) {
			free(rom_bank_cache[i]);
			rom_bank_cache[i] = nullptr;
		}
		cached_bank_numbers[i] = 0xFFFF;
		cache_lru_counter[i] = 0;
	}
	
	// Free permanent bank 0
	if (rom_bank0_permanent) {
		free(rom_bank0_permanent);
		rom_bank0_permanent = nullptr;
	}
	
	// Close ROM file if open
	if (rom_stream_file) {
		rom_stream_file.close();
	}
	
	rom_streaming_mode = false;
	total_rom_size = 0;
	total_rom_banks = 0;
	current_rom_path = "";
	cache_head = 0;
}

// Get ROM bank with caching (streaming mode)
static const uint8_t* get_rom_bank_streaming(uint16_t bank_number) {
	// Bank 0 should always use the permanent allocation
	if (bank_number == 0) {
		return rom_bank0_permanent;
	}
	
	// Check if bank is already cached
	for (int i = 0; i < MAX_ROM_BANKS; i++) {
		if (cached_bank_numbers[i] == bank_number) {
			// Update LRU counter
			cache_lru_counter[i] = 255;
			for (int j = 0; j < MAX_ROM_BANKS; j++) {
				if (j != i && cache_lru_counter[j] > 0) {
					cache_lru_counter[j]--;
				}
			}
			return rom_bank_cache[i];
		}
	}
	
	// Bank not cached, need to load it
	// Find least recently used slot
	int lru_slot = 0;
	for (int i = 1; i < MAX_ROM_BANKS; i++) {
		if (cache_lru_counter[i] < cache_lru_counter[lru_slot]) {
			lru_slot = i;
		}
	}
	
	// Verify the slot has memory allocated or allocate it now
	if (!rom_bank_cache[lru_slot]) {
		Serial.printf("ROM bank cache slot %d not pre-allocated, allocating on-demand...\n", lru_slot);
		
		// Check available memory before allocation
		size_t free_heap = ESP.getFreeHeap();
		size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
		
		if (free_heap < ROM_BANK_SIZE + 20*1024 || largest_block < ROM_BANK_SIZE) {
			Serial.printf("ERROR: Insufficient memory for ROM bank allocation\n");
			Serial.printf("  Free heap: %d bytes, largest block: %d bytes, need: %d bytes\n", 
			              free_heap, largest_block, ROM_BANK_SIZE);
			
			// Try emergency cleanup and retry
			Serial.println("Attempting emergency memory cleanup...");
			espeon_check_memory();
			
			free_heap = ESP.getFreeHeap();
			largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
			Serial.printf("After cleanup - Free heap: %d bytes, largest block: %d bytes\n", 
			              free_heap, largest_block);
			
			if (largest_block < ROM_BANK_SIZE) {
				Serial.println("ERROR: Still insufficient memory after cleanup");
				return nullptr;
			}
		}
		
		rom_bank_cache[lru_slot] = (uint8_t*)malloc(ROM_BANK_SIZE);
		if (!rom_bank_cache[lru_slot]) {
			Serial.printf("ERROR: Failed to allocate ROM bank cache slot %d on-demand\n", lru_slot);
			return nullptr;
		}
		cached_bank_numbers[lru_slot] = 0xFFFF; // Mark as empty but allocated
	}
	
	// Load bank from SD card with simplified approach
	if (!spi_acquire_lock(1000)) {
		Serial.printf("ERROR: Failed to acquire SPI lock for ROM bank %d\n", bank_number);
		free(rom_bank_cache[lru_slot]);
		rom_bank_cache[lru_slot] = nullptr;
		return nullptr;
	}
	
	// Open ROM file for this read operation
	File romFile = SD.open(current_rom_path.c_str(), FILE_READ);
	if (!romFile) {
		Serial.printf("ERROR: Failed to open ROM file for bank %d: %s\n", bank_number, current_rom_path.c_str());
		spi_release_lock();
		free(rom_bank_cache[lru_slot]);
		rom_bank_cache[lru_slot] = nullptr;
		return nullptr;
	}
	
	// Seek to bank position and read
	size_t bank_offset = bank_number * ROM_BANK_SIZE;
	if (bank_offset >= total_rom_size) {
		Serial.printf("ERROR: ROM bank %d exceeds ROM size\n", bank_number);
		romFile.close();
		spi_release_lock();
		free(rom_bank_cache[lru_slot]);
		rom_bank_cache[lru_slot] = nullptr;
		return nullptr;
	}
	
	romFile.seek(bank_offset);
	size_t bytes_to_read = (ROM_BANK_SIZE < (total_rom_size - bank_offset)) ? ROM_BANK_SIZE : (total_rom_size - bank_offset);
	size_t bytes_read = romFile.read(rom_bank_cache[lru_slot], bytes_to_read);
	
	romFile.close(); // Always close the file after reading
	spi_release_lock();
	
	if (bytes_read != bytes_to_read) {
		Serial.printf("ERROR: ROM bank %d read error: got %d, expected %d bytes\n", bank_number, bytes_read, bytes_to_read);
		free(rom_bank_cache[lru_slot]);
		rom_bank_cache[lru_slot] = nullptr;
		return nullptr;
	}
	
	// Update cache info
	cached_bank_numbers[lru_slot] = bank_number;
	cache_lru_counter[lru_slot] = 255;
	
	// Age other cache entries
	for (int i = 0; i < MAX_ROM_BANKS; i++) {
		if (i != lru_slot && cache_lru_counter[i] > 0) {
			cache_lru_counter[i]--;
		}
	}
	
	Serial.printf("Loaded ROM bank %d into cache slot %d\n", bank_number, lru_slot);
	return rom_bank_cache[lru_slot];
}

// Static ROM buffer for SD card loaded ROMs (legacy mode for small ROMs)
static uint8_t* sd_rom_data = nullptr;

const uint8_t* espeon_load_rom(const char* path)
{
	// If no path specified, try to load from flash partition
	if (!path) {
		Serial.println("Loading last ROM from flash partition");
		const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PARTITION_ROM, NULL);
		if (!part) {
			Serial.println("ROM partition not found");
			return nullptr;
		}
		return espeon_get_last_rom(part);
	}
	
	Serial.printf("Attempting to load ROM from SD card: %s\n", path);
	
	// Check memory before starting
	espeon_check_memory();
	
	// Simple loading screen
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.setCursor(10, 50);
	tft.print("Loading ROM...");
	tft.setTextSize(1);
	tft.setCursor(10, 80);
	tft.printf("File: %s", path);
	
	// Acquire SPI lock with extended timeout for ROM loading
	if (!spi_acquire_lock(5000)) {
		Serial.println("CRITICAL: Failed to acquire SPI lock for ROM loading!");
		tft.fillScreen(TFT_BLACK);
		tft.setTextColor(TFT_RED);
		tft.setTextSize(2);
		tft.setCursor(10, 50);
		tft.print("SPI Lock Error!");
		delay(3000);
		return nullptr;
	}
	
	// Minimal SD health check and potential reinitialization
	tft.setCursor(10, 100);
	tft.setTextColor(TFT_CYAN);
	tft.print("Checking SD card...");
	
	// Try to open root directory as a health check
	File root = SD.open("/");
	if (!root) {
		Serial.println("SD card health check failed, attempting reinitialization...");
		tft.setCursor(10, 120);
		tft.setTextColor(TFT_YELLOW);
		tft.print("Reinitializing SD...");
		
		// Simple reinitialization
		SD.end();
		delay(100);
		if (!SD.begin(SD_CS, *sdSPI, 4000000U)) {
			Serial.println("CRITICAL: SD card reinitialization failed!");
			spi_release_lock();
			
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("SD Error!");
			delay(3000);
			return nullptr;
		}
		Serial.println("SD card reinitialized successfully");
	} else {
		root.close();
		Serial.println("SD card health check passed");
	}
	
	// Now try to open ROM file
	tft.setCursor(10, 140);
	tft.setTextColor(TFT_GREEN);
	tft.print("Opening ROM file...");
	
	File romfile = SD.open(path, FILE_READ);
	if (!romfile) {
		Serial.printf("Failed to open ROM file: %s\n", path);
		spi_release_lock();
		
		tft.fillScreen(TFT_BLACK);
		tft.setTextColor(TFT_RED);
		tft.setTextSize(2);
		tft.setCursor(10, 50);
		tft.print("ROM Load Error!");
		tft.setTextSize(1);
		tft.setCursor(10, 80);
		tft.printf("Could not open: %s", path);
		delay(3000);
		return nullptr;
	}
	
	size_t romsize = romfile.size();
	Serial.printf("ROM file size: %d bytes\n", romsize);
	
	// Update display with size info
	tft.setCursor(10, 160);
	tft.setTextColor(TFT_GREEN);
	tft.printf("Size: %d bytes", romsize);
	
	// Check ROM size limit
	if (romsize > 8*1024*1024) { // 8MB limit
		Serial.printf("ROM too large: %d bytes\n", romsize);
		romfile.close();
		spi_release_lock();
		return nullptr;
	}
	
	// Clean up any existing ROM data
	if (sd_rom_data) {
		free(sd_rom_data);
		sd_rom_data = nullptr;
	}
	cleanup_rom_streaming();
	
	// Check if we should use streaming mode (for ROMs > 200KB)
	size_t free_heap = ESP.getFreeHeap();
	size_t memory_threshold = 200*1024; // 200KB threshold
	
	if (romsize > memory_threshold || free_heap < romsize + 100*1024) {
		Serial.printf("Using ROM streaming mode for %d byte ROM (free heap: %d)\n", romsize, free_heap);
		
		// Initialize streaming mode
		if (!init_rom_streaming()) {
			Serial.println("Failed to initialize ROM streaming (pre-allocation failed)");
			romfile.close();
			spi_release_lock();
			
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("Cache Alloc Error!");
			delay(3000);
			return nullptr;
		}
		rom_streaming_mode = true;
		current_rom_path = String(path);
		total_rom_size = romsize;
		total_rom_banks = (romsize + ROM_BANK_SIZE - 1) / ROM_BANK_SIZE;
		
		// Always load bank 0 permanently for initialization
		rom_bank0_permanent = (uint8_t*)malloc(ROM_BANK_SIZE);
		if (!rom_bank0_permanent) {
			Serial.println("Failed to allocate permanent bank 0");
			romfile.close();
			spi_release_lock();
			cleanup_rom_streaming();
			
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("Bank 0 Alloc Error!");
			delay(3000);
			return nullptr;
		}
		
		// Read bank 0 data directly from the already open file
		romfile.seek(0);
		size_t bank0_size = (ROM_BANK_SIZE < romsize) ? ROM_BANK_SIZE : romsize;
		size_t bytes_read = romfile.read(rom_bank0_permanent, bank0_size);
		romfile.close(); // Close the file, streaming will reopen as needed
		
		if (bytes_read != bank0_size) {
			Serial.printf("Failed to read bank 0: got %d, expected %d bytes\n", bytes_read, bank0_size);
			spi_release_lock();
			cleanup_rom_streaming();
			
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("Bank 0 Read Error!");
			delay(3000);
			return nullptr;
		}
		
		// Verify ROM bank 0 data was read correctly
		Serial.printf("ROM: Bank 0 loaded, first bytes: %02X %02X %02X %02X\n",
		              rom_bank0_permanent[0], rom_bank0_permanent[1], 
		              rom_bank0_permanent[2], rom_bank0_permanent[3]);
		Serial.printf("ROM: Bank 0 address 0x0038: %02X (should NOT be 0xFF)\n", rom_bank0_permanent[0x0038]);
		Serial.printf("ROM: Nintendo logo check (0x0104): %02X %02X %02X %02X\n",
		              rom_bank0_permanent[0x0104], rom_bank0_permanent[0x0105], 
		              rom_bank0_permanent[0x0106], rom_bank0_permanent[0x0107]);
		
		// Extended debug: Check interrupt vector area
		Serial.println("ROM: Interrupt vector area analysis:");
		Serial.printf("  RST 00 (0x00): %02X, RST 08 (0x08): %02X, RST 10 (0x10): %02X, RST 18 (0x18): %02X\n",
		              rom_bank0_permanent[0x00], rom_bank0_permanent[0x08], 
		              rom_bank0_permanent[0x10], rom_bank0_permanent[0x18]);
		Serial.printf("  RST 20 (0x20): %02X, RST 28 (0x28): %02X, RST 30 (0x30): %02X, RST 38 (0x38): %02X\n",
		              rom_bank0_permanent[0x20], rom_bank0_permanent[0x28], 
		              rom_bank0_permanent[0x30], rom_bank0_permanent[0x38]);
		Serial.printf("  VBlank (0x40): %02X, LCDC (0x48): %02X, Timer (0x50): %02X, Serial (0x58): %02X\n",
		              rom_bank0_permanent[0x40], rom_bank0_permanent[0x48], 
		              rom_bank0_permanent[0x50], rom_bank0_permanent[0x58]);
		Serial.printf("  Joypad (0x60): %02X\n", rom_bank0_permanent[0x60]);
		
		// Check if this ROM has 0xFF padding in interrupt vector area
		bool has_ff_padding = true;
		for (int i = 0; i < 0x100; i += 8) {
			if (rom_bank0_permanent[i] != 0xFF) {
				has_ff_padding = false;
				break;
			}
		}
		
		if (has_ff_padding) {
			Serial.println("ROM: This ROM has 0xFF padding in interrupt vector area (normal for many ROMs)");
			Serial.println("ROM: Bootrom disable will use selective copying to preserve safety vectors");
		} else {
			Serial.println("ROM: This ROM has valid interrupt vectors in ROM bank 0");
		}
		
		// Critical safety check
		if (rom_bank0_permanent[0x0038] == 0xFF) {
			Serial.println("WARNING: ROM bank 0 has 0xFF at address 0x0038!");
			Serial.println("WARNING: This may cause infinite RST 38 loop during emulation!");
			Serial.println("WARNING: Patching ROM bank 0 with NOP at 0x0038 to prevent infinite loop");
			rom_bank0_permanent[0x0038] = 0x00; // Patch with NOP to prevent infinite loop
		}
		
		spi_release_lock();
		
		// Show success message
		tft.setCursor(10, 200);
		tft.setTextColor(TFT_GREEN);
		tft.print("ROM streaming ready!");
		delay(1000);
		
		Serial.printf("ROM streaming initialized: %d banks, bank 0 loaded permanently\n", total_rom_banks);
		return rom_bank0_permanent; // Return pointer to bank 0
		
	} else {
		Serial.printf("Using legacy mode for %d byte ROM (free heap: %d)\n", romsize, free_heap);
		
		// Use legacy full-ROM-in-memory mode for small ROMs
		size_t needed = romsize + 1024; // Extra 1KB for safety
		
		// Allocate memory for ROM data
		sd_rom_data = (uint8_t*)malloc(needed);
		if (!sd_rom_data) {
			Serial.println("Failed to allocate ROM memory");
			romfile.close();
			spi_release_lock();
			
			tft.fillScreen(TFT_BLACK);
			tft.setTextColor(TFT_RED);
			tft.setTextSize(2);
			tft.setCursor(10, 50);
			tft.print("Alloc Failed!");
			delay(3000);
			return nullptr;
		}
		
		Serial.printf("Successfully allocated %d bytes for ROM\n", needed);
		
		// Update display
		tft.setCursor(10, 180);
		tft.setTextColor(TFT_CYAN);
		tft.print("Reading ROM data...");
		
		// Read ROM data from SD card in chunks
		size_t bytesRead = 0;
		size_t chunkSize = 4096; // 4KB chunks
		uint8_t* writePtr = sd_rom_data;
		
		romfile.seek(0);
		
		while (bytesRead < romsize) {
			size_t toRead = (chunkSize < (romsize - bytesRead)) ? chunkSize : (romsize - bytesRead);
			size_t read = romfile.read(writePtr, toRead);
			
			if (read == 0) {
				Serial.println("SD read error - reached EOF early");
				break;
			}
			
			bytesRead += read;
			writePtr += read;
			
			// Show progress every 32KB
			if (bytesRead % (32*1024) == 0 || bytesRead == romsize) {
				tft.setCursor(10, 200);
				tft.setTextColor(TFT_YELLOW);
				tft.printf("Read: %d/%d KB", bytesRead/1024, romsize/1024);
			}
			
			// Yield occasionally to avoid watchdog
			if (bytesRead % (16*1024) == 0) {
				yield();
			}
		}
		
		romfile.close();
		spi_release_lock();
		
		if (bytesRead != romsize) {
			Serial.printf("Warning: Only read %d of %d bytes\n", bytesRead, romsize);
			// Still proceed as partial ROMs might work for testing
		}
		
		// Show success message
		tft.setCursor(10, 220);
		tft.setTextColor(TFT_GREEN);
		tft.print("ROM loaded successfully!");
		delay(1000);
		
		Serial.printf("Successfully loaded %d bytes in legacy mode\n", bytesRead);
		return sd_rom_data;
	}
}

void espeon_set_brightness(uint8_t brightness)
{
	// Clamp brightness to 0-100 range
	if (brightness > 100) brightness = 100;
	
	// brightness should be 0-100 (percentage)
	uint8_t pwm_value = (brightness * 255) / 100;
	
	// Debug output
	Serial.printf("Setting brightness: %d%% -> PWM value: %d\n", brightness, pwm_value);
	
	// Re-establish PWM control (in case something interfered)
	ledcDetachPin(TFT_BL_PIN);
	ledcAttachPin(TFT_BL_PIN, PWM_CHANNEL);
	
	// Write the PWM value
	ledcWrite(PWM_CHANNEL, pwm_value);
	
	// Additional verification - force pin mode
	pinMode(TFT_BL_PIN, OUTPUT);
	ledcAttachPin(TFT_BL_PIN, PWM_CHANNEL);
}

// Alternative brightness function for troubleshooting (digital on/off)
void espeon_set_brightness_digital(bool on)
{
	Serial.printf("Setting brightness digital: %s\n", on ? "ON" : "OFF");
	
	// Detach PWM first
	ledcDetachPin(TFT_BL_PIN);
	
	// Set as digital output
	pinMode(TFT_BL_PIN, OUTPUT);
	digitalWrite(TFT_BL_PIN, on ? HIGH : LOW);
}

// Get the list of available ROM files (populated during SD initialization)
const std::vector<String>& espeon_get_rom_files()
{
	return availableRomFiles;
}

// Get the number of available ROM files
int espeon_get_rom_count()
{
	return availableRomFiles.size();
}

void espeon_cleanup_rom()
{
	if (sd_rom_data) {
		free(sd_rom_data);
		sd_rom_data = nullptr;
		Serial.println("Legacy ROM memory cleaned up");
	}
	
	if (rom_streaming_mode) {
		cleanup_rom_streaming();
		Serial.println("ROM streaming cleaned up");
	}
}

// Cleanup all SPI resources
void espeon_cleanup_spi()
{
	if (spi_mutex) {
		vSemaphoreDelete(spi_mutex);
		spi_mutex = nullptr;
	}
	
	if (sdSPI) {
		sdSPI->end();
		delete sdSPI;
		sdSPI = nullptr;
	}
	
	spi_initialized = false;
	Serial.println("SPI resources cleaned up");
}

// Memory management optimization with actual cleanup
void espeon_check_memory() {
	size_t free_heap = ESP.getFreeHeap();
	size_t min_free = ESP.getMinFreeHeap();
	size_t heap_size = ESP.getHeapSize();
	
	Serial.printf("Free heap: %d bytes\n", free_heap);
	Serial.printf("Min free heap: %d bytes\n", min_free);
	Serial.printf("Heap size: %d bytes\n", heap_size);
	
	// Attempt memory cleanup if heap is low
	if (free_heap < 200*1024) {  // Less than 200KB available (more aggressive threshold)
		Serial.println("Low memory detected, performing cleanup...");
		
		// Clear any cached ROM banks if in streaming mode (more aggressive)
		if (rom_streaming_mode) {
			int banks_freed = 0;
			for (int i = 0; i < MAX_ROM_BANKS; i++) {
				// Keep only bank 0 and most recently used bank
				if (rom_bank_cache[i] && cached_bank_numbers[i] > 0 && cache_lru_counter[i] < 200) {
					Serial.printf("Clearing cached ROM bank %d (LRU: %d)\n", cached_bank_numbers[i], cache_lru_counter[i]);
					free(rom_bank_cache[i]);
					rom_bank_cache[i] = nullptr;
					cached_bank_numbers[i] = 0xFFFF;
					cache_lru_counter[i] = 0;
					banks_freed++;
				}
			}
			Serial.printf("Freed %d ROM bank cache slots\n", banks_freed);
		}
		
		// Force heap compaction with more aggressive approach
		heap_caps_check_integrity_all(true);
		delay(100); // Give more time for cleanup
		
		Serial.printf("After cleanup - Free heap: %d bytes\n", ESP.getFreeHeap());
	}
}

// Get ROM bank data (supports both legacy and streaming modes)
const uint8_t* espeon_get_rom_bank(uint16_t bank_number) {
	// Special case: Bank 0 is always available
	if (bank_number == 0) {
		Serial.printf("DEBUG: espeon_get_rom_bank(0) called - rom_streaming_mode=%d, rom_bank0_permanent=%p, sd_rom_data=%p\n", 
		              rom_streaming_mode, rom_bank0_permanent, sd_rom_data);
		
		if (rom_streaming_mode && rom_bank0_permanent) {
			Serial.printf("DEBUG: Returning rom_bank0_permanent: %p\n", rom_bank0_permanent);
			// Verify the data looks reasonable
			Serial.printf("DEBUG: Bank 0 first bytes: %02X %02X %02X %02X\n",
			              rom_bank0_permanent[0], rom_bank0_permanent[1], 
			              rom_bank0_permanent[2], rom_bank0_permanent[3]);
			return rom_bank0_permanent;
		} else if (sd_rom_data) {
			Serial.printf("DEBUG: Returning sd_rom_data: %p\n", sd_rom_data);
			// Verify the data looks reasonable
			Serial.printf("DEBUG: Bank 0 first bytes: %02X %02X %02X %02X\n",
			              sd_rom_data[0], sd_rom_data[1], sd_rom_data[2], sd_rom_data[3]);
			return sd_rom_data; // In legacy mode, bank 0 is at the start
		} else {
			Serial.println("ERROR: No ROM loaded for bank 0 request");
			Serial.printf("ERROR: rom_streaming_mode=%d, rom_bank0_permanent=%p, sd_rom_data=%p\n", 
			              rom_streaming_mode, rom_bank0_permanent, sd_rom_data);
			return nullptr;
		}
	}
	
	if (rom_streaming_mode) {
		// Use streaming mode for banks > 0
		return get_rom_bank_streaming(bank_number);
	} else if (sd_rom_data) {
		// Use legacy mode - calculate offset in the full ROM data
		size_t bank_offset = bank_number * ROM_BANK_SIZE;
		// Note: In legacy mode we have the full ROM in memory, so just return offset
		return sd_rom_data + bank_offset;
	} else {
		// No ROM loaded
		Serial.printf("ERROR: No ROM loaded for bank %d request\n", bank_number);
		return nullptr;
	}
}

// Pre-allocated main memory management  
static uint8_t* preallocated_main_mem = nullptr;

void espeon_set_preallocated_main_mem(uint8_t* mem) {
	preallocated_main_mem = mem;
	Serial.printf("Set pre-allocated main memory: %p\n", mem);
}

uint8_t* espeon_get_preallocated_main_mem() {
	if (preallocated_main_mem) {
		uint8_t* mem = preallocated_main_mem;
		preallocated_main_mem = nullptr; // Transfer ownership
		return mem;
	}
	return nullptr;
}

// Pre-allocated MBC RAM management
static uint8_t* preallocated_mbc_ram = nullptr;
static size_t preallocated_mbc_size = 0;

void espeon_set_preallocated_mbc_ram(uint8_t* ram, size_t size) {
	preallocated_mbc_ram = ram;
	preallocated_mbc_size = size;
	Serial.printf("Set pre-allocated MBC RAM: %p, size: %d\n", ram, size);
}

uint8_t* espeon_get_preallocated_mbc_ram(size_t* size) {
	if (preallocated_mbc_ram && size) {
		*size = preallocated_mbc_size;
		uint8_t* ram = preallocated_mbc_ram;
		preallocated_mbc_ram = nullptr; // Transfer ownership
		preallocated_mbc_size = 0;
		return ram;
	}
	return nullptr;
}
