// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Daniel Collin
// Ported from NostalgicPlayer C# implementation by Thomas Neumann (MIT)
//
// SidMon 1.0 player. The replayer is an interrupt routine that walks four sequences of tracks and plays synthesised
// waveforms or samples on the four Paula channels. The tick follows the replayer's code, including its byte wraps, the
// order the voices run in and when the loop registers of a sample are written.
//
// The replayer works on the module in memory (it rewrites waveforms and instruments), so the player keeps a working
// copy of the module and every offset below is an offset into it.

#include "sd1_internal.h"

#include "nuade_base.h"
#include "nuade_paula_channel.h"
#include "nuade_tracker.h"
#include "nuade_utils.h"
#include <stdlib.h>
#include <string.h>

#define FIRST_SAMPLE_WAVEFORM 16
#define FIRST_SPECIAL_INSTRUMENT 60
#define WAVE_JUMP 0xFF
#define WAVE_LIST_END 16
#define NO_LOOP_ADDRESS 0xFFFFFFFFu
#define INITIAL_PERIOD 0x9999
#define SYNTH_LENGTH_WORDS 16
#define SILENCE_WORDS 2

// Track row effects that are not a bend
#define EFFECT_ATTACK 0
#define EFFECT_SPEED 2
#define EFFECT_ROWS 3

// The replayer calls its voice routine for channel 0, 1, 3, 2.
static const int k_voice_order[SD1_VOICES] = { 0, 1, 3, 2 };

bool sd1_probe(const uint8_t* data, size_t size) {
    if (data == nullptr || size < MIN_MODULE_SIZE || size > UINT32_MAX / 2) {
        return false;
    }
    Image m = { .bytes = nuade_paula_padded_copy(data, size), .size = (uint32_t)size };
    if (m.bytes == nullptr) {
        return false;
    }
    Sd1Layout layout;
    const bool ok = sd1_layout_from_module(&m, &layout);
    free(m.bytes);
    return ok;
}

Sd1CreateResult sd1_create(const uint8_t* data, size_t size) {
    Sd1CreateResult result = { .status = Sd1Status_InvalidData };
    if (data == nullptr || size < MIN_MODULE_SIZE || size > UINT32_MAX / 2) {
        return result;
    }
    Sd1Player* p = calloc(1, sizeof(Sd1Player));
    if (p == nullptr) {
        result.status = Sd1Status_OutOfMemory;
        return result;
    }
    p->module.size = (uint32_t)size;
    p->module.memory = nuade_paula_padded_copy(data, size);
    p->memory = nuade_paula_padded_copy(data, size);
    if (p->module.memory == nullptr || p->memory == nullptr) {
        sd1_destroy(p);
        result.status = Sd1Status_OutOfMemory;
        return result;
    }
    const Image m = { .bytes = p->module.memory, .size = p->module.size };
    if (!sd1_layout_from_module(&m, &p->module.layout)) {
        sd1_destroy(p);
        return result;
    }
    result.player = p;
    result.status = Sd1Status_Ok;
    return result;
}

void sd1_destroy(Sd1Player* player) {
    if (player == nullptr) {
        return;
    }
    free(player->module.memory);
    free(player->memory);
    free(player);
}

// ----------------------------------------------------------------------------------------------------------------
// The song

static Image memory_image(const Sd1Player* p) {
    return (Image) { .bytes = p->memory, .size = p->module.size };
}

// An address as Paula reads it: inside the module its bytes, past it the zeros that follow.
static const int8_t* chip_pointer(const Sd1Player* p, uint32_t offset) {
    return (const int8_t*)p->memory + (offset < p->module.size ? offset : p->module.size);
}

// A byte of the period table, as the replayer reads it: `index` words from its start. The table is followed by the
// rest of the module, which an index past it reads.
static uint16_t period_word(const Sd1Player* p, uint32_t index) {
    const Image m = memory_image(p);
    return image_u16(&m, p->module.layout.periods + index * 2);
}

// The place of a note in the period table: the oldest replayers index it by the note itself, later ones from the entry
// after it.
static uint32_t note_table_index(const Sd1Player* p, uint8_t note) {
    return note + (p->module.layout.old_replayer ? 0u : 1u);
}

