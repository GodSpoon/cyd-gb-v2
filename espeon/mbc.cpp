#include "mbc.h"
#include "rom.h"
#include "espeon.h"
#include <esp_heap_caps.h>

#define SET_ROM_BANK(n)		do { \
	const uint8_t* new_bank = espeon_get_rom_bank((n) & (rom_banks - 1)); \
	if (new_bank) { \
		rombank = new_bank; \
	} else { \
		Serial.printf("ERROR: Failed to load ROM bank %d, keeping previous bank\n", (n) & (rom_banks - 1)); \
	} \
} while(0)
#define SET_RAM_BANK(n)		(rambank = &ram[((n) & (ram_banks - 1)) * 0x2000])

static uint32_t curr_rom_bank = 1;
static uint8_t rom_banks;
static uint32_t curr_ram_bank = 0;
static uint8_t ram_banks;
static bool ram_select;
static bool ram_enabled;
static const uint8_t *rom;
static uint8_t *ram;
const uint8_t *rombank;
uint8_t *rambank;
static const s_rominfo *rominfo;

MBCReader mbc_read_ram;
MBCWriter mbc_write_rom;
MBCWriter mbc_write_ram;


bool mbc_init()
{
	Serial.println("MBC: Starting initialization");
	rom = rom_getbytes();
	Serial.println("MBC: Got ROM bytes");
	rominfo = rom_get_info();
	Serial.println("MBC: Got ROM info");
	
	rom_banks = rominfo->rom_banks;
	ram_banks = rominfo->ram_banks;
	Serial.printf("MBC: ROM banks: %d, RAM banks: %d\n", rom_banks, ram_banks);
	
	int ram_size = rom_get_ram_size();
	Serial.printf("MBC: Need RAM size: %d bytes\n", ram_size);
	
	// Check available memory before MBC RAM allocation
	size_t free_heap = ESP.getFreeHeap();
	Serial.printf("MBC: Available heap: %d bytes\n", free_heap);
	
	// Calculate actual allocation size (minimum 8KB for compatibility)
	size_t alloc_size = (ram_size < 1024*8) ? 1024*8 : ram_size;
	
	// Try to use pre-allocated RAM first to avoid fragmentation
	size_t preallocated_size = 0;
	ram = espeon_get_preallocated_mbc_ram(&preallocated_size);
	
	if (ram && preallocated_size >= alloc_size) {
		Serial.printf("MBC: Using pre-allocated RAM (%d bytes available)\n", preallocated_size);
	} else {
		// Fallback to regular allocation
		if (ram) {
			Serial.println("MBC: Pre-allocated RAM too small, freeing and reallocating");
			free(ram);
			ram = nullptr;
		}
		
		// Check if we have enough memory for MBC RAM
		if (free_heap < alloc_size + 30*1024) {
			Serial.printf("ERROR: MBC: Insufficient memory - need %d bytes, have %d\n", 
			              alloc_size + 30*1024, free_heap);
			return false;
		}
		
		// Check largest contiguous block  
		size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
		Serial.printf("MBC: Largest contiguous block: %d bytes (need %d)\n", largest_block, alloc_size);
		
		if (largest_block < alloc_size) {
			Serial.println("ERROR: MBC: Memory too fragmented for RAM allocation");
			return false;
		}
		
		Serial.printf("MBC: Attempting to allocate %d bytes of RAM...\n", alloc_size);
		ram = (uint8_t *)calloc(1, alloc_size);
		if (!ram) {
			Serial.println("ERROR: MBC: Failed to allocate RAM");
			return false;
		}
	}
	Serial.printf("MBC: RAM allocated successfully at %p\n", ram);
	
	if (rominfo->has_battery && ram_size) {
		Serial.println("MBC: Loading SRAM");
		espeon_load_sram(ram, ram_size);
	}
	
	Serial.println("MBC: Setting ROM bank 1");
	SET_ROM_BANK(1);
	if (!rombank) {
		Serial.println("ERROR: MBC: Failed to get ROM bank 1 - performing memory cleanup and retry");
		
		// Try emergency cleanup
		espeon_check_memory();
		
		// Retry once more
		SET_ROM_BANK(1);
		if (!rombank) {
			Serial.println("ERROR: MBC: Failed to get ROM bank 1 after cleanup");
			Serial.println("WARNING: MBC will continue with limited functionality");
			// Don't return false here - let the system continue with degraded performance
			// Set rombank to bank 0 as fallback
			Serial.println("MBC: Using ROM bank 0 as fallback for bank 1");
			rombank = espeon_get_rom_bank(0);
			if (!rombank) {
				Serial.println("CRITICAL: MBC: Even ROM bank 0 is unavailable");
				return false;
			}
		}
	}
	Serial.println("MBC: ROM bank 1 set successfully");
	
	Serial.println("MBC: Setting RAM bank 0");
	SET_RAM_BANK(0);
	Serial.println("MBC: RAM bank 0 set successfully");
	
	Serial.println("MBC: Setting up MBC type handlers");
	switch(rominfo->rom_mapper)
	{
		case MBC2:
			mbc_write_rom = MBC3_write_ROM;
			mbc_write_ram = MBC1_write_RAM;
			mbc_read_ram = MBC1_read_RAM;
		break;
		case MBC3:
			mbc_write_rom = MBC3_write_ROM;
			mbc_write_ram = MBC3_write_RAM;
			mbc_read_ram = MBC3_read_RAM;
		break;
		default:
			mbc_write_rom = MBC1_write_ROM;
			mbc_write_ram = MBC1_write_RAM;
			mbc_read_ram = MBC1_read_RAM;
		break;
	}
	
	Serial.println("MBC: Initialization completed successfully");
	return true;
}

