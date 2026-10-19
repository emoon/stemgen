// SPDX-License-Identifier: MIT
// Richard Joseph game driver, translated from the driver code of the converted modules (Cadaver, The Lure of the
// Temptress, Moonstone) and from Wanted Team's "Richard Joseph_v2.asm" (the eagleplayer that plays them; adapted by
// Wanted Team, driver (c) 1989-92 by Richard Joseph).
//
// Four voices, each running a sequence: a byte stream of notes (0..0x7F) and commands (0x80 and up, four apart; 22 of
// them). A note starts the voice's instrument at the pitch the instrument's period table gives, for a number of ticks
// (the duration times the tempo). The driver runs once per tick (50 Hz) and has three passes: the sequencers of the
// four voices, the volume envelopes, then the writes to the Paula registers. A sound plays its sample once, or loops
// it; the channel's audio interrupt chains the loop pointer or ends the one-shot (see audio_interrupt).

#include "nuade_paula_channel.h"
#include "nuade_tracker.h"
#include "rjg_internal.h"
#include <stdlib.h>
#include <string.h>

// What a voice may do in one tick before the player gives up on its sequence (the original would hang): a command
// costs one, and the sequence bytes a duration sum adds up cost one each.
#define COMMAND_BUDGET (1u << 18)

// Ticks of the driver run to find a subsong's tempo, and the longest row of the grid
#define GRID_SCAN_TICKS 64
#define GRID_MAX_TICKS 64

// What the silent block plays: one word.
#define DUMMY_WORDS 1
// A voice record's period before the first note: the instrument-0 defaults that a sequence start writes
#define DEFAULT_PERIOD 0x100
#define INITIAL_LENGTH_WORDS 8

#define VOLUME_MASK 0x7F
#define MODULATION_VOLUME_MASK 0x3F
#define ENVELOPE_CEILING 0x40

#define STACK_LONG 4
#define STACK_WORD 2

typedef enum Command {
    Command_Volume,      // operand: the voice's volume ceiling
    Command_SkipA,       // operand ignored
    Command_Restart,     // the sequence starts over
    Command_Duration,    // operand: ticks of a note in units of the tempo
    Command_Rest,        // wait a note's duration
    Command_Tempo,       // operand: the tempo is the tempo scale divided by it
    Command_DurationSum, // operands: count, then that many numbers; their sum in units of the tempo
    Command_Modulation,  // operand: row of the modulation table
    Command_SkipB,
    Command_SkipC,
    Command_Flags, // operand: bit 0 sets the volume slots to reload when idle, bit 1 the pitch slots
    Command_End,   // the voice stops
    Command_Call,  // operand: sequence number; return with Command_Return
    Command_Return,
    Command_TransposeAdd, // operand: signed quarter notes
    Command_Transpose,    // operand: signed quarter notes
    Command_LoopBegin,    // operand: the loop runs this many times plus once
    Command_LoopEnd,
    Command_Envelope, // operand: row of the envelope table
    Command_EnvelopeOff,
    Command_Instrument, // operand: instrument row
    Command_Jump,       // operand: sequence number
} Command;

// Paula ignores the low bit of an AUDxLC address.
static const int8_t* sample_pointer(const RjgModule* m, uint64_t offset) {
    return offset <= m->size ? (const int8_t*)m->data + (offset & ~(uint64_t)1) : nullptr;
}

// --- the call stacks: the four 0x80-byte blocks next to each other, a voice's pointer counts down

static uint32_t stack_top(uint32_t voice) {
    return (voice + 1) * RJG_STACK_SPACING;
}

static uint8_t stack_byte(const RjgPlayer* p, uint32_t at) {
    return at < RJG_STACK_BYTES ? p->stack_memory[at] : 0;
}

static uint32_t stack_peek_long(const RjgPlayer* p, uint32_t at) {
    return (uint32_t)stack_byte(p, at) << 24 | (uint32_t)stack_byte(p, at + 1) << 16
           | (uint32_t)stack_byte(p, at + 2) << 8 | stack_byte(p, at + 3);
}

static uint16_t stack_peek_word(const RjgPlayer* p, uint32_t at) {
    return (uint16_t)(stack_byte(p, at) << 8 | stack_byte(p, at + 1));
}