// The offset the instrument's fine tune adds to a period index, in words. One replayer builds the byte offset in a
// 16-bit register and adds it signed, so a fine tune from 241 on reads the table backwards.
static uint32_t instrument_fine_tune(const Sd1Player* p, uint32_t instrument) {
    const Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    const uint32_t fine_tune = image_u8(&m, instrument + INSTRUMENT_FINE_TUNE);
    if (l->finetune_is_word) {
        return (uint32_t)((int32_t)(int16_t)(uint16_t)(fine_tune * l->finetune_stride * 2u) / 2);
    }
    return fine_tune * l->finetune_stride;
}

static uint32_t sequence_entry(const Sd1Player* p, int voice, int sequence) {
    return p->module.layout.sequences[voice] + (uint32_t)sequence * SEQUENCE_ENTRY_SIZE;
}

// The first row of the track a sequence entry names.
static uint32_t track_start(const Sd1Player* p, uint32_t entry) {
    const Image m = memory_image(p);
    return sd1_track_start(&m, &p->module.layout, entry);
}

// What one run of the voice routine leaves for the register writes at its end.
typedef struct VoiceOutput {
    uint32_t start;  // AUDxLC; 0 when not written
    uint16_t period; // written when not 0
    uint16_t volume; // 8 bit volume of the envelope, written when not 0 as volume / 4
    uint16_t length; // AUDxLEN in words; written when not 0
    bool sample;     // this run started a sample
} VoiceOutput;

static uint32_t waveform_address(const Sd1Player* p, uint32_t number) {
    return p->module.layout.waveforms + (number - 1) * WAVEFORM_SIZE;
}

static void dma_off(Sd1Voice* v) {
    nuade_paula_channel_dma_off(&v->paula);
}

// A sample instrument starts: its data is played once from the start, then the loop registers follow two
// interrupts later (run_loop_registers).
static void start_sample(Sd1Player* p, Sd1Voice* v, VoiceOutput* out, uint32_t waveform) {
    const Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    if (v->sample_state != 0) {
        dma_off(v);
    }
    const uint32_t entry = l->samples + 4 + (waveform - FIRST_SAMPLE_WAVEFORM) * SAMPLE_ENTRY_SIZE;
    const uint32_t data = l->samples + image_u32(&m, l->samples) + 4;
    const uint32_t start = image_u32(&m, entry + SAMPLE_START);
    const uint32_t end = image_u32(&m, entry + SAMPLE_END);
    out->start = data + start;
    out->sample = true;
    v->loop_start = image_u32(&m, entry + SAMPLE_LOOP) == l->no_loop ? NO_LOOP_ADDRESS
                                                                     : data + image_u32(&m, entry + SAMPLE_LOOP);
    v->loop_length = (uint32_t)((int32_t)(data + end - v->loop_start) >> 1);
    out->length = (uint16_t)((int32_t)(end - start) >> 1);
    v->sample_state = 1;
}

// The effect of a row that starts a note.
static void start_note_effect(Sd1Player* p, Sd1Voice* v, uint32_t row, int8_t transpose) {
    Image m = memory_image(p);
    Sd1Song* s = &p->song;
    const uint8_t parameter = image_u8(&m, row + 3);
    switch (image_u8(&m, row + 2)) {
        case EFFECT_ATTACK:
            if (parameter != 0) {
                image_set_u8(&m, v->instrument + INSTRUMENT_ATTACK_SPEED, parameter);
                image_set_u8(&m, v->instrument + INSTRUMENT_ATTACK_MAX, parameter);
                v->wave_speed = 0;
            }
            break;
        case EFFECT_SPEED:
            s->speed = (s->speed & ~0xFFu) | parameter;
            v->wave_speed = 0;
            break;
        case EFFECT_ROWS:
            s->rows = (s->rows & ~0xFFu) | parameter;
            v->wave_speed = 0;
            break;
        default:
            v->bend_target = (uint8_t)(image_u8(&m, row + 2) + transpose);
            v->bend_speed = (int8_t)parameter;
            break;
    }
    v->phase_speed = image_u8(&m, v->instrument + INSTRUMENT_PHASE_SPEED);
}

