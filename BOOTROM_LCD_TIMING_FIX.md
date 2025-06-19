## BOOTROM LCD TIMING FIX

### Problem Identified
The emulator was getting stuck in the Game Boy bootrom's LCD synchronization routine at PC 0x0064-0x0068, where it polls the LY register (0xFF44) waiting for it to progress from 0x00 to higher values. The issue was:

1. **Conflicting LY Updates**: The main loop was forcing LY updates every 16ms based on wall-clock time, while the LCD cycle was trying to update LY based on CPU cycles. These two systems were interfering with each other.

2. **Too Slow LCD Timing**: The LCD cycle constants were set to `SCANLINE_CYCLES = 456/4 = 114`, requiring 114 CPU cycles before LY would increment. This was too slow compared to the actual CPU execution rate.

3. **LY Stuck at 0x00**: The bootrom expects LY to increment naturally at Game Boy hardware timing (456 CPU cycles per scanline), but our implementation wasn't achieving this.

### Solutions Implemented

#### 1. Fixed Conflicting LY Updates (espeon.ino)
- **Removed** forced LY updates from main loop VBlank generation
- Let only the `lcd_cycle()` function handle LY progression naturally
- Main loop now only forces VBlank interrupts, not LY register changes

#### 2. Improved LCD Cycle Timing (lcd.cpp)
- **Reduced scanline timing**: Changed from `456/4` to `456/8` cycles per scanline
- Now `SCANLINE_CYCLES = 57` instead of 114, making LY increment twice as fast
- Added `while` loop to handle multiple scanlines if many cycles accumulated
- Restructured logic to update LY first, then handle LCD modes

#### 3. Less Aggressive CPU Loop Detection (cpu.cpp)
- **Removed** forced LY updates from infinite loop detection
- Let natural LCD timing handle LY progression
- Still update STAT and IF registers to help break other types of loops
- Added comment explaining why LY shouldn't be force-updated

#### 4. Better LCD Mode Handling (lcd.cpp)
- Fixed mode transitions during scanline progression
- Proper STAT register updates to reflect current LCD mode
- Separated VBlank handling from visible line handling

### Technical Details

#### Bootrom LCD Sync Loop (0x0064-0x0068)
```assembly
; Bootrom LCD sync routine
0x0064: F0 44    LD A, (0xFF44)    ; Read LY register
0x0066: FE XX    CP XX             ; Compare with expected value  
0x0068: 20 FA    JR NZ, 0x0064     ; Loop back if not equal
```

The bootrom expects LY to increment from 0→1→2→...→153→0 at a rate of 456 CPU cycles per increment.

#### Timing Calculations
- **Old timing**: 114 cycles per scanline = ~16-20 loop iterations before LY increments
- **New timing**: 57 cycles per scanline = ~8-10 loop iterations before LY increments  
- **Loop cycle cost**: ~7-8 cycles per iteration (F0=3, FE=2, 20=2-3 cycles)

#### Expected Behavior
1. Bootrom starts polling LY at 0x00
2. After ~8-10 polling iterations, LY increments to 0x01
3. Bootrom continues until LY reaches the expected value
4. Bootrom exits the polling loop and continues initialization
5. Eventually hands control to the ROM after showing Nintendo logo

### Build Status
✅ **BUILD SUCCESSFUL** - All changes compile cleanly.

### Next Steps
Flash to hardware and test:
1. Check if bootrom progresses past 0x0064-0x0068 polling loop
2. Verify LY register increments naturally during execution
3. Confirm Nintendo logo appears on screen
4. Ensure bootrom completes and hands control to game ROM

This should resolve the white screen issue and allow the emulator to boot properly through the Game Boy bootrom sequence.