// A write past the blocks goes nowhere (it would hit the driver's other variables).
static void stack_poke_long(RjgPlayer* p, uint32_t at, uint32_t value) {
    for (uint32_t i = 0; i < 4 && at + i < RJG_STACK_BYTES; i++) {
        p->stack_memory[at + i] = (uint8_t)(value >> (24 - 8 * i));
    }
}

static void stack_poke_word(RjgPlayer* p, uint32_t at, uint16_t value) {
    for (uint32_t i = 0; i < 2 && at + i < RJG_STACK_BYTES; i++) {
        p->stack_memory[at + i] = (uint8_t)(value >> (8 - 8 * i));
    }
}

// A push below the blocks has nowhere to go: the voice's sequence ends.
static bool stack_push_long(RjgPlayer* p, RjgVoice* v, uint32_t value) {
    if (v->stack < STACK_LONG) {
        return false;
    }
    v->stack -= STACK_LONG;
    stack_poke_long(p, v->stack, value);
    return true;
}

static bool stack_push_word(RjgPlayer* p, RjgVoice* v, uint16_t value) {
    if (v->stack < STACK_WORD) {
        return false;
    }
    v->stack -= STACK_WORD;
    stack_poke_word(p, v->stack, value);
    return true;
}

// --- the channel registers

// AUDxLC / AUDxLEN / AUDxPER / AUDxVOL of a silent block: volume 0 and one word. The sequencer plays the first word of
// the instrument table (an instrument row starts with a loop flag, -1 or 1: near silence); a sequence start and the
// audio interrupt use the dummy word.
static void point_at_silence(RjgVoice* v, const int8_t* word) {
    v->paula.volume = 0;
    v->paula.lc = word;
    v->paula.len_words = DUMMY_WORDS;
    v->paula.period = 1;
}

// The channel's audio interrupt, at every block start: the first is the DMA start. A sound that loops points AUDxLC at
// its loop (the interrupt then switches itself off); a one-shot points it at the silent block and, at the next
// interrupt (the silent block starting), stops the channel.
static void audio_interrupt(const RjgModule* m, RjgVoice* v) {
    if (!v->interrupt_enabled) {
        return;
    }
    v->interrupt_phase--;
    if (v->instrument.loops < 0) {
        v->paula.lc = sample_pointer(m, (uint64_t)v->instrument.sample + v->instrument.loop_offset);
        v->paula.len_words = (uint16_t)(v->instrument.length_words - (v->instrument.loop_offset >> 1));
        v->interrupt_enabled = false;
    } else if (v->interrupt_phase == 0) {
        v->interrupt_enabled = false;
        nuade_paula_channel_dma_off(&v->paula);
    } else {
        v->paula.lc = sample_pointer(m, m->dummy);
        v->paula.len_words = DUMMY_WORDS;
    }
}

// Block starts the mixer played since the last tick are the interrupts that fired in between.
static void answer_interrupts(const RjgModule* m, RjgVoice* v, const NuadeVoices* voices) {
    NuadeVoiceState state;
    nuade_voice_get_state(voices, v->index, &state);
    for (uint32_t n = state.wraps - v->wraps_seen; n > 0; n--) {
        audio_interrupt(m, v);
    }
    v->wraps_seen = state.wraps;
}

// --- modulation slots: counters that reload from the row the modulation command copied

static void reload_volume_slots(RjgVoice* v) {
    for (uint32_t k = 0; k < RJG_VOLUME_SLOTS; k++) {
        v->volume_delay[k] = v->volume_delay_reload[k];
        v->volume_count[k] = v->volume_count_reload[k];
    }
}

static void reload_pitch_slots(RjgVoice* v) {
    for (uint32_t k = 0; k < RJG_PITCH_SLOTS; k++) {
        v->pitch_delay[k] = v->pitch_delay_reload[k];
        v->pitch_count[k] = v->pitch_count_reload[k];
    }
}