// A row that holds a note (not 0, not a release): its period and a fresh envelope.
static void start_note(Sd1Player* p, Sd1Voice* v, VoiceOutput* out, int voice, uint32_t row) {
    const Image m = memory_image(p);
    const int8_t transpose = image_s8(&m, sequence_entry(p, voice, v->sequence) + SEQUENCE_TRANSPOSE);
    const uint8_t index = (uint8_t)(image_u8(&m, row) + (uint8_t)transpose);
    v->shown_note = sd1_note_semitone(p, index);
    v->period = period_word(p, instrument_fine_tune(p, v->instrument) + note_table_index(p, index));
    v->note_offset = (uint8_t)(index * 2u);
    out->period = v->period;
    v->envelope = Sd1Envelope_Attack;
    v->volume = 0;
    v->sustain_count = 0;
    v->pitch_control = 0;
    v->pitch_fall_control = 0;
    v->bend_speed = 0;
    start_note_effect(p, v, row, transpose);
}

// The row of a voice: an instrument and/or a note.
static void play_row(Sd1Player* p, Sd1Voice* v, VoiceOutput* out, int voice) {
    const Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    const uint32_t row = v->row_pointer;
    const uint8_t note = image_u8(&m, row);
    const uint8_t instrument_number = image_u8(&m, row + 1);
    const uint8_t duration = image_u8(&m, row + 4);

    if (instrument_number != 0) {
        const uint32_t instrument = l->instruments + (uint32_t)(instrument_number - 1) * INSTRUMENT_SIZE;
        out->length = SYNTH_LENGTH_WORDS;
        if (v->sample_state != 0) {
            dma_off(v);
        }
        v->sample_state = 0;
        const uint32_t waveform = image_u32(&m, instrument + INSTRUMENT_WAVEFORM);
        if ((uint8_t)waveform >= FIRST_SAMPLE_WAVEFORM) {
            start_sample(p, v, out, waveform);
        }
        const uint32_t entry = l->wave_list + (uint32_t)(uint8_t)(waveform - 1) * WAVE_LIST_ENTRY_SIZE;
        v->wave_index = 0;
        v->waveform = (uint8_t)waveform;
        v->wave_speed = image_u8(&m, entry + 1);
        if (!out->sample) {
            out->start = waveform_address(p, image_u8(&m, entry));
        }
        v->instrument = instrument;
        v->envelope = Sd1Envelope_Attack;
        v->pitch_fall_control = 0;
        v->pitch_control = 0;
        v->row_count = duration;
        v->shown_instrument = instrument_number;
    } else if (note != 0) {
        // A note on the current instrument; a sample plays again
        v->row_count = duration;
        if (v->sample_state != 0) {
            start_sample(p, v, out, v->waveform);
        }
    }

    if (note != 0) {
        v->row_count = duration;
        if (note != SD1_NOTE_RELEASE) {
            start_note(p, v, out, voice, row);
        }
    }
    v->row_pointer += TRACK_ROW_SIZE;
}

// The volume envelope: attack to the maximum, decay to the minimum, sustain for a count of ticks, release.
static void run_envelope(const Sd1Player* p, Sd1Voice* v, VoiceOutput* out) {
    const Image m = memory_image(p);
    const uint32_t i = v->instrument;
    switch (v->envelope) {
        case Sd1Envelope_Attack: {
            const uint16_t volume = (uint16_t)(v->volume + image_u8(&m, i + INSTRUMENT_ATTACK_SPEED));
            const uint8_t maximum = image_u8(&m, i + INSTRUMENT_ATTACK_MAX);
            out->volume = volume;
            v->volume = (uint8_t)volume;
            if (volume > maximum) {
                v->envelope = Sd1Envelope_Decay;
                out->volume = maximum;
                v->volume = maximum;
            }
            break;
        }
        case Sd1Envelope_Decay: {
            const uint16_t volume = (uint16_t)(v->volume - image_u8(&m, i + INSTRUMENT_DECAY_SPEED));
            const uint8_t minimum = image_u8(&m, i + INSTRUMENT_DECAY_MIN);
            out->volume = volume;
            v->volume = (uint8_t)volume;
            if (volume <= minimum || volume >= 0xFF00) {
                v->envelope = Sd1Envelope_Sustain;
                out->volume = minimum;
                v->volume = minimum;
                v->sustain_count = image_u8(&m, i + INSTRUMENT_SUSTAIN_TIME);
            }
            break;
        }
        case Sd1Envelope_Sustain:
            out->volume = v->volume;
            v->sustain_count--;
            if (v->sustain_count == 0) {
                v->envelope = Sd1Envelope_Release;
            }
            break;
        case Sd1Envelope_Release: {
            const uint16_t volume = (uint16_t)(v->volume - image_u8(&m, i + INSTRUMENT_RELEASE_SPEED));
            const uint8_t minimum = image_u8(&m, i + INSTRUMENT_RELEASE_MIN);
            out->volume = volume;
            v->volume = (uint8_t)volume;
            if (volume <= minimum || volume >= 0xFF00) {
                v->envelope = Sd1Envelope_Done;
                out->volume = minimum;
                v->volume = minimum;
            }
            break;
        }
        case Sd1Envelope_Done:
            break;
    }
}

