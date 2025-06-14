#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <FS.h>
#include <Wire.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_partition.h>

#include "espeon.h"
#include "interrupt.h"
#include "mbc.h"
#include "rom.h"

// TFT_eSPI instance for CYD display
TFT_eSPI tft = TFT_eSPI();

#define PARTITION_ROM (esp_partition_subtype_t(0x40))
#define MAX_ROM_SIZE (2*1024*1024)

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

void espeon_init(void)
{
	// Initialize Serial communication first
	Serial.begin(115200);
	delay(1000); // Give time for serial to initialize
	Serial.println("Espeon v1.0 - CYD GameBoy Emulator");
	Serial.println("Initializing...");
	
	// Initialize TFT display
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
	
	// Initialize SD card with proper pins for CYD
	// CYD SD card pins: MISO=19, MOSI=23, SCK=18, CS=5
	Serial.println("Initializing SD card...");
	
	// Configure SPI for SD card (different from display SPI)
	SPIClass sdSPI(VSPI);
	sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
	
	if (!SD.begin(SD_CS, sdSPI, 4000000U)) {
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
		
		// List root directory to verify card is working
		File root = SD.open("/");
		if (root) {
			Serial.println("SD Card contents:");
			File entry = root.openNextFile();
			while (entry) {
				Serial.printf("  %s (%d bytes)\n", entry.name(), entry.size());
				entry = root.openNextFile();
			}
			root.close();
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
	
	File sram = SD.open(path, FILE_WRITE);
	if (sram) {
		sram.seek(0);
		size_t written = sram.write(ram, size);
		sram.close();
		Serial.printf("SRAM saved: %d bytes written\n", written);
	} else {
		Serial.printf("Failed to open SRAM file for writing: %s\n", path);
	}
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
	
	if (!SD.exists(path)) {
		Serial.printf("SRAM file does not exist: %s\n", path);
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
}

const uint8_t* espeon_load_bootrom(const char* path)
{
	static uint8_t bootrom[256];
	
	Serial.printf("Attempting to load bootrom from: %s\n", path);
	
	if (!SD.exists(path)) {
		Serial.printf("Bootrom file does not exist: %s\n", path);
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
		return bootrom;
	} else {
		Serial.printf("Failed to open bootrom file: %s\n", path);
	}
	
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

const uint8_t* espeon_load_rom(const char* path)
{
	const esp_partition_t* part;
	part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PARTITION_ROM, NULL);
	if (!part) {
		Serial.println("ROM partition not found");
		return nullptr;
	}
	
	if (!path) {
		Serial.println("Loading last ROM from flash");
		return espeon_get_last_rom(part);
	}
	
	Serial.printf("Attempting to load ROM from: %s\n", path);
	
	if (!SD.exists(path)) {
		Serial.printf("ROM file does not exist: %s\n", path);
		return nullptr;
	}
	
	File romfile = SD.open(path, FILE_READ);
	if (!romfile) {
		Serial.printf("Failed to open ROM file: %s\n", path);
		return nullptr;
	}
	
	size_t romsize = romfile.size();
	Serial.printf("ROM file size: %d bytes\n", romsize);
	
	if (romsize > MAX_ROM_SIZE) {
		Serial.printf("ROM too large: %d > %d\n", romsize, MAX_ROM_SIZE);
		romfile.close();
		return nullptr;
	}
	
	esp_err_t err;
	err = esp_partition_erase_range(part, 0, MAX_ROM_SIZE);
	if (err != ESP_OK) {
		Serial.printf("Failed to erase partition: 0x%x\n", err);
		romfile.close();
		return nullptr;
	}
	
	const size_t bufsize = 32 * 1024;
	uint8_t* rombuf = (uint8_t*)calloc(bufsize, 1);
	if (!rombuf) {
		Serial.println("Failed to allocate ROM buffer");
		romfile.close();
		return nullptr;
	}
	
	tft.fillScreen(TFT_BLACK);
	tft.setTextColor(TFT_WHITE);
	tft.drawString("Flashing ROM...", 0, 0);
	size_t offset = 0;
	while(romfile.available()) {
		romfile.read(rombuf, bufsize);
		esp_partition_write(part, offset, (const void*)rombuf, bufsize);
		offset += bufsize;
		// Simple progress indicator - TFT_eSPI doesn't have progressBar
		int progress = (offset*100)/romsize;
		tft.fillRect(50, 100, (progress * 200) / 100, 10, TFT_GREEN);
	}
	tft.fillScreen(TFT_BLACK);
	free(rombuf);
	romfile.close();
	
	return espeon_get_last_rom(part);
}

void espeon_set_brightness(uint8_t brightness)
{
	// brightness should be 0-100 (percentage)
	uint8_t pwm_value = (brightness * 255) / 100;
	ledcWrite(PWM_CHANNEL, pwm_value);
}
