## CRITICAL BUG FIXED: LY REGISTER RESET LOOP

### Root Cause Identified
The LCD timing was working correctly, but there was a **critical bug** causing an infinite reset loop:

**The Problem:**
1. `lcd_cycle()` increments `lcd_line` from 0 to 1
2. Calls `mem_write_byte(0xFF44, lcd_line)` to update LY register  
3. **MMU write handler**: `case 0xFF44: lcd_reset(); break;`
4. `lcd_reset()` immediately sets `lcd_line = 0`
5. **Result**: LY register always reset back to 0, creating infinite loop

**Debug Evidence:**
```
LCD_DEBUG: LY incremented to 0 (lcd_cycles was 28)  <-- Always 0!
CPU: 28103 instrs, PC=0x009F, op=0x17                <-- CPU progressed past 0x0064!
```

The CPU actually **DID** progress past the bootrom polling loop (0x0064-0x0068), reaching PC=0x009F, but the LY register kept resetting to 0.

### Fixes Applied

#### 1. Fixed LY Register Write Handler (mem.cpp)
**Before:**
```c
case 0xFF44: lcd_reset(); break;  // WRONG: Reset LCD on any LY write
```

**After:**
```c
case 0xFF44: /* LY register is read-only, ignore writes */ break;
```

**Reasoning**: On real Game Boy hardware, the LY register is read-only. Writes should be ignored, not trigger LCD reset.

#### 2. Fixed LY Register Update Method (lcd.cpp)  
**Before:**
```c
mem_write_byte(0xFF44, lcd_line);  // Triggers MMU write handler -> lcd_reset()
```

**After:**
```c
mem[0xFF44] = lcd_line;  // Direct memory update, bypasses write handler
```

**Reasoning**: Internal LCD updates should directly modify memory without triggering MMU write handlers.

#### 3. Added Detailed Debug Output
Enhanced debugging to show the increment process:
- Before increment value
- After increment value  
- Reset detection
- Final LY value

### Expected Results

With this fix, we should now see:
```
LCD_DEBUG: Before increment - lcd_line=0
LCD_DEBUG: After increment - lcd_line=1  
LCD_DEBUG: LY incremented to 1 (lcd_cycles was 28)
LCD_DEBUG: Before increment - lcd_line=1
LCD_DEBUG: After increment - lcd_line=2
LCD_DEBUG: LY incremented to 2 (lcd_cycles was 28)
```

And the bootrom should progress:
```
CPU: POLL(0xFF44)=0x01
CPU: POLL(0xFF44)=0x02
```

Eventually reaching the Nintendo logo display and game boot.

### Technical Significance

This was a **fundamental emulation bug** where:
- The LCD timing system was actually working correctly
- The CPU was properly executing and progressing  
- But a single incorrect MMU write handler was sabotaging the entire boot process

The bootrom **WAS** progressing (reached PC=0x009F), but got stuck later because LY couldn't increment naturally.

### Build Status
✅ **BUILD SUCCESSFUL** - Critical bug fix applied.

**Next Test**: Flash to hardware and verify that LY now increments properly: 0→1→2→3→...→153→0, allowing the bootrom to complete its LCD synchronization and boot sequence.