uint8_t* mbc_get_ram()
{
	return ram;
}


void MBC3_write_ROM(uint16_t d, uint8_t i)
{
	if(d < 0x2000)
		ram_enabled = ((i & 0x0F) == 0x0A);
	
	else if(d < 0x4000) {
		uint8_t old_bank = curr_rom_bank;
		curr_rom_bank = i & 0x7F;

		if(curr_rom_bank == 0)
			curr_rom_bank++;

		// Bank switch counting (debug output removed for performance)
		static uint32_t bank_switch_count = 0;
		bank_switch_count++;

		SET_ROM_BANK(curr_rom_bank);
	}
	
	else if(d < 0x6000) {
		/* TODO: select RTC */
		curr_ram_bank = i & 0x07;
		SET_RAM_BANK(curr_ram_bank);
	}
}

void MBC3_write_RAM(uint16_t d, uint8_t i)
{
	/* TODO: write to RTC */
	if (!ram_enabled)
		return;
	rambank[d - 0xA000] = i;
	//sram_modified = true;
}

uint8_t MBC3_read_RAM(uint16_t i)
{
	return ram_enabled ? rambank[i - 0xA000] : 0xFF;
}


void MBC1_write_ROM(uint16_t d, uint8_t i)
{
	if(d < 0x2000)
		ram_enabled = ((i & 0x0F) == 0x0A);
	
	else if(d < 0x4000) {
		uint8_t old_bank = curr_rom_bank;
		curr_rom_bank = (curr_rom_bank & 0x60) | (i & 0x1F);

		if(curr_rom_bank == 0 || curr_rom_bank == 0x20 || curr_rom_bank == 0x40 || curr_rom_bank == 0x60)
			curr_rom_bank++;

		// Bank switch counting (debug output removed for performance)
		static uint32_t bank_switch_count = 0;
		bank_switch_count++;
		
		SET_ROM_BANK(curr_rom_bank);
	}
	
	else if(d < 0x6000) {
		if (ram_select) {
			curr_ram_bank = i&3;
			SET_RAM_BANK(curr_ram_bank);
		}
		else {
			uint8_t old_bank = curr_rom_bank;
			curr_rom_bank = ((i & 0x3)<<5) | (curr_rom_bank & 0x1F);
			
			// Upper bank switch counting (debug output removed for performance)
			static uint32_t upper_switch_count = 0;
			upper_switch_count++;
			
			SET_ROM_BANK(curr_rom_bank);
		}
	}
	
	else if(d < 0x8000) {
		ram_select = i&1;
		if (ram_select) {
			curr_rom_bank &= 0x1F;
			SET_ROM_BANK(curr_rom_bank);
		}
		else {
			curr_ram_bank = 0;
			SET_RAM_BANK(curr_ram_bank);
		}
	}
}

void MBC1_write_RAM(uint16_t d, uint8_t i)
{
	if (!ram_enabled)
		return;
	rambank[d - 0xA000] = i;
	//sram_modified = true;
}

uint8_t MBC1_read_RAM(uint16_t i)
{
	return ram_enabled ? rambank[i - 0xA000] : 0xFF;
}
