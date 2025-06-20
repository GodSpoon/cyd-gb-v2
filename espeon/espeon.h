#ifndef ESPEON_H
#define ESPEON_H

#include <Arduino.h>
#include <vector>

extern volatile bool sram_modified;
extern uint8_t btn_directions;
extern uint8_t btn_faces;

typedef uint16_t fbuffer_t;
extern uint16_t palette[];

void espeon_update(void);
void espeon_init(void);
void espeon_faint(const char* msg);
fbuffer_t* espeon_get_framebuffer(void);
void espeon_clear_framebuffer(fbuffer_t col);
void espeon_end_frame(void);
void espeon_clear_screen(uint16_t col);
void espeon_set_palette(const uint32_t* col);
void espeon_render_border(const uint8_t* img, uint32_t size);
void espeon_save_sram(uint8_t* ram, uint32_t size);
void espeon_load_sram(uint8_t* ram, uint32_t size);
const uint8_t* espeon_load_rom(const char* path);
const uint8_t* espeon_load_bootrom(const char* path);
const uint8_t* espeon_get_rom_bank(uint16_t bank_number);  // For streaming mode ROM access
void espeon_set_brightness(uint8_t brightness);
void espeon_set_brightness_digital(bool on); // Alternative for troubleshooting
void espeon_cleanup_rom();
void espeon_cleanup_spi();
void espeon_check_memory();
void espeon_set_preallocated_main_mem(uint8_t* mem);
uint8_t* espeon_get_preallocated_main_mem();
void espeon_set_preallocated_mbc_ram(uint8_t* ram, size_t size);
uint8_t* espeon_get_preallocated_mbc_ram(size_t* size);

// ROM file management
const std::vector<String>& espeon_get_rom_files();
int espeon_get_rom_count();

#endif
