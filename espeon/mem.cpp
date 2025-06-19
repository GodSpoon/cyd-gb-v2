#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <esp_heap_caps.h>
#include "mem.h"
#include "rom.h"
#include "lcd.h"
#include "mbc.h"
#include "interrupt.h"
#include "timer.h"
#include "cpu.h"
#include "espeon.h"

bool usebootrom = false;
uint8_t *mem = nullptr;
static uint8_t *echo;
static uint32_t DMA_pending;
static uint8_t joypad_select_buttons, joypad_select_directions;
uint8_t btn_directions, btn_faces;
static const s_rominfo *rominfo;
static const uint8_t *rom;


uint8_t mem_get_byte(uint16_t i)
{
	if(DMA_pending && i < 0xFF80)
	{
		uint32_t elapsed = cpu_get_cycles() - DMA_pending;
		if(elapsed >= 160) {
			DMA_pending = 0;
		} else {
			return mem[0xFE00+elapsed];
		}
	}

	if(i >= 0x4000 && i < 0x8000) {
		if (!rombank) {
			static bool error_logged = false;
			if (!error_logged) {
				Serial.printf("ERROR: ROM bank access failed - rombank is NULL at address 0x%04X\n", i);
				Serial.println("ERROR: This indicates ROM bank allocation failure");
				Serial.println("WARNING: Returning 0xFF for all ROM reads - game may not work correctly");
				error_logged = true; // Only log once to avoid spam
			}
			return 0xFF; // Return safe value to prevent crash
		}
		return rombank[i - 0x4000];
	}

	else if (i >= 0xA000 && i < 0xC000)
		return mbc_read_ram(i);
	
	else if (i >= 0xE000 && i < 0xFE00)
		return echo[i];

	else switch(i)
	{
		case 0xFF00: {	/* Joypad */
			uint8_t mask = 0;
			if(!joypad_select_buttons)
				mask = btn_faces;
			if(!joypad_select_directions)
				mask = btn_directions;
			return (0xC0) | (joypad_select_buttons | joypad_select_directions) | (mask);
		}
		case 0xFF04: return timer_get_div();
		case 0xFF0F: return 0xE0 | IF;
		case 0xFF41: return lcd_get_stat();
		case 0xFF44: return lcd_get_line();
		case 0xFF4D: return 0xFF; /* GBC speed switch */
		case 0xFFFF: return IE;
	}

	return mem[i];
}