// The period of the note, stepped through the instrument's 16 arpeggio entries one per tick. The note offset and the
// sum are bytes: a note past the end of the table wraps.
static void run_arpeggio(const Sd1Player* p, Sd1Voice* v, VoiceOutput* out) {
    const Image m = memory_image(p);
    v->arpeggio_index = (uint8_t)((v->arpeggio_index + 1) & 0x0F);
    const uint8_t step = image_u8(&m, v->instrument + INSTRUMENT_ARPEGGIO + v->arpeggio_index);
    const uint8_t offset = (uint8_t)(v->note_offset + step * 2u);
    v->period = period_word(p, instrument_fine_tune(p, v->instrument) + offset / 2u);
    out->period = v->period;
}

// Slide towards the bend target note; the speed is subtracted from the accumulated slide every tick.
static void run_bend(const Sd1Player* p, Sd1Voice* v) {
    if (v->bend_speed == 0) {
        return;
    }
    const int16_t step = (int16_t)(int8_t)(uint8_t)(-v->bend_speed);
    const uint16_t target = period_word(p, instrument_fine_tune(p, v->instrument) + v->bend_target);
    v->pitch_control = (uint16_t)(v->pitch_control + step);
    v->period = (uint16_t)(v->period + v->pitch_control);
    const bool reached = step < 0 ? target >= v->period : target <= v->period;
    if (reached) {
        v->note_offset = (uint8_t)(v->bend_target * 2u);
        v->period = target;
        v->pitch_control = 0;
        v->bend_speed = 0;
    }
}

// The waveform the instrument names offsets the period by a quarter of its bytes.
static void run_phase(const Sd1Player* p, Sd1Voice* v) {
    const Image m = memory_image(p);
    const uint8_t shift = image_u8(&m, v->instrument + INSTRUMENT_PHASE_SHIFT);
    if (shift == 0) {
        return;
    }
    if (v->phase_speed != 0) {
        v->phase_speed--;
        return;
    }
    v->phase_index = (uint8_t)((v->phase_index + 1) & 0x1F);
    const uint32_t wave = p->module.layout.waveforms + (uint32_t)(uint8_t)(shift - 1) * WAVEFORM_SIZE;
    v->period = (uint16_t)(v->period + (image_s8(&m, wave + v->phase_index) >> 2));
}

static void run_pitch_fall(const Sd1Player* p, Sd1Voice* v) {
    const Image m = memory_image(p);
    v->pitch_fall_control = (uint16_t)(v->pitch_fall_control - image_s8(&m, v->instrument + INSTRUMENT_PITCH_FALL));
    v->period = (uint16_t)(v->period + v->pitch_fall_control);
}

// The wave list steps through waveforms: each entry holds a waveform and the ticks to wait; 0xFF jumps.
static void run_wave_list(const Sd1Player* p, Sd1Voice* v, VoiceOutput* out) {
    if (v->sample_state != 0) {
        return;
    }
    if (v->wave_speed != 0) {
        v->wave_speed--;
        return;
    }
    if (v->wave_index == WAVE_LIST_END) {
        return;
    }
    const Image m = memory_image(p);
    const uint32_t entry
        = p->module.layout.wave_list + (uint32_t)(uint8_t)(v->waveform - 1) * WAVE_LIST_ENTRY_SIZE + v->wave_index;
    if (image_u8(&m, entry) == WAVE_JUMP) {
        v->wave_index = image_u8(&m, entry + 1) & 0xFE;
        return;
    }
    out->start = p->module.layout.waveforms + (uint32_t)(uint8_t)(image_u8(&m, entry) - 1) * WAVEFORM_SIZE;
    v->wave_speed = image_u8(&m, entry + 1);
    v->wave_index = (uint8_t)(v->wave_index + 2);
}

