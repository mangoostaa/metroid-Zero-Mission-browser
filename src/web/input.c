/* input.c - keyboard/gamepad input for the web port (TARGET_WEB).
 *
 * gba_set_keys() stores an already-formed active-low GBA KEYINPUT value as a
 * little-endian u16 at gGbaIo + 0x130 (REG_KEY_INPUT, per include/gba/keys.h).
 * The game reads it into gButtonInput / gChangedInput. Active-low: a SET bit
 * means NOT pressed, so all-released == 0x03FF.
 *
 * Stored byte-wise (LE) so the result is independent of host alignment/endian;
 * wasm is little-endian regardless.
 */
#include <stdint.h>

#include "gba_hle.h" /* frozen contract: extern unsigned char gGbaIo[] */

#define IO_KEY_INPUT 0x130

void gba_set_keys(uint16_t keyinput)
{
    gGbaIo[IO_KEY_INPUT + 0] = (unsigned char)(keyinput & 0xFF);
    gGbaIo[IO_KEY_INPUT + 1] = (unsigned char)((keyinput >> 8) & 0xFF);
}