// The first slot that is waiting or has counts left works this tick; with none, the offset is 0 and the slots reload if
// the flag says so.
static void run_volume_slots(RjgVoice* v) {
    bool active = false;
    for (uint32_t k = 0; k < RJG_VOLUME_SLOTS && !active; k++) {
        if (v->volume_delay[k] != 0) {
            v->volume_delay[k]--;
            active = true;
        } else if (v->volume_count[k] != 0) {
            v->volume_count[k]--;
            v->volume_offset = (uint16_t)(v->volume_offset + v->volume_delta[k]);
            v->volume_delay[k] = v->volume_delay_reload[k];
            active = true;
        }
    }
    if (!active) {
        v->volume_offset = 0;
        if (v->volume_slots_reload) {
            reload_volume_slots(v);
        }
    }
}

static void run_pitch_slots(RjgVoice* v) {
    bool active = false;
    for (uint32_t k = 0; k < RJG_PITCH_SLOTS && !active; k++) {
        if (v->pitch_delay[k] != 0) {
            v->pitch_delay[k]--;
            active = true;
        } else if (v->pitch_count[k] != 0) {
            v->pitch_count[k]--;
            v->pitch_offset = (uint16_t)(v->pitch_offset + (uint16_t)(int16_t)(int8_t)v->pitch_delta[k]);
            v->pitch_delay[k] = v->pitch_delay_reload[k];
            active = true;
        }
    }
    if (!active && v->pitch_slots_reload) {
        reload_pitch_slots(v);
    }
}

// --- the sequencer commands

// The row of the modulation table: 15 signed bytes that fill the slot tables in the order of the record.
static void load_modulation(const RjgModule* m, RjgVoice* v, uint8_t row) {
    v->volume_slots_reload = false;
    v->pitch_slots_reload = false;
    uint16_t word[RJG_MODULATION_SIZE];
    const uint64_t at = (uint64_t)m->modulation + (uint32_t)row * RJG_MODULATION_SIZE;
    for (uint32_t j = 0; j < RJG_MODULATION_SIZE; j++) {
        word[j] = (uint16_t)(int16_t)(int8_t)rjg_u8(m, at + j);
    }
    for (uint32_t k = 0; k < RJG_VOLUME_SLOTS; k++) {
        v->volume_count_reload[k] = word[k];
        v->volume_delta[k] = word[5 + k];
        v->volume_delay_reload[k] = word[10 + k];
    }
    for (uint32_t k = 0; k < RJG_PITCH_SLOTS; k++) {
        v->pitch_count_reload[k] = word[2 + k];
        v->pitch_delta[k] = word[7 + k];
        v->pitch_delay_reload[k] = word[12 + k];
    }
    v->volume_delay[0] = v->volume_delay[1] = 0;
    for (uint32_t k = 0; k < RJG_PITCH_SLOTS; k++) {
        v->pitch_delay[k] = 0;
    }
    v->volume_offset = 0;
    v->pitch_offset = 0;
}

static void select_instrument(const RjgModule* m, RjgVoice* v, uint8_t row) {
    v->instrument = rjg_instrument(m, row);
    v->shown_instrument = row;
}

// A note byte: the voice's channel stops and the instrument's sound is set up for this tick's register write.
static void start_note(const RjgModule* m, RjgVoice* v, uint8_t note) {
    nuade_paula_channel_dma_off(&v->paula);
    v->interrupt_enabled = false;
    point_at_silence(v, sample_pointer(m, m->instruments));
    if (v->envelope != 0) {
        v->envelope_active = true;
        v->envelope_phase = 0;
        v->volume = 0;
        v->envelope_timer = 0;
    }
    uint16_t index = (uint16_t)(note * RJG_PERIODS_PER_NOTE);
    index = (uint16_t)(index + (uint16_t)v->transpose);
    index = (uint16_t)(index + index);
    v->base_period = rjg_u16(m, (int64_t)v->instrument.period_table + (int16_t)index);
    v->trigger = true;
    reload_volume_slots(v);
    v->volume_offset = 0;
    reload_pitch_slots(v);
    v->pitch_offset = 0;
    v->shown_note = (int)nuade_period_to_note(v->base_period);
}

// The sequence ends: the channel goes quiet and the voice stops
static void end_sequence(RjgVoice* v) {
    v->passed_end = true;
    v->interrupt_enabled = false;
    nuade_paula_channel_dma_off(&v->paula);
    v->trigger = false;
}

typedef enum Step {
    Step_Continue, // read the next sequence byte
    Step_Wait,     // the voice waits (a note, a rest or the end)
    Step_Stop,     // the sequence cannot go on
} Step;

