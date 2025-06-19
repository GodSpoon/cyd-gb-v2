## AGGRESSIVE LCD TIMING AND FALLBACK DEBUGGING

### Problem Identified
Even with improved LCD timing, the LY register was still stuck at 0x00 after 700,000+ instructions. This indicates a fundamental issue with either:
1. LCD cycle accumulation not working properly
2. LCD timing constants still too conservative  
3. LCD enable state not being properly managed
4. Cycle counting logic having bugs

### Additional Fixes Applied

#### 1. Much More Aggressive LCD Timing (lcd.cpp)
- **Further reduced timing**: Changed from `456/8 = 57` to `456/16 = 28` cycles per scanline
- Now LY should increment every ~4 bootrom polling iterations instead of ~8
- This should make any timing issues much more obvious

#### 2. Comprehensive Debug Output (lcd.cpp)
Added detailed debug information every 3 seconds:
- **Call frequency**: How many times `lcd_cycle()` is called
- **Cycle accumulation**: Current `lcd_cycles` value and input cycle parameter
- **Scanline progress**: How many cycles needed for next LY increment
- **LCD enable state**: Whether LCD is actually enabled
- **LY increment logging**: Explicit notification when LY changes

#### 3. CPU Cycle Debug Output (cpu.cpp)
Added debug output every 50,000 instructions:
- **Cycle values**: What cycle count each CPU instruction returns
- **Total cycles**: Cumulative cycle counter to verify accumulation
- **Delta tracking**: Ensure cycle deltas are reasonable

#### 4. Emergency LY Fallback (cpu.cpp)
Added safety mechanism for when natural LCD timing fails:
- **Stuck detection**: Monitor if LY stays at same value for >10,000 polling attempts
- **Force increment**: Manually increment LY if it's stuck too long
- **Debug notification**: Clear indication when fallback triggers

#### 5. Enhanced Loop Detection
Improved bootrom polling loop debugging:
- **LY stuck counting**: Track consecutive polls of same LY value
- **Forced update indication**: Show when emergency LY increment occurs
- **Safer register updates**: Only modify STAT and IF, let LCD timing handle LY naturally

### Expected Debug Output

With these changes, we should see:
```
LCD_DEBUG: Called 50000 times, lcd_cycles=1234, cycles_param=3, SCANLINE_CYCLES=28, lcd_line=0
LCD_DEBUG: LCD enabled=1, need 15 cycles for next scanline
CPU_DEBUG: Instruction 50000 returned 3 cycles, total c.cycles=150000
LCD_DEBUG: LY incremented to 1 (lcd_cycles was 45)
```

If the natural timing still doesn't work, we'll see:
```
CPU: POLL(0xFF44)=0x00 FORCE_LY_UPDATE
```

### Diagnostic Questions This Will Answer

1. **Is lcd_cycle() being called?** Debug output will show call frequency
2. **Are cycles accumulating?** Will show lcd_cycles progression  
3. **Is LCD enabled?** Will explicitly report enable state
4. **Are cycle values reasonable?** CPU debug shows instruction cycle returns
5. **Is timing too slow?** With 28-cycle scanlines, should be very fast

### Build Status
✅ **BUILD SUCCESSFUL** - All debug additions compile cleanly.

This version should either fix the LY progression naturally, or provide clear diagnostic information about why it's not working, plus a fallback to keep the bootrom progressing.