void mem_write_byte(uint16_t d, uint8_t i)
{
	/* ROM */
	if (d < 0x8000)
		mbc_write_rom(d, i);
	
	/* SRAM */
	else if (d >= 0xA000 && d < 0xC000)
		mbc_write_ram(d, i);
	
	/* ECHO */
	else if (d >= 0xE000 && d < 0xFE00)
		echo[d] = i;

	else switch(d)
	{
		case 0xFF00:	/* Joypad */
			joypad_select_buttons = i&0x20;
			joypad_select_directions = i&0x10;
		break;
		case 0xFF04: timer_reset_div(); break;
		case 0xFF07: timer_set_tac(i); break;
		case 0xFF0F: IF = i; break;
		case 0xFF40: lcd_write_control(i); break;
		case 0xFF41: lcd_write_stat(i); break;
		case 0xFF42: lcd_write_scroll_y(i); break;
		case 0xFF43: lcd_write_scroll_x(i); break;
		case 0xFF44: /* LY register is read-only, ignore writes */ break;
		case 0xFF45: lcd_set_ly_compare(i); break;
		case 0xFF46: { /* OAM DMA */
			/* Check if address overlaps with RAM or ROM */
			uint16_t addr = i * 0x100;
			const uint8_t* src = mem;
			if (addr >= 0x4000 && addr < 0x8000) {
				src = rombank;
				addr -= 0x4000;
			}
			else if (addr >= 0xA000 && addr < 0xC000) {
				src = rambank;
				addr -= 0xA000;
			}
			
			/* Copy 0xA0 bytes from source to OAM */
			memcpy(&mem[0xFE00], &src[addr], 0xA0);
			DMA_pending = cpu_get_cycles();
			break;
		}
		case 0xFF47: lcd_write_bg_palette(i); break;
		case 0xFF48: lcd_write_spr_palette1(i); break;
		case 0xFF49: lcd_write_spr_palette2(i); break;
		case 0xFF4A: lcd_set_window_y(i); break;
		case 0xFF4B: lcd_set_window_x(i); break;
		case 0xFF50: { /* Lock bootROM */
			Serial.println("MMU: Bootrom disable requested via 0xFF50");
			const uint8_t* rom_bank0 = espeon_get_rom_bank(0);
			if (rom_bank0) {
				// CRITICAL FIX: Many Game Boy ROMs have 0xFF padding in the first 256 bytes
				// because that area is normally handled by boot ROM. We need to be selective
				// about what we copy to avoid overwriting critical interrupt vectors with 0xFF.
				
				// Check if ROM has valid interrupt vectors (non-0xFF values)
				bool rom_has_valid_vectors = false;
				for (int i = 0; i < 0x100; i += 8) { // Check RST vectors (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38)
					if (rom_bank0[i] != 0xFF) {
						rom_has_valid_vectors = true;
						break;
					}
				}
				
				if (rom_has_valid_vectors) {
					// ROM has valid vectors, safe to copy the whole first 256 bytes
					Serial.println("MMU: ROM has valid interrupt vectors, copying full 0x0000-0x00FF");
					memcpy(&mem[0x0000], &rom_bank0[0x0000], 0x100);
				} else {
					// ROM has 0xFF padding in vector area, only copy safe areas
					Serial.println("MMU: ROM has 0xFF padding in vector area, selective copy to preserve safety");
					
					// Keep our safe NOP vectors at interrupt addresses, but copy other areas
					// Copy areas that are not critical interrupt vectors
					for (int addr = 0x00; addr < 0x100; addr++) {
						// Skip RST vectors (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38) 
						// and interrupt vectors (0x40, 0x48, 0x50, 0x58, 0x60)
						if (addr == 0x00 || addr == 0x08 || addr == 0x10 || addr == 0x18 ||
						    addr == 0x20 || addr == 0x28 || addr == 0x30 || addr == 0x38 ||
						    addr == 0x40 || addr == 0x48 || addr == 0x50 || addr == 0x58 || addr == 0x60) {
							// Keep our safe NOP at these critical addresses
							continue;
						}
						
						// For other addresses, copy from ROM if not 0xFF
						if (rom_bank0[addr] != 0xFF) {
							mem[addr] = rom_bank0[addr];
						}
					}
				}
				
				usebootrom = false;  // Disable bootrom mode
				Serial.println("MMU: Bootrom disabled, ROM bank 0 selectively mapped to 0x0000-0x00FF");
				
				// Verification: Check that critical address 0x0038 is not 0xFF
				Serial.printf("MMU: Post-disable verification - Address 0x0038: 0x%02X\n", mem[0x0038]);
				if (mem[0x0038] == 0xFF) {
					Serial.println("WARNING: Address 0x0038 still contains 0xFF after bootrom disable!");
					Serial.println("WARNING: This will cause infinite RST 38 loop - keeping safe NOP");
					mem[0x0038] = 0x00; // Force safe NOP
				}
			} else {
				Serial.println("ERROR: MMU: Failed to get ROM bank 0 for bootrom disable");
			}
			break;
		}
		case 0xFFFF: IE = i; break;

		default: mem[d] = i; break;
	}
}