// The cursor stays at the largest address once it gets there, so a sequence that starts at the end of the address
// space reads zeros and does not wrap to the start of the module.
static uint8_t read_byte(const RjgModule* m, uint32_t* cursor) {
    const uint8_t byte = rjg_u8(m, *cursor);
    if (*cursor != UINT32_MAX) {
        (*cursor)++;
    }
    return byte;
}

// Runs one command; `cursor` is behind the command byte. The sequence bytes a command adds up are charged to `budget`.
static Step run_command(RjgPlayer* p, RjgVoice* v, Command command, uint32_t* cursor, uint32_t* budget) {
    const RjgModule* m = &p->module;
    switch (command) {
        case Command_Volume:
            v->max_volume = read_byte(m, cursor);
            break;
        case Command_SkipA:
        case Command_SkipB:
        case Command_SkipC:
            (void)read_byte(m, cursor);
            break;
        case Command_Restart:
            *cursor = v->sequence_start;
            v->sequence = *cursor;
            v->passed_end = true;
            break;
        case Command_Duration:
            v->duration = (uint16_t)(read_byte(m, cursor) * p->tempo);
            break;
        case Command_Rest:
            return Step_Wait;
        case Command_Tempo: {
            const uint8_t divisor = read_byte(m, cursor);
            if (divisor == 0) { // the original divides by zero
                return Step_Stop;
            }
            // divu.w: a quotient that does not fit 16 bits leaves the dividend
            const uint32_t quotient = m->tempo_scale / divisor;
            p->tempo = (uint16_t)(quotient > 0xFFFF ? m->tempo_scale : quotient);
            break;
        }
        case Command_DurationSum: {
            // dbra over a count of 0 runs 65536 times
            const uint16_t count = (uint16_t)(read_byte(m, cursor) - 1);
            const uint32_t numbers = (uint32_t)count + 1;
            if (numbers > *budget) {
                return Step_Stop;
            }
            *budget -= numbers;
            uint16_t sum = 0;
            for (uint32_t n = numbers; n > 0; n--) {
                sum = (uint16_t)(sum + read_byte(m, cursor));
            }
            v->duration = (uint16_t)((uint32_t)sum * p->tempo);
            break;
        }
        case Command_Modulation:
            load_modulation(m, v, read_byte(m, cursor));
            break;
        case Command_Flags: {
            const uint8_t flags = read_byte(m, cursor);
            v->volume_slots_reload |= (flags & 1) != 0;
            v->pitch_slots_reload |= (flags & 2) != 0;
            break;
        }
        case Command_End:
            v->sequence_start = 0;
            end_sequence(v);
            *cursor = 0;
            return Step_Wait;
        case Command_Call: {
            const uint8_t number = read_byte(m, cursor);
            if (!stack_push_long(p, v, *cursor)) {
                return Step_Stop;
            }
            *cursor = rjg_sequence_offset(m, number);
            break;
        }
        case Command_Return: {
            *cursor = stack_peek_long(p, v->stack);
            v->stack += STACK_LONG;
            break;
        }
        case Command_TransposeAdd: {
            const int8_t amount = (int8_t)read_byte(m, cursor);
            v->transpose = amount == 0 ? 0 : (int16_t)(v->transpose + amount);
            break;
        }
        case Command_Transpose:
            v->transpose = (int8_t)read_byte(m, cursor);
            break;
        case Command_LoopBegin: {
            const uint8_t count = read_byte(m, cursor);
            const uint32_t restart = count != 0 ? *cursor : 0;
            if (!stack_push_long(p, v, restart) || !stack_push_word(p, v, count)) {
                return Step_Stop;
            }
            break;
        }
        case Command_LoopEnd: {
            const uint32_t restart = stack_peek_long(p, v->stack + STACK_WORD);
            if (restart == 0) {
                v->stack += STACK_LONG + STACK_WORD;
                break;
            }
            *cursor = restart;
            const uint16_t left = (uint16_t)(stack_peek_word(p, v->stack) - 1);
            stack_poke_word(p, v->stack, left);
            if (left == 0) {
                stack_poke_long(p, v->stack + STACK_WORD, 0);
            }
            break;
        }
        case Command_Envelope:
            v->envelope = m->envelopes + (uint32_t)read_byte(m, cursor) * RJG_ENVELOPE_SIZE;
            break;
        case Command_EnvelopeOff:
            v->envelope = 0;
            break;
        case Command_Instrument:
            select_instrument(m, v, read_byte(m, cursor));
            break;
        case Command_Jump: {
            const uint8_t number = read_byte(m, cursor);
            *cursor = rjg_sequence_offset(m, number);
            break;
        }
    }
    return Step_Continue;
}

