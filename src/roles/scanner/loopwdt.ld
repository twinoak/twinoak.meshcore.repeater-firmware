/* Where the loop watchdog's marker lives: RAM that Reset_Handler neither copies into nor zeroes.
 *
 * src/roles/scanner/LoopWdt.cpp writes one word here before it resets the part, and reads it back
 * on the next boot to know that the reset was the loop check's - which is how `boot` says
 * `reset: wdt` for a reset RESETREAS calls SREQ (docs/noisescope.md section 6).
 *
 * This is an augmentation of the core's own linker script and not a replacement of it: the
 * scanner environment passes it as a second -T (platformio.ini), exactly as the core's
 * ldscript.ld inserts its own .svc_data and .fs_data after .data. The three things the placement
 * has to get right, against cores/nRF5/linker/nrf52_common.ld and gcc_startup_nrf52840.S of the
 * pinned Adafruit core:
 *
 *   - AFTER .bss, so the word is outside the range Reset_Handler copies .data into
 *     (__data_start__ .. __bss_start__) and outside the range it zeroes
 *     (__bss_start__ .. __bss_end__). nRF52840 RAM survives a soft reset; this word therefore
 *     survives one too;
 *   - BEFORE .heap, which is the next section of nrf52_common.ld, so __HeapBase sits above the
 *     word and malloc can never hand it out;
 *   - NOLOAD, so it costs no flash, puts no RAM address into firmware.bin and is not part of any
 *     load segment (readelf says NOBITS, and the .bin is the same size it was).
 *
 * No `> RAM` on the section: the linker reads this script before the one that declares the memory
 * regions, so naming the region here is a warning and nothing else. Where the word lands is
 * decided by the INSERT - the location counter is already inside RAM at the end of .bss - and the
 * two ASSERTs below are what hold that, with the addresses the map file shows.
 *
 * The two ASSERTs are the placement held where it is made. If this file ever stops being applied
 * at all, .noinit becomes an orphan section the linker places by its own rules and the ASSERTs go
 * with it - what catches that is the board: docs/bench.md section 9's watchdog line reads
 * `reset` after a `hang`, and a marker that did not survive makes it `soft`.
 */

SECTIONS
{
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    PROVIDE(__noinit_start__ = .);
    KEEP(*(.noinit))
    KEEP(*(.noinit.*))
    . = ALIGN(4);
    PROVIDE(__noinit_end__ = .);
  }
} INSERT AFTER .bss;

ASSERT(__noinit_start__ >= __bss_end__,
       "the loop watchdog's marker is inside .bss or .data: Reset_Handler would clear it on every boot")
ASSERT(__noinit_end__ <= __HeapBase,
       "the loop watchdog's marker is inside the heap: malloc could hand it out")