bool mmu_init(const uint8_t* bootrom)
{
	Serial.println("MMU: Starting initialization");
	
	// Main Game Boy memory: 64KB (0x10000)
	const size_t main_mem_size = 0x10000;
	
	// PRIORITY 1: Try to use pre-allocated main memory first (CRITICAL for fragmented heap)
	mem = espeon_get_preallocated_main_mem();
	if (mem) {
		Serial.printf("MMU: Using pre-allocated main memory at %p\n", mem);
	} else {
		// Pre-allocation failed, check if we can still allocate
		Serial.println("MMU: Pre-allocated memory not available, checking fragmentation...");
		
		// Check available memory before allocation  
		size_t free_heap = ESP.getFreeHeap();
		Serial.printf("MMU: Available heap: %d bytes\n", free_heap);
		
		// Check if we have enough memory (need 64KB + safety margin)
		if (free_heap < main_mem_size + 50*1024) {
			Serial.printf("ERROR: MMU: Insufficient memory - need %d bytes, have %d\n", 
			              main_mem_size + 50*1024, free_heap);
			
			// Aggressive cleanup before giving up
			Serial.println("MMU: Performing emergency memory cleanup...");
			espeon_cleanup_rom();
			delay(100);
			
			free_heap = ESP.getFreeHeap();
			Serial.printf("MMU: Available heap after cleanup: %d bytes\n", free_heap);
			
			if (free_heap < main_mem_size + 20*1024) {
				Serial.println("ERROR: MMU: Still insufficient memory after cleanup");
				return false;
			}
		}
		
		// Check largest contiguous block
		size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
		Serial.printf("MMU: Largest contiguous block: %d bytes (need %d)\n", largest_block, main_mem_size);
		
		if (largest_block < main_mem_size) {
			Serial.println("ERROR: MMU: Memory too fragmented for 64KB allocation");
			Serial.println("MMU: Try restarting device for fresh memory layout");
			return false;
		}
		
		// Allocate main Game Boy memory
		Serial.println("MMU: Attempting to allocate main memory...");
		mem = (uint8_t*)calloc(1, main_mem_size);
		if (!mem) {
			Serial.println("ERROR: MMU: Failed to allocate main memory");
			return false;
		}
		Serial.printf("MMU: Main memory allocated successfully at %p\n", mem);
	
	// CRITICAL: Initialize critical interrupt vectors with safe NOPs to prevent infinite loops
	// This is a safety measure in case ROM loading fails
	Serial.println("MMU: Initializing critical memory locations with safe values");
	mem[0x0000] = 0x00; // NOP
	mem[0x0008] = 0x00; // RST 08 vector 
	mem[0x0010] = 0x00; // RST 10 vector
	mem[0x0018] = 0x00; // RST 18 vector
	mem[0x0020] = 0x00; // RST 20 vector
	mem[0x0028] = 0x00; // RST 28 vector
	mem[0x0030] = 0x00; // RST 30 vector
	mem[0x0038] = 0x00; // RST 38 vector - CRITICAL for our infinite loop issue
	mem[0x0040] = 0x00; // VBlank interrupt vector
	mem[0x0048] = 0x00; // LCDC status interrupt vector
	mem[0x0050] = 0x00; // Timer overflow interrupt vector
	mem[0x0058] = 0x00; // Serial transfer completion interrupt vector
	mem[0x0060] = 0x00; // High-to-low of pin number P10-P13 interrupt vector
	Serial.println("MMU: Critical memory locations initialized with NOPs for safety");
	
	// Initialize LCD registers to reasonable defaults to prevent polling loops
	mem[0xFF40] = 0x91; // LCDC - LCD enabled, BG enabled, Window tilemap select
	mem[0xFF41] = 0x00; // STAT - Mode 0 (H-Blank)
	mem[0xFF42] = 0x00; // SCY - Scroll Y
	mem[0xFF43] = 0x00; // SCX - Scroll X  
	mem[0xFF44] = 0x00; // LY - LCD Y coordinate (will be updated by LCD)
	mem[0xFF45] = 0x00; // LYC - LY Compare
	mem[0xFF47] = 0xFC; // BGP - BG Palette Data
	mem[0xFF48] = 0xFF; // OBP0 - Object Palette 0 Data
	mem[0xFF49] = 0xFF; // OBP1 - Object Palette 1 Data
	mem[0xFF4A] = 0x00; // WY - Window Y Position
	mem[0xFF4B] = 0x00; // WX - Window X Position
	Serial.println("MMU: LCD registers initialized to prevent polling loops");
	}
	
	Serial.println("MMU: Initializing MBC");
	if (!mbc_init()) {
		Serial.println("ERROR: MMU: MBC initialization failed");
		return false;
	}
	Serial.println("MMU: MBC initialized successfully");
	
	Serial.println("MMU: Getting ROM bytes");
	rom = rom_getbytes();
	Serial.println("MMU: Setting up echo memory");
	echo = mem + 0xC000 - 0xE000;
	
	if (bootrom) {
		Serial.println("MMU: Copying bootrom to memory");
		memcpy(&mem[0x0000], &bootrom[0x0000], 0x100);
		// Get bank 0 data for ROM initialization
		Serial.println("MMU: Getting ROM bank 0 for bootrom mode");
		const uint8_t* rom_bank0 = espeon_get_rom_bank(0);
		if (rom_bank0) {
			Serial.println("MMU: Copying ROM bank 0 data");
			memcpy(&mem[0x0100], &rom_bank0[0x0100], 0x4000 - 0x100);
		} else {
			Serial.println("ERROR: MMU: Failed to get ROM bank 0 for bootrom mode");
		}
		usebootrom = true;
		Serial.println("MMU: Bootrom mode initialization complete");
		return true;
	}
	
	// First ROM bank is always in RAM - get bank 0
	Serial.println("MMU: Getting ROM bank 0 for normal mode");
	const uint8_t* rom_bank0 = espeon_get_rom_bank(0);
	if (rom_bank0) {
		Serial.println("MMU: Copying ROM bank 0 data to memory");
		memcpy(&mem[0x0000], &rom_bank0[0x0000], 0x4000);
		
		// Verify ROM bank 0 was copied correctly
		Serial.printf("MMU: Verification - First few bytes: %02X %02X %02X %02X\n", 
		              mem[0x0000], mem[0x0001], mem[0x0002], mem[0x0003]);
		Serial.printf("MMU: Verification - Address 0x0038: %02X (should NOT be 0xFF)\n", mem[0x0038]);
		Serial.printf("MMU: Verification - Nintendo logo start (0x0104): %02X %02X %02X %02X\n",
		              mem[0x0104], mem[0x0105], mem[0x0106], mem[0x0107]);
		
		// Critical check: ensure 0x0038 is not 0xFF
		if (mem[0x0038] == 0xFF) {
			Serial.println("ERROR: MMU: Critical - Address 0x0038 contains 0xFF after ROM copy!");
			Serial.println("ERROR: MMU: This will cause infinite RST 38 loop!");
			return false;
		}
	} else {
		Serial.println("ERROR: MMU: Failed to get ROM bank 0 for normal mode");
		Serial.println("ERROR: MMU: Cannot proceed without valid ROM bank 0 data");
		return false;
	}

	// Default values if bootrom is not present
	mem[0xFF10] = 0x80;
	mem[0xFF11] = 0xBF;
	mem[0xFF12] = 0xF3;
	mem[0xFF14] = 0xBF;
	mem[0xFF16] = 0x3F;
	mem[0xFF19] = 0xBF;
	mem[0xFF1A] = 0x7F;
	mem[0xFF1B] = 0xFF;
	mem[0xFF1C] = 0x9F;
	mem[0xFF1E] = 0xBF;
	mem[0xFF20] = 0xFF;
	mem[0xFF23] = 0xBF;
	mem[0xFF24] = 0x77;
	mem[0xFF25] = 0xF3;
	mem[0xFF26] = 0xF1;
	mem[0xFF40] = 0x91;
	mem[0xFF47] = 0xE4;  // Background palette: 11 10 01 00 (proper dark to light progression)
	mem[0xFF48] = 0xE4;  // Sprite palette 1: same as background
	mem[0xFF49] = 0xE4;  // Sprite palette 2: same as background
	
	Serial.println("MMU: Initialization completed successfully");
	return true;
}