// A sequence reads bytes until a note, a rest or the end. What the original does with a command byte that is not
// one of the 22 (a jump through whatever follows the table) is undefined: the voice stops, as it does when it runs
// more commands than any song does in one tick.
static void run_sequence(RjgPlayer* p, RjgVoice* v) {
    const RjgModule* m = &p->module;
    uint32_t cursor = v->sequence;
    uint32_t budget = COMMAND_BUDGET;
    while (budget > 0) {
        budget--;
        const uint8_t byte = read_byte(m, &cursor);
        if (byte < RJG_COMMAND_FIRST) {
            start_note(m, v, byte);
            v->sequence = cursor;
            v->wait = v->duration;
            return;
        }
        const uint32_t offset = (uint32_t)byte - RJG_COMMAND_FIRST;
        if ((offset & 3) != 0 || offset / 4 >= RJG_COMMAND_COUNT) {
            break;
        }
        const Step step = run_command(p, v, (Command)(offset / 4), &cursor, &budget);
        if (step == Step_Stop) {
            break;
        }
        if (step == Step_Wait) {
            v->sequence = cursor;
            v->wait = v->duration;
            return;
        }
    }
    v->sequence = 0;
}

// The silent block: the channel plays the empty word, and keeps its registers for the next note.
static void silence_channel(const RjgModule* m, RjgVoice* v) {
    nuade_paula_channel_dma_off(&v->paula);
    v->interrupt_enabled = false;
    point_at_silence(v, sample_pointer(m, m->instruments));
    nuade_paula_channel_dma_on(&v->paula);
}

static void run_voice(RjgPlayer* p, RjgVoice* v) {
    const RjgModule* m = &p->module;
    if (v->sequence == 0) {
        return;
    }
    if (v->wait == 0) {
        if (v->instrument.loop_offset == 0) {
            silence_channel(m, v);
        }
        run_sequence(p, v);
    }
    v->wait--;
    if (v->envelope == 0) {
        run_volume_slots(v);
    }
    run_pitch_slots(v);
    if (v->envelope == 0) {
        v->volume = (uint16_t)((v->max_volume + v->volume_offset) & MODULATION_VOLUME_MASK);
    }
    v->period = (uint16_t)(v->base_period + v->pitch_offset);
}

// --- the volume envelope: attack, decay, sustain until the note is nearly over, then release

static uint8_t envelope_byte(const RjgModule* m, const RjgVoice* v, uint32_t index) {
    return rjg_u8(m, (uint64_t)v->envelope + index);
}

enum {
    ENV_RATE = 0,
    ENV_ATTACK_TICKS,
    ENV_DECAY_RATE,
    ENV_DECAY_TICKS,
    ENV_SUSTAIN,
    ENV_RELEASE_AT,
    ENV_RELEASE_RATE,
    ENV_RELEASE_TICKS
};

static uint16_t replace_low_byte(uint16_t word, uint8_t low) {
    return (uint16_t)((word & 0xFF00) | low);
}

static void envelope_release(const RjgModule* m, RjgVoice* v) {
    v->envelope_phase = 0;
    if (v->volume == 0) {
        return;
    }
    v->envelope_timer = envelope_byte(m, v, ENV_RELEASE_TICKS);
    const uint8_t rate = envelope_byte(m, v, ENV_RELEASE_RATE);
    const uint8_t low = (uint8_t)v->volume;
    v->volume = low < rate ? 0 : replace_low_byte(v->volume, (uint8_t)(low - rate));
}

static void envelope_sustain(const RjgModule* m, RjgVoice* v) {
    v->volume = envelope_byte(m, v, ENV_SUSTAIN);
    if ((int8_t)(uint8_t)v->wait > (int8_t)envelope_byte(m, v, ENV_RELEASE_AT)) {
        return;
    }
    v->envelope_active = false;
}