// The registers a voice routine wrote, then DMA on for every channel. A voice that has not chosen an instrument yet
// plays a waveform from wherever its instrument pointer leads, silently (its volume is 0): that is not started.
static void write_registers(Sd1Player* p, Sd1Voice* v, const VoiceOutput* out) {
    if (out->start != 0 && v->shown_instrument != 0) {
        v->paula.lc = chip_pointer(p, out->start);
    }
    if (out->period != 0) {
        v->paula.period = v->period;
    }
    if (out->volume != 0) {
        v->paula.volume = (uint8_t)((int16_t)out->volume >> 2);
    }
    if (out->length != 0) {
        v->paula.len_words = out->length;
    }
    for (int i = 0; i < SD1_VOICES; i++) {
        nuade_paula_channel_dma_on(&p->song.voices[i].paula);
    }
}

// ----------------------------------------------------------------------------------------------------------------
// The oldest replayer: the waveform comes straight from the instrument, there are no samples but three fixed ones on
// voice 3, and a row is a note with a bend (its effect byte is the target note).

static void run_pitch_fall_old(const Sd1Player* p, Sd1Voice* v) {
    const Image m = memory_image(p);
    // The fine tune and pitch fall bytes of a later instrument are one word here
    const int16_t fall = (int16_t)image_u16(&m, v->instrument + INSTRUMENT_FINE_TUNE);
    v->pitch_fall_control = (uint16_t)(v->pitch_fall_control - fall);
    v->period = (uint16_t)(v->period + v->pitch_fall_control);
}

// An instrument from number 61 on starts one of the special samples; only voice 3 plays them.
static void start_special_sample(Sd1Player* p, uint8_t instrument_index) {
    Sd1Voice* v = &p->song.voices[SD1_VOICES - 1];
    const Sd1Layout* l = &p->module.layout;
    dma_off(v);
    const unsigned sample = instrument_index - FIRST_SPECIAL_INSTRUMENT;
    if (sample < SD1_SPECIAL_SAMPLES) {
        v->paula.lc = chip_pointer(p, l->special_samples[sample].start);
        v->paula.len_words = l->special_samples[sample].length_words;
        v->paula.volume = 64;
    }
}

// The row of a voice. False when the replayer's voice routine ends there: a special sample on another voice.
static bool play_row_old(Sd1Player* p, Sd1Voice* v, VoiceOutput* out, int voice) {
    const Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    const uint32_t row = v->row_pointer;
    const uint8_t note = image_u8(&m, row);
    const uint8_t instrument_number = image_u8(&m, row + 1);
    const uint8_t duration = image_u8(&m, row + 4);

    if (instrument_number != 0) {
        const uint8_t index = (uint8_t)(instrument_number - 1);
        if (p->song.special_samples && index >= FIRST_SPECIAL_INSTRUMENT) {
            if (voice != SD1_VOICES - 1) {
                return false;
            }
            start_special_sample(p, index);
        } else {
            const uint32_t instrument = l->instruments + (uint32_t)index * INSTRUMENT_SIZE;
            out->start = waveform_address(p, image_u32(&m, instrument + INSTRUMENT_WAVEFORM));
            v->instrument = instrument;
            v->envelope = Sd1Envelope_Attack;
            v->pitch_fall_control = 0;
            v->pitch_control = 0;
            v->shown_instrument = instrument_number;
        }
    }
    if (note == SD1_NOTE_RELEASE) {
        v->row_count = duration;
    } else if (note != 0) {
        const int8_t transpose = image_s8(&m, sequence_entry(p, voice, v->sequence) + SEQUENCE_TRANSPOSE);
        const uint8_t index = (uint8_t)(note + (uint8_t)transpose);
        v->shown_note = sd1_note_semitone(p, index);
        v->period = period_word(p, note_table_index(p, index));
        v->note_offset = (uint8_t)(index * 2u);
        out->period = v->period;
        if (p->song.special_samples && voice == SD1_VOICES - 1) {
            // The special sample restarts with the note
            dma_off(v);
            v->paula.period = v->period;
        }
        v->envelope = Sd1Envelope_Attack;
        v->volume = 0;
        v->sustain_count = 0;
        v->pitch_control = 0;
        v->pitch_fall_control = 0;
        v->bend_target = (uint8_t)(image_u8(&m, row + 2) + (uint8_t)transpose);
        v->bend_speed = (int8_t)image_u8(&m, row + 3);
        v->row_count = duration;
        v->phase_speed = image_u8(&m, v->instrument + INSTRUMENT_PHASE_SPEED);
    }
    v->row_pointer += TRACK_ROW_SIZE;
    return true;
}

