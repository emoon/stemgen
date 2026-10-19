// SPDX-License-Identifier: MIT
// Dave Lowe player: the sounds of a module. The sample records are not listed anywhere the player can count on (a
// Deli module has no table, and the records outside a table are the short immediate ones), so the sounds are found
// the way the engine finds them: by reading the patterns of every subsong and collecting the records their sample
// commands name. The patterns are read once, without playing them.

#include "dl_internal.h"
#include <stdlib.h>
#include <string.h>

#define MAX_COMMAND_WORD 0x64 // a larger word is a note (see dl_engine.c)
#define MAX_LIST_ENTRIES 0x10000
#define MAX_REPEAT_DEPTH 32

// Operand sizes in bytes
#define SAMPLE_OPERANDS 4
#define ENVELOPE_OPERANDS 4
#define SLIDE_OPERANDS 8
#define VIBRATO_OPERANDS 12
#define REST_OPERANDS 2
#define JUMP_OPERANDS 4
#define NOTE_OPERANDS 2 // the length of the note
// A repeat: a counter word, the address to go to while it counts down, the address to go to when it is 0
#define REPEAT_WHILE_COUNTING 2
#define REPEAT_WHEN_DONE 6

typedef struct Collector {
    const DlPlayer* player;
    uint8_t* seen; // one bit per word of the module: a command word that has been read
    uint32_t* records;
    uint32_t count;
    uint32_t capacity;
    bool out_of_memory;
} Collector;

static bool test_and_set_seen(Collector* c, uint32_t address) {
    const uint32_t bit = address / 2;
    const bool seen = (c->seen[bit / 8] & (1u << (bit % 8))) != 0;
    c->seen[bit / 8] |= (uint8_t)(1u << (bit % 8));
    return seen;
}

// Keeps the records sorted and without duplicates
static void add_record(Collector* c, uint32_t record) {
    uint32_t lo = 0;
    uint32_t hi = c->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (c->records[mid] < record) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < c->count && c->records[lo] == record) {
        return;
    }
    if (c->count == c->capacity) {
        const uint32_t capacity = c->capacity != 0 ? c->capacity * 2 : 64;
        uint32_t* records = realloc(c->records, capacity * sizeof(*records));
        if (records == nullptr) {
            c->out_of_memory = true;
            return;
        }
        c->records = records;
        c->capacity = capacity;
    }
    memmove(c->records + lo + 1, c->records + lo, (c->count - lo) * sizeof(*c->records));
    c->records[lo] = record;
    c->count++;
}

// Reads a pattern up to a command that ends it. A jump continues at its target, a repeat at both of its targets.
static void walk_pattern(Collector* c, uint32_t address, int depth) {
    const DlImage* image = c->player->image;
    const DlVariant* variant = &c->player->variant;
    while (address < image->size && !c->out_of_memory && !test_and_set_seen(c, address)) {
        const uint16_t word = dli_u16(image, address);
        address += 2;
        if (word > MAX_COMMAND_WORD) {
            address += NOTE_OPERANDS;
            continue;
        }
        const uint32_t index = word >> 2;
        const bool valid = (word & 3) == 0 && index < variant->command_count;
        switch (valid ? variant->commands[index] : DlCommand_Freeze) {
            case DlCommand_Sample:
                add_record(c, dli_u32(image, address));
                address += SAMPLE_OPERANDS;
                break;
            case DlCommand_Envelope:
                address += ENVELOPE_OPERANDS;
                break;
            case DlCommand_Slide:
                address += SLIDE_OPERANDS;
                break;
            case DlCommand_Vibrato:
                address += VIBRATO_OPERANDS;
                break;
            case DlCommand_Rest:
                address += REST_OPERANDS;
                break;
            case DlCommand_SlideOff:
            case DlCommand_VibratoOff:
            case DlCommand_Filter:
            case DlCommand_BonusOn:
                break;
            case DlCommand_Jump:
                address = dli_u32(image, address);
                break;
            case DlCommand_Repeat:
                if (depth >= MAX_REPEAT_DEPTH) {
                    return;
                }
                walk_pattern(c, dli_u32(image, address + REPEAT_WHILE_COUNTING), depth + 1);
                address = dli_u32(image, address + REPEAT_WHEN_DONE);
                break;
            case DlCommand_NextPattern:
            case DlCommand_Stop:
            case DlCommand_Restart:
            case DlCommand_SongEnd:
            case DlCommand_Freeze:
                return;
        }
    }
}