static void envelope_step(const RjgModule* m, RjgVoice* v) {
    if (!v->envelope_active) {
        envelope_release(m, v);
        return;
    }
    if (v->envelope_phase == 0) {
        v->envelope_timer = envelope_byte(m, v, ENV_ATTACK_TICKS);
        uint16_t level = replace_low_byte(v->volume, (uint8_t)((uint8_t)v->volume + envelope_byte(m, v, ENV_RATE)));
        if (level >= v->max_volume) {
            v->envelope_phase++;
            level = v->max_volume;
        }
        v->volume = level;
        return;
    }
    if (v->envelope_phase == 1) {
        v->envelope_timer = envelope_byte(m, v, ENV_DECAY_TICKS);
        uint16_t level
            = replace_low_byte(v->volume, (uint8_t)((uint8_t)v->volume - envelope_byte(m, v, ENV_DECAY_RATE)));
        if ((uint8_t)level >= ENVELOPE_CEILING) {
            level = 0;
        }
        if ((uint8_t)level >= envelope_byte(m, v, ENV_SUSTAIN)) {
            v->volume = level;
            return;
        }
        v->envelope_phase++;
    }
    envelope_sustain(m, v);
}

static void run_envelope(const RjgModule* m, RjgVoice* v) {
    if (v->sequence == 0 || v->envelope == 0) {
        return;
    }
    if (v->envelope_timer != 0) {
        v->envelope_timer--;
        if (v->envelope_timer != 0) {
            return;
        }
    }
    envelope_step(m, v);
}

// --- the register writes at the end of the tick

static void write_registers(const RjgModule* m, RjgVoice* v) {
    v->paula.period = v->period;
    v->paula.volume = (uint8_t)((int16_t)v->volume < 0 ? 0 : (v->volume & VOLUME_MASK));
    if (!v->trigger) {
        return;
    }
    v->trigger = false;
    v->paula.lc = sample_pointer(m, v->instrument.sample);
    v->paula.len_words = v->instrument.length_words;
    v->interrupt_phase = 2;
    nuade_paula_channel_dma_on(&v->paula);
    if (v->instrument.loops < 0) {
        if (v->instrument.loop_offset == 0) {
            v->interrupt_enabled = false;
            return;
        }
        v->interrupt_phase = 1;
    }
    v->interrupt_enabled = true;
    audio_interrupt(m, v); // the DMA start
}

static void run_driver(RjgPlayer* p) {
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        run_voice(p, &p->voices[i]);
    }
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        run_envelope(&p->module, &p->voices[i]);
    }
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        write_registers(&p->module, &p->voices[i]);
    }
}

// --- starting a sequence on a voice, and the start of a subsong

// The sequence start routine: the voice's record is cleared and its channel is set up with the empty word; the
// instrument 0 defaults go into the registers for the first note.
static void start_voice(RjgPlayer* p, uint8_t number, uint32_t index) {
    const RjgModule* m = &p->module;
    RjgVoice* v = &p->voices[index];
    const uint32_t wraps_seen = v->wraps_seen;
    const uint16_t interrupt_phase = v->interrupt_phase;
    const NuadePaulaChannel paula = v->paula;
    const int shown_note = v->shown_note;
    const uint16_t transpose = (uint16_t)v->transpose;
    *v = (RjgVoice) { .index = index,
                      .wraps_seen = wraps_seen,
                      .interrupt_phase = interrupt_phase,
                      .paula = paula,
                      .shown_note = shown_note,
                      .shown_instrument = 0 };
    if (m->clear_longs + 1 < 32) { // the clear stops short of the transpose word
        v->transpose = (int16_t)transpose;
    }

    nuade_paula_channel_dma_off(&v->paula);
    point_at_silence(v, sample_pointer(m, m->dummy));
    nuade_paula_channel_dma_on(&v->paula);
    v->sequence_start = rjg_sequence_offset(m, number);
    v->sequence = v->sequence_start;
    v->stack = stack_top(index);
    v->instrument = rjg_instrument(m, 0);
    v->base_period = DEFAULT_PERIOD;
    v->period = DEFAULT_PERIOD;
    v->paula.lc = sample_pointer(m, v->instrument.sample);
    v->paula.len_words = v->instrument.length_words;
    v->paula.period = DEFAULT_PERIOD;
    if (m->stop_dma_after_start) {
        nuade_paula_channel_dma_off(&v->paula);
    }
}