// The registers of a voice; voice 3 writes none while the special samples are in use, and DMA stays on.
static void write_registers_old(Sd1Player* p, Sd1Voice* v, const VoiceOutput* out, int voice) {
    Sd1Song* s = &p->song;
    if (!(s->special_samples && voice == SD1_VOICES - 1)) {
        if (out->start != 0 && v->shown_instrument != 0) {
            v->paula.lc = chip_pointer(p, out->start);
        }
        if (out->period != 0 || out->volume != 0) {
            v->paula.period = v->period;
        }
        if (out->volume != 0) {
            v->paula.volume = (uint8_t)((int16_t)out->volume >> 2);
        }
    }
    if (s->special_samples) {
        s->special_pending = true;
    }
}

// One voice for one tick (the replayer's note routine).
static void run_voice(Sd1Player* p, int voice) {
    Sd1Song* s = &p->song;
    Sd1Voice* v = &s->voices[voice];
    const Sd1Layout* l = &p->module.layout;
    VoiceOutput out = { 0 };

    if (s->speed_counter == 0) {
        if (s->new_position) {
            if (l->clears_row_count) {
                v->row_count = 0;
            }
            v->sequence = s->loop_song ? 0 : v->sequence + 1;
            s->looped = s->looped || s->loop_song;
            v->row_pointer = track_start(p, sequence_entry(p, voice, v->sequence));
        }
        if (v->row_count != 0) {
            v->row_count--;
        } else if (l->old_replayer) {
            if (!play_row_old(p, v, &out, voice)) {
                return;
            }
        } else {
            play_row(p, v, &out, voice);
        }
    }

    run_envelope(p, v, &out);
    run_arpeggio(p, v, &out);
    run_bend(p, v);
    run_phase(p, v);
    if (l->old_replayer) {
        run_pitch_fall_old(p, v);
        write_registers_old(p, v, &out, voice);
    } else {
        run_pitch_fall(p, v);
        run_wave_list(p, v, &out);
        write_registers(p, v, &out);
    }
}

// Waveform 1 and 2 of the song parameters are blended into a destination waveform, every `speed` ticks, with the
// second source rotated by one more each time.
static void run_mixers(Sd1Player* p) {
    Image m = memory_image(p);
    Sd1Song* s = &p->song;
    const Sd1Layout* l = &p->module.layout;
    for (int k = 0; k < 2; k++) {
        const uint32_t speed = image_u32(&m, l->song + SONG_MIX_SPEED + (uint32_t)k * 4);
        if (speed == 0) {
            continue;
        }
        if (s->mix_counter[k] == 0) {
            s->mix_counter[k] = speed;
            s->mix_position[k] = (s->mix_position[k] + 1) & 0x1F;
            const uint32_t source_1 = waveform_address(p, image_u32(&m, l->song + SONG_MIX_SOURCE_1 + (uint32_t)k * 4));
            const uint32_t source_2 = waveform_address(p, image_u32(&m, l->song + SONG_MIX_SOURCE_2 + (uint32_t)k * 4));
            const uint32_t destination
                = waveform_address(p, image_u32(&m, l->song + SONG_MIX_DESTINATION + (uint32_t)k * 4));
            uint32_t rotation = s->mix_position[k];
            for (int i = WAVEFORM_SIZE - 1; i >= 0; i--) {
                const int a = (uint8_t)(image_u8(&m, source_1 + (uint32_t)i) + 0x80);
                const int b = (uint8_t)(image_u8(&m, source_2 + rotation) + 0x80);
                image_set_u8(&m, destination + (uint32_t)i, (uint8_t)(((a + b) >> 1) - 0x80));
                rotation = (rotation - 1) & 0x1F;
            }
        }
        s->mix_counter[k]--;
    }
}