static void walk_list(Collector* c, uint32_t list) {
    const DlImage* image = c->player->image;
    for (uint32_t i = 0; i < MAX_LIST_ENTRIES && list + i * 4 < image->size; i++) {
        const uint32_t pattern = dli_u32(image, list + i * 4);
        if (pattern == 0) {
            break;
        }
        walk_pattern(c, pattern, 0);
    }
}

static DlSubsong subsong_of(const DlPlayer* player, uint32_t index) {
    switch (player->kind) {
        case DlKind_Game:
            return dl_game_subsong(player->image, &player->game, index);
        case DlKind_Deli:
            return dl_deli_subsong(player->image, &player->deli, index);
        case DlKind_New:
            break;
    }
    return dl_new_subsong(player->image, &player->new_module, index);
}

uint32_t* dl_collect_sample_records(const DlPlayer* player, uint32_t* count) {
    Collector c = { .player = player, .seen = calloc(player->image->size / 16 + 1, 1) };
    *count = 0;
    if (c.seen == nullptr) {
        return nullptr;
    }
    for (uint32_t s = 0; s < player->subsong_count; s++) {
        const DlSubsong subsong = subsong_of(player, s);
        for (uint32_t v = 0; v < DL_VOICES; v++) {
            walk_list(&c, subsong.lists[v]);
        }
        if (player->variant.effect_table != 0 && subsong.effect_request != 0) {
            const uint32_t pattern = dli_u32(player->image, player->variant.effect_table + subsong.effect_request * 4);
            walk_pattern(&c, pattern, 0);
        }
    }
    free(c.seen);
    if (c.out_of_memory) {
        free(c.records);
        return nullptr;
    }
    *count = c.count;
    return c.records;
}

uint32_t dl_sample_count(DlPlayer* player) {
    if (!player->records_found) {
        player->records = dl_collect_sample_records(player, &player->record_count);
        player->records_found = true;
    }
    return player->record_count;
}

bool dl_sample_pcm(DlPlayer* player, uint32_t n, int* instrument, const int8_t** data, uint32_t* length,
                   uint32_t* loop_start, uint32_t* loop_length) {
    if (n >= dl_sample_count(player)) {
        return false;
    }
    const DlImage* image = player->image;
    const uint32_t record = player->records[n];
    const DlSampleRecord r = dl_read_sample_record(image, record);
    const uint32_t start = r.address & ~1u;
    if (r.length_words == 0 || start >= image->module_size) {
        return false;
    }
    // a sound that runs past the module plays zeros, which are not worth a file
    uint32_t size = (uint32_t)r.length_words * 2;
    *loop_start = 0;
    *loop_length = 0;
    if (r.state != DlSampleState_Immediate && r.loop_length_words > 1 && (r.loop_address & ~1u) >= start) {
        // the loop part follows the first part in the module (the attack is a prefix of the sound)
        const uint32_t loop_at = (r.loop_address & ~1u) - start;
        const uint32_t loop_size = (uint32_t)r.loop_length_words * 2;
        if (loop_at + loop_size > size) {
            size = loop_at + loop_size;
        }
        *loop_start = loop_at;
        *loop_length = loop_size;
    }
    if (size > image->module_size - start) {
        size = image->module_size - start;
        if (*loop_start >= size) {
            *loop_start = 0;
            *loop_length = 0;
        } else if (*loop_length > size - *loop_start) {
            *loop_length = size - *loop_start;
        }
    }
    *instrument = dl_instrument_of_record(player, record);
    *data = dli_sample(image, start);
    *length = size;
    return true;
}