// The voices power on as the driver's init leaves them.
static void init_driver(RjgPlayer* p) {
    const RjgModule* m = &p->module;
    memset(p->stack_memory, 0, sizeof(p->stack_memory));
    p->tempo = m->tempo_initial;
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        RjgVoice* v = &p->voices[i];
        *v = (RjgVoice) { .index = i, .interrupt_phase = 2, .shown_note = -1, .shown_instrument = -1 };
        v->paula.volume = 0;
        v->paula.lc = sample_pointer(m, m->dummy);
        v->paula.len_words = INITIAL_LENGTH_WORDS;
        v->paula.period = DEFAULT_PERIOD;
        v->stack = stack_top(i);
    }
}

// The row of a tune in the tune table: the tune number times four, in 16 bits.
static uint64_t tune_row(const RjgModule* m, uint8_t tune) {
    return (uint64_t)m->tunes + (uint16_t)(tune * RJG_TUNE_SIZE);
}

static void start_tune(RjgPlayer* p, uint8_t tune) {
    const RjgModule* m = &p->module;
    const uint64_t row = tune_row(m, tune);
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        const uint8_t number = rjg_u8(m, row + i);
        start_voice(p, number, i);
        if (m->start_kind == RjgStart_Tune && number == 0) {
            break;
        }
    }
}

// --- the player

bool rjg_probe(const uint8_t* data, size_t size) {
    RjgCreateResult created = rjg_create(data, size);
    rjg_destroy(created.player);
    return created.player != nullptr;
}

// A sequence start reads an entry of the sequence table. The offset in it may lie beyond the module (the game's tables
// have such entries for sequences the module does not carry): the cursor then reads zeros.
static bool sequence_entry_fits(const RjgModule* m, uint8_t number) {
    const uint64_t entry = (uint64_t)m->sequences + (uint64_t)number * RJG_SEQUENCE_ENTRY;
    return entry + RJG_SEQUENCE_ENTRY <= m->size;
}

// A subsong's start reads a tune row (or just a sequence number) and the sequence table entries it names.
static bool subsongs_fit(const RjgModule* m) {
    for (uint32_t i = 0; i < m->subsong_count; i++) {
        const uint8_t argument = rjg_subsong_argument(m, i);
        if (m->start_kind == RjgStart_Sequence) {
            if (!sequence_entry_fits(m, argument)) {
                return false;
            }
            continue;
        }
        const uint64_t row = tune_row(m, argument);
        if (row + RJG_TUNE_SIZE > m->size) {
            return false;
        }
        for (uint32_t v = 0; v < RJG_VOICES; v++) {
            if (!sequence_entry_fits(m, rjg_u8(m, row + v))) {
                return false;
            }
        }
    }
    return true;
}

RjgCreateResult rjg_create(const uint8_t* data, size_t size) {
    RjgCreateResult result = { .status = RjgStatus_InvalidData };
    RjgPlayer* p = calloc(1, sizeof(RjgPlayer));
    if (p == nullptr) {
        result.status = RjgStatus_OutOfMemory;
        return result;
    }
    if (!rjg_module_load(&p->module, data, size) || !subsongs_fit(&p->module)) {
        free(p->module.data);
        free(p);
        return result;
    }
    result.player = p;
    result.status = RjgStatus_Ok;
    return result;
}

void rjg_destroy(RjgPlayer* player) {
    if (player != nullptr) {
        free(player->module.data);
        free(player);
    }
}

int rjg_subsong_count(const RjgPlayer* player) {
    return (int)player->module.subsong_count;
}

// What the conversion's InitSong does: the driver's init, then the start routine with the subsong's argument.
static void start_subsong(RjgPlayer* player, uint32_t subsong) {
    const RjgModule* m = &player->module;
    init_driver(player);
    const uint8_t argument = rjg_subsong_argument(m, subsong);
    if (m->start_kind == RjgStart_Sequence) {
        start_voice(player, argument, 0);
    } else {
        start_tune(player, argument);
    }
}

