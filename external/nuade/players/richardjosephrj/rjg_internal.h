// SPDX-License-Identifier: MIT
// Richard Joseph game driver: the module reader and the accessors of its tables.

#pragma once

#include <nuade/players/richardjosephrj.h>

#define RJG_INSTRUMENT_SIZE 14 // bytes of an instrument row
#define RJG_MODULATION_SIZE 15 // bytes of a modulation row
#define RJG_ENVELOPE_SIZE 8    // bytes of a volume envelope row
#define RJG_SEQUENCE_ENTRY 4   // bytes of an entry of the sequence table
#define RJG_TUNE_SIZE 4        // bytes of a tune row

// The note byte of a sequence is 0..0x7F; 0x80 and above are commands, four apart.
#define RJG_COMMAND_FIRST 0x80
#define RJG_COMMAND_COUNT 22

// The period of a note is in the instrument's table, four words per note; the transpose command adds quarter notes.
#define RJG_PERIODS_PER_NOTE 4

// Reads from the module; past the end they give 0.
uint8_t rjg_u8(const RjgModule* module, uint64_t offset);
uint16_t rjg_u16(const RjgModule* module, uint64_t offset);
uint32_t rjg_u32(const RjgModule* module, uint64_t offset);

// The instrument row with this number (the driver reads rows without a bound).
RjgInstrument rjg_instrument(const RjgModule* module, uint32_t row);

// The offset of a sequence: the long of the sequence table, 0 for none.
uint32_t rjg_sequence_offset(const RjgModule* module, uint8_t sequence);

// What the subsong list says a subsong starts with: the number the conversion's InitSong hands to the start routine.
uint8_t rjg_subsong_argument(const RjgModule* module, uint32_t subsong);

// Parses a converted module; false when it is not one of a known driver build.
bool rjg_module_load(RjgModule* module, const uint8_t* data, size_t size);