// The filter flips one byte of the first waveform every tick.
static void run_filter(Sd1Player* p) {
    Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    const uint32_t position = l->filter_shares_position ? p->song.mix_position[0] : p->song.filter_position;
    const uint32_t at = l->waveforms + position;
    image_set_u8(&m, at, (uint8_t)-image_s8(&m, at));
}

// A sample's loop registers are written two interrupts after it started, once its first pass is under way.
static void run_loop_registers(Sd1Player* p) {
    for (int i = 0; i < SD1_VOICES; i++) {
        Sd1Voice* v = &p->song.voices[i];
        if (v->sample_state == 1) {
            v->sample_state = 2;
        } else if (v->sample_state == 2) {
            v->sample_state = 3;
            if (v->loop_start != NO_LOOP_ADDRESS) {
                v->paula.lc = chip_pointer(p, v->loop_start);
                v->paula.len_words = (uint16_t)v->loop_length;
            } else {
                v->paula.lc = chip_pointer(p, p->module.size);
                v->paula.len_words = SILENCE_WORDS;
            }
        }
    }
}

// The position counters move on at the end of the interrupt, when the speed counter wrapped.
static void advance_position(Sd1Player* p) {
    Sd1Song* s = &p->song;
    const Image m = memory_image(p);
    s->speed_counter++;
    if (s->speed < s->speed_counter) {
        s->speed_counter = 0;
    }
    s->new_position = false;
    s->loop_song = false;
    if (s->speed_counter != 0) {
        return;
    }
    s->row++;
    if (s->row == s->rows) {
        s->row = 0;
        s->new_position = true;
        s->position++;
        if (image_u32(&m, p->module.layout.song + SONG_POSITIONS) <= s->position) {
            s->position = 1;
            s->loop_song = true;
        }
    }
}

static void init_voice(Sd1Player* p, int voice) {
    const Image m = memory_image(p);
    const Sd1Layout* l = &p->module.layout;
    Sd1Voice* v = &p->song.voices[voice];
    *v = (Sd1Voice) { .sequence = 0, .period = INITIAL_PERIOD, .envelope = Sd1Envelope_Done, .shown_note = -1 };
    v->row_pointer = track_start(p, sequence_entry(p, voice, 0));
    const uint8_t first = image_u8(&m, v->row_pointer + 1);
    v->instrument = l->instruments + (uint32_t)(uint8_t)(first - 1) * INSTRUMENT_SIZE;
    v->paula.len_words = SYNTH_LENGTH_WORDS;
    v->paula.volume = 0;
}

bool sd1_start(Sd1Player* player, NuadeVoices* voices) {
    if (player == nullptr || voices == nullptr) {
        return false;
    }
    Sd1Song* s = &player->song;
    const Sd1Layout* l = &player->module.layout;
    memcpy(player->memory, player->module.memory, (size_t)player->module.size + NUADE_PAULA_MAX_SAMPLE_BYTES);
    const Image m = memory_image(player);
    *s = (Sd1Song) { 0 };
    s->speed = image_u32(&m, l->song + SONG_SPEED);
    if (l->init_starts_playing) {
        s->speed_counter = s->speed;
    } else {
        // The eagleplayer runs the interrupt without the module's start routine, so the speed counter and the
        // flags keep what the module file holds.
        s->speed_counter = image_u32(&m, l->counter);
        s->new_position = image_u32(&m, l->new_position_flag) == 1;
        s->loop_song = image_u32(&m, l->loop_flag) == 1;
    }
    s->rows = image_u32(&m, l->song + SONG_ROWS);
    s->row = UINT32_MAX;
    s->position = 1;
    for (int k = 0; k < 2; k++) {
        if (l->mixing) {
            s->mix_counter[k] = image_u32(&m, l->mix_counter[k]);
            s->mix_position[k] = image_u32(&m, l->mix_position[k]);
        }
    }
    if (l->filter) {
        s->filter_position = image_u32(&m, l->filter_position);
    }
    s->special_samples = l->old_replayer && image_u32(&m, l->special_flag) == 1;
    for (int i = 0; i < SD1_VOICES; i++) {
        init_voice(player, i);
        nuade_voice_stop(voices, (uint32_t)i);
    }
    for (int i = 0; i < SD1_VOICES; i++) {
        nuade_paula_channel_dma_on(&s->voices[i].paula);
    }
    return true;
}