bool rjg_start(RjgPlayer* player, int subsong, NuadeVoices* voices) {
    if (player == nullptr || voices == nullptr || subsong < 0 || (uint32_t)subsong >= player->module.subsong_count) {
        return false;
    }
    start_subsong(player, (uint32_t)subsong);
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        NuadeVoiceState state;
        nuade_voice_stop(voices, i);
        nuade_voice_get_state(voices, i, &state);
        player->voices[i].wraps_seen = state.wraps;
    }
    return true;
}

RjgTickResult rjg_tick(RjgPlayer* player, NuadeVoices* voices) {
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        answer_interrupts(&player->module, &player->voices[i], voices);
    }
    run_driver(player);
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        nuade_paula_channel_sync(&player->voices[i].paula, i, voices);
    }

    // Song end: every voice that plays a sequence has looped or ended
    RjgTickResult result = { 0 };
    bool any = false;
    bool all_passed = true;
    bool all_stopped = true;
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        const RjgVoice* v = &player->voices[i];
        if (v->sequence_start != 0 || v->passed_end) {
            any = true;
            all_passed = all_passed && v->passed_end;
        }
        all_stopped = all_stopped && v->sequence == 0;
    }
    if (any && all_passed) {
        result.looped = true;
        for (uint32_t i = 0; i < RJG_VOICES; i++) {
            player->voices[i].passed_end = false;
        }
    }
    result.ended = all_stopped;
    return result;
}

// An instrument row is a real one when it has a loop flag of -1 or 1 and its sample lies in the module; the rows behind
// the last one are other data of the module (the table is the module's last).
static bool instrument_valid(const RjgModule* m, const RjgInstrument* in) {
    return (in->loops == -1 || in->loops == 1) && in->length_words != 0
           && (uint64_t)in->sample + (uint64_t)in->length_words * 2 <= m->size;
}

int rjg_instrument_count(const RjgPlayer* player) {
    const RjgModule* m = &player->module;
    int count = 0;
    while (count < RJG_MAX_INSTRUMENTS) {
        const RjgInstrument in = rjg_instrument(m, (uint32_t)count);
        if (!instrument_valid(m, &in)) {
            break;
        }
        count++;
    }
    return count;
}

bool rjg_instrument_pcm(const RjgPlayer* player, int n, RjgInstrumentPcm* out) {
    const RjgModule* m = &player->module;
    if (n < 0 || n >= rjg_instrument_count(player)) {
        return false;
    }
    const RjgInstrument in = rjg_instrument(m, (uint32_t)n);
    const uint32_t frames = (uint32_t)in.length_words * 2;
    const int8_t* data = (const int8_t*)m->data + (in.sample & ~1u);
    // the unused rows point at the dummy word (silence), as do the default instrument 0 and some tails of samples
    bool silent = true;
    for (uint32_t i = 0; i < frames && silent; i++) {
        silent = data[i] == 0;
    }
    if (in.length_words <= DUMMY_WORDS || silent) {
        return false;
    }
    *out = (RjgInstrumentPcm) { .data = data, .frames = frames };
    // a looping sound repeats from its loop offset (bytes) to its end
    if (in.loops < 0 && in.loop_offset < frames) {
        out->loop_start = in.loop_offset;
        out->loop_frames = frames - in.loop_offset;
    }
    return true;
}

RjgVoiceState rjg_voice_state(const RjgPlayer* player, int voice) {
    const RjgVoice* v = &player->voices[voice];
    return (RjgVoiceState) { .note = v->shown_note, .instrument = v->shown_instrument };
}

// The first tempo the subsong sets: a copy of the player runs the driver (the sequencers need no channels) until the
// tempo word changes.
uint32_t rjg_grid_ticks(const RjgPlayer* player, int subsong) {
    if (player == nullptr || subsong < 0 || (uint32_t)subsong >= player->module.subsong_count) {
        return 0;
    }
    RjgPlayer scratch = *player;
    start_subsong(&scratch, (uint32_t)subsong);
    const uint16_t initial = scratch.tempo;
    for (uint32_t tick = 0; tick < GRID_SCAN_TICKS && scratch.tempo == initial; tick++) {
        run_driver(&scratch);
    }
    return scratch.tempo != 0 && scratch.tempo <= GRID_MAX_TICKS ? scratch.tempo : 1;
}
