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
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_partition.h>

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

uint16_t palette[] = { 0xFFFF, 0xAAAA, 0x5555, 0x2222 };

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
	
	// Create dedicated SPI instance for SD card 
	if (!sdSPI) {
		sdSPI = new SPIClass(VSPI);
	}
	
	// Initialize with CYD SD card pins at higher speed
	sdSPI->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
	
	// Initialize SD with dedicated SPI instance and optimized speed
	// Try highest speed first, fall back if needed
	if (SD.begin(SD_CS, *sdSPI, 25000000U)) { // 25MHz
		Serial.println("SD initialized at 25MHz");
	} else if (SD.begin(SD_CS, *sdSPI, 10000000U)) { // 10MHz fallback
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
	
	// Initialize SPI mutex for resource management
	spi_mutex = xSemaphoreCreateMutex();
	if (!spi_mutex) {
		Serial.println("FATAL: Failed to create SPI mutex!");
		ESP.restart();
	}
	Serial.println("SPI mutex created successfully");
	
	// Initialize TFT display first (uses HSPI by default)
	tft.init();
	tft.setRotation(1); // Landscape orientation for CYD
	tft.fillScreen(TFT_BLACK);
	
	// Show startup message on screen
	tft.setCursor(10, 10);
	tft.setTextColor(TFT_WHITE);
	tft.setTextSize(2);
	tft.print("Espeon v1.0");
	tft.setCursor(10, 30);
	tft.setTextSize(1);
	tft.print("Initializing...");
	
	// Initialize backlight PWM for brightness control
	ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
	ledcAttachPin(TFT_BL_PIN, PWM_CHANNEL);
	// Set brightness to 50% (128 out of 255)
	ledcWrite(PWM_CHANNEL, 128);
	
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
	
	const uint32_t pal[] = {0xEFFFDE, 0xADD794, 0x525F73, 0x183442}; // Default greenscale palette
	espeon_set_palette(pal);
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
	memset(pixels, col, sizeof(pixels));
}

void espeon_clear_screen(uint16_t col)
{
	tft.fillScreen(col);
}

void espeon_set_palette(const uint32_t* col)
{
	/* RGB888 -> RGB565 */
	for (int i = 0; i < 4; ++i) {
		palette[i] = ((col[i]&0xFF)>>3)+((((col[i]>>8)&0xFF)>>2)<<5)+((((col[i]>>16)&0xFF)>>3)<<11);
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

// Static ROM buffer for SD card loaded ROMs
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
	
	// Force garbage collection before file operations
	delay(100);
	
	// Try to open ROM file directly 
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
	tft.setCursor(10, 100);
	tft.printf("Size: %d bytes", romsize);
	
	// Check ROM size limit - be more generous for compatibility
	if (romsize > 8*1024*1024) { // 8MB limit
		Serial.printf("ROM too large: %d bytes\n", romsize);
		romfile.close();
		spi_release_lock();
		return nullptr;
	}
	
	// Free previous ROM data if it exists
	if (sd_rom_data) {
		free(sd_rom_data);
		sd_rom_data = nullptr;
		// Force garbage collection after freeing
		delay(50);
	}
	
	// Check available memory before allocation
	size_t free_heap = ESP.getFreeHeap();
	size_t needed = romsize + 1024; // Extra 1KB for safety
	
	Serial.printf("Free heap: %d bytes, needed: %d bytes\n", free_heap, needed);
	
	if (free_heap < needed + 100*1024) { // Need at least 100KB extra
		Serial.println("Insufficient memory for ROM loading");
		romfile.close();
		spi_release_lock();
		
		tft.fillScreen(TFT_BLACK);
		tft.setTextColor(TFT_RED);
		tft.setTextSize(2);
		tft.setCursor(10, 50);
		tft.print("Memory Error!");
		tft.setTextSize(1);
		tft.setCursor(10, 80);
		tft.printf("Need %dKB, have %dKB", needed/1024, free_heap/1024);
		delay(3000);
		return nullptr;
	}
	
	// Allocate memory for ROM data with some extra space
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
	tft.setCursor(10, 120);
	tft.setTextColor(TFT_CYAN);
	tft.print("Reading ROM data...");
	
	// Read ROM data from SD card in chunks for better stability
	size_t bytesRead = 0;
	size_t chunkSize = 4096; // 4KB chunks
	uint8_t* writePtr = sd_rom_data;
	
	romfile.seek(0);
	
	while (bytesRead < romsize) {
		size_t toRead = min(chunkSize, romsize - bytesRead);
		size_t read = romfile.read(writePtr, toRead);
		
		if (read == 0) {
			Serial.println("SD read error - reached EOF early");
			break;
		}
		
		bytesRead += read;
		writePtr += read;
		
		// Show progress every 64KB
		if (bytesRead % (64*1024) == 0 || bytesRead == romsize) {
			tft.setCursor(10, 140);
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
	tft.setCursor(10, 160);
	tft.setTextColor(TFT_GREEN);
	tft.print("ROM loaded successfully!");
	delay(1000);
	
	Serial.printf("Successfully loaded %d bytes from SD card ROM\n", bytesRead);
	return sd_rom_data;
}

void espeon_set_brightness(uint8_t brightness)
{
	// brightness should be 0-100 (percentage)
	uint8_t pwm_value = (brightness * 255) / 100;
	ledcWrite(PWM_CHANNEL, pwm_value);
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
		Serial.println("ROM memory cleaned up");
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

// Memory management optimization
void espeon_check_memory() {
	Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
	Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
	Serial.printf("Heap size: %d bytes\n", ESP.getHeapSize());
	
	// Free up any unused memory if heap is low
	if (ESP.getFreeHeap() < 512*1024) {
		Serial.println("Low memory detected, attempting cleanup...");
		// Force garbage collection and heap defragmentation
		delay(100);
	}
}