Sd1TickResult sd1_tick(Sd1Player* player, NuadeVoices* voices) {
    Sd1Song* s = &player->song;
    s->looped = false;
    const Sd1Layout* l = &player->module.layout;
    if (l->old_replayer) {
        // The interrupt starts by switching DMA on; a special sample that was started waits for it
        for (int i = 0; i < SD1_VOICES; i++) {
            nuade_paula_channel_dma_on(&s->voices[i].paula);
        }
        if (s->special_pending) {
            s->voices[SD1_VOICES - 1].paula.len_words = SILENCE_WORDS;
        }
    }
    for (int i = 0; i < SD1_VOICES; i++) {
        run_voice(player, k_voice_order[i]);
    }
    advance_position(player);
    if (l->mixing) {
        run_mixers(player);
    }
    if (l->filter) {
        run_filter(player);
    }
    if (!l->old_replayer) {
        run_loop_registers(player);
    }
    for (int i = 0; i < SD1_VOICES; i++) {
        nuade_paula_channel_sync(&s->voices[i].paula, (uint32_t)i, voices);
    }
    return (Sd1TickResult) { .looped = s->looped };
}

Sd1Position sd1_position(const Sd1Player* player) {
    const Sd1Song* s = &player->song;
    return (Sd1Position) { .order = (int)s->position - 1,
                           .row = s->row == UINT32_MAX ? 0 : (int)s->row,
                           .rows = (int)s->rows,
                           .speed = (int)s->speed };
}

Sd1VoiceState sd1_voice_state(const Sd1Player* player, int voice) {
    const Sd1Voice* v = &player->song.voices[voice];
    return (Sd1VoiceState) { .note = v->shown_note, .instrument = v->shown_instrument };
}

int sd1_instrument_count(const Sd1Player* player) {
    return player->module.layout.instrument_count;
}

int sd1_note_semitone(const Sd1Player* player, int note) {
    const uint16_t period = period_word(player, note_table_index(player, (uint8_t)note));
    return period != 0 ? nuade_period_to_note_shifted(period, SD1_NOTE_OCTAVE_SHIFT) : -1;
}

bool sd1_sample_name(const Sd1Player* player, int instrument, char out[SD1_SAMPLE_NAME_SIZE + 1]) {
    const Sd1Layout* l = &player->module.layout;
    const Image m = { .bytes = player->module.memory, .size = player->module.size };
    if (l->old_replayer || instrument < 1 || instrument > l->instrument_count) {
        return false;
    }
    const uint32_t waveform = image_u32(&m, l->instruments + (uint32_t)(instrument - 1) * INSTRUMENT_SIZE);
    if (waveform < FIRST_SAMPLE_WAVEFORM || waveform >= FIRST_SAMPLE_WAVEFORM + (uint32_t)l->sample_count) {
        return false;
    }
    const uint32_t name = l->samples + 4 + (waveform - FIRST_SAMPLE_WAVEFORM) * SAMPLE_ENTRY_SIZE + SAMPLE_NAME;
    for (int i = 0; i < SD1_SAMPLE_NAME_SIZE; i++) {
        out[i] = (char)image_u8(&m, name + (uint32_t)i);
    }
    out[SD1_SAMPLE_NAME_SIZE] = '\0';
    return true;
}

int sd1_order_count(const Sd1Player* player) {
    return player->module.layout.position_count;
}

Sd1Cell sd1_pattern_cell(const Sd1Player* player, int order, int voice, int row) {
    const Image m = memory_image(player);
    Sd1Cell cell = { 0 };
    if (order < 0 || order >= player->module.layout.position_count || voice < 0 || voice >= SD1_VOICES || row < 0) {
        return cell;
    }
    uint32_t at = track_start(player, sequence_entry(player, voice, order));
    for (int line = 0; line < ROWS_PER_TRACK && line <= row; at += TRACK_ROW_SIZE) {
        const uint8_t duration = image_u8(&m, at + 4);
        if (line == row) {
            cell = (Sd1Cell) { .note = image_u8(&m, at),
                               .instrument = image_u8(&m, at + 1),
                               .effect = image_u8(&m, at + 2),
                               .parameter = image_u8(&m, at + 3),
                               .duration = duration };
            break;
        }
        line += duration + 1;
    }
    return cell;
}
