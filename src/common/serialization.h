// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <bitsery/adapter/buffer.h>
#include <bitsery/ext/pointer.h>
#include <bitsery/ext/std_optional.h>
#include <bitsery/ext/std_variant.h>
#include <bitsery/traits/array.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <vestige/aeffectx.h>

#include <variant>

#include "vst24.h"

// These constants are limits used by bitsery

/**
 * The maximum number of audio channels supported.
 */
constexpr size_t max_audio_channels = 32;
/**
 * The maximum number of samples in a buffer.
 */
constexpr size_t max_buffer_size = 16384;
/**
 * The maximum number of MIDI events in a single `VstEvents` struct.
 */
constexpr size_t max_midi_events = max_buffer_size / sizeof(size_t);
/**
 * The maximum size in bytes of a string or buffer passed through a void pointer
 * in one of the dispatch functions. This is used to create buffers for plugins
 * to write strings to.
 */
[[maybe_unused]] constexpr size_t max_string_length = 64;

/**
 * The size for a buffer in which we're receiving chunks. Allow for up to 50 MB
 * chunks. Hopefully no plugin will come anywhere near this limit, but it will
 * add up when plugins start to audio samples in their presets.
 */
constexpr size_t binary_buffer_size = 50 << 20;

// The plugin should always be compiled to a 64-bit version, but the host
// application can also be 32-bit to allow using 32-bit legacy Windows VST in a
// modern Linux VST host. Because of this we have to make sure to always use
// 64-bit integers in places where we would otherwise use `size_t` and
// `intptr_t`. Otherwise the binary serialization would break. The 64 <-> 32 bit
// conversion for the 32-bit host application won't cause any issues for us
// since we can't directly pass pointers between the plugin and the host anyway.

#ifndef __WINE__
// Sanity check for the plugin, both the 64 and 32 bit hosts should follow these
// conventions
static_assert(std::is_same_v<size_t, uint64_t>);
static_assert(std::is_same_v<intptr_t, int64_t>);
#endif
using native_size_t = uint64_t;
using native_intptr_t = int64_t;

// The cannonical overloading template for `std::visitor`, not sure why this
// isn't part of the standard library
template <class... Ts>
struct overload : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overload(Ts...) -> overload<Ts...>;

/**
 * The serialization function for `AEffect` structs. This will s serialize all
 * of the values but it will not touch any of the pointer fields. That way you
 * can deserialize to an existing `AEffect` instance.
 */
template <typename S>
void serialize(S& s, AEffect& plugin) {
    s.value4b(plugin.magic);
    s.value4b(plugin.numPrograms);
    s.value4b(plugin.numParams);
    s.value4b(plugin.numInputs);
    s.value4b(plugin.numOutputs);
    s.value4b(plugin.flags);
    s.value4b(plugin.initialDelay);
    s.value4b(plugin.empty3a);
    s.value4b(plugin.empty3b);
    s.value4b(plugin.unkown_float);
    s.value4b(plugin.uniqueID);
    s.value4b(plugin.version);
}

template <typename S>
void serialize(S& s, VstIOProperties& props) {
    s.container1b(props.data);
}

template <typename S>
void serialize(S& s, VstMidiKeyName& key_name) {
    s.container1b(key_name.data);
}

template <typename S>
void serialize(S& s, VstParameterProperties& props) {
    s.value4b(props.stepFloat);
    s.value4b(props.smallStepFloat);
    s.value4b(props.largeStepFloat);
    s.container1b(props.label);
    s.value4b(props.flags);
    s.value4b(props.minInteger);
    s.value4b(props.maxInteger);
    s.value4b(props.stepInteger);
    s.value4b(props.largeStepInteger);
    s.container1b(props.shortLabel);
    s.value2b(props.displayIndex);
    s.value2b(props.category);
    s.value2b(props.numParametersInCategory);
    s.value2b(props.reserved);
    s.container1b(props.categoryLabel);
    s.container1b(props.future);
}

template <typename S>
void serialize(S& s, VstRect& rect) {
    s.value2b(rect.top);
    s.value2b(rect.left);
    s.value2b(rect.right);
    s.value2b(rect.bottom);
}

template <typename S>
void serialize(S& s, VstTimeInfo& time_info) {
    s.value8b(time_info.samplePos);
    s.value8b(time_info.sampleRate);
    s.value8b(time_info.nanoSeconds);
    s.value8b(time_info.ppqPos);
    s.value8b(time_info.tempo);
    s.value8b(time_info.barStartPos);
    s.value8b(time_info.cycleStartPos);
    s.value8b(time_info.cycleEndPos);
    s.value4b(time_info.timeSigNumerator);
    s.value4b(time_info.timeSigDenominator);
    s.container1b(time_info.empty3);
    s.value4b(time_info.flags);
}

/**
 * A wrapper around `VstEvents` that stores the data in a vector instead of a
 * C-style array. Needed until bitsery supports C-style arrays
 * https://github.com/fraillt/bitsery/issues/28. An advantage of this approach
 * is that RAII will handle cleanup for us.
 *
 * Before serialization the events are read from a C-style array into a vector
 * using this class's constructor, and after deserializing the original struct
 * can be reconstructed using the `as_c_events()` method.
 */
class alignas(16) DynamicVstEvents {
   public:
    DynamicVstEvents(){};

    explicit DynamicVstEvents(const VstEvents& c_events);

    /**
     * Construct a `VstEvents` struct from the events vector. This contains a
     * pointer to that vector's elements, so the returned object should not
     * outlive this struct.
     */
    VstEvents& as_c_events();

    /**
     * MIDI events are sent in batches.
     */
    std::vector<VstEvent> events;

   private:
    /**
     * Some buffer we can build a `VstEvents`. This object can be populated with
     * contents of the `VstEvents` vector using the `as_c_events()` method.
     *
     * The reason why this is necessary is because the `VstEvents` struct is
     * actually a variable size object. In the definition in
     * `vestige/aeffectx.h` the struct contains a single element `VstEvent`
     * pointer array, but the actual length of this array is
     * `VstEvents::numEvents`. Because there is no real limit on the number of
     * MIDI events the host can send at once we have to build this object on the
     * heap by hand.
     */
    std::vector<uint8_t> vst_events_buffer;
};

/**
 * Marker struct to indicate that that the event writes arbitrary data into one
 * of its own buffers and uses the void pointer to store start of that data,
 * with the return value indicating the size of the array.
 */
struct WantsChunkBuffer {};

/**
 * Marker struct to indicate that the event handler will write a pointer to a
 * `VstRect` struct into the void pointer.
 */
struct WantsVstRect {};

/**
 * Marker struct to indicate that the event handler will return a pointer to a
 * `VstTimeInfo` struct that should be returned transfered.
 */
struct WantsVstTimeInfo {};

/**
 * Marker struct to indicate that that the event requires some buffer to write
 * a C-string into.
 */
struct WantsString {};

/**
 * VST events are passed a void pointer that can contain a variety of different
 * data types depending on the event's opcode. This is typically either:
 *
 * - A null pointer, used for simple events.
 * - A char pointer to a null terminated string, used for passing strings to the
 *   plugin such as when renaming presets. Bitsery handles the serialization for
 *   us.
 *
 *   NOTE: Bitsery does not support null terminated C-strings without a known
 *         size. We can replace `std::string` with `char*` once it does for
 *         clarity's sake.
 *
 * - A byte vector for handling chunk data during `effSetChunk()`. We can't
     reuse the regular string handling here since the data may contain null
     bytes and `std::string::as_c_str()` might cut off everything after the
     first null byte.
 * - An X11 window handle.
 * - Specific data structures from `aeffextx.h`. For instance an event with the
 *   opcode `effProcessEvents` the hosts passes a `VstEvents` struct containing
 *   MIDI events, and `audioMasterIOChanged` lets the host know that the
 *   `AEffect` struct has changed.
 *
 * - Some empty buffer for the plugin to write its own data to, for instance for
 *   a plugin to report its name or the label for a certain parameter. There are
 *   two separate cases here. This is typically a short null terminated
 *   C-string. We'll assume this as the default case when none of the above
 *   options apply.
 *
 *   - Either the plugin writes arbitrary data and uses its return value to
 *     indicate how much data was written (i.e. for the `effGetChunk` opcode).
 *     For this we use a vector of bytes instead of a string since
 *   - Or the plugin will write a short null terminated C-string there. We'll
 *     assume that this is the default if none of the above options apply.
 */
using EventPayload = std::variant<std::nullptr_t,
                                  std::string,
                                  std::vector<uint8_t>,
                                  native_size_t,
                                  AEffect,
                                  DynamicVstEvents,
                                  WantsChunkBuffer,
                                  VstIOProperties,
                                  VstMidiKeyName,
                                  VstParameterProperties,
                                  WantsVstRect,
                                  WantsVstTimeInfo,
                                  WantsString>;

template <typename S>
void serialize(S& s, EventPayload& payload) {
    s.ext(payload,
          bitsery::ext::StdVariant{
              [](S&, std::nullptr_t&) {},
              [](S& s, std::string& string) {
                  s.text1b(string, max_string_length);
              },
              [](S& s, std::vector<uint8_t>& buffer) {
                  s.container1b(buffer, binary_buffer_size);
              },
              [](S& s, native_size_t& window_handle) {
                  s.value8b(window_handle);
              },
              [](S& s, AEffect& effect) { s.object(effect); },
              [](S& s, DynamicVstEvents& events) {
                  s.container(
                      events.events, max_midi_events,
                      [](S& s, VstEvent& event) { s.container1b(event.dump); });
              },
              [](S& s, VstIOProperties& props) { s.object(props); },
              [](S& s, VstMidiKeyName& key_name) { s.object(key_name); },
              [](S& s, VstParameterProperties& props) { s.object(props); },
              [](S&, WantsChunkBuffer&) {}, [](S&, WantsVstRect&) {},
              [](S&, WantsVstTimeInfo&) {}, [](S&, WantsString&) {}});
}

/**
 * An event as dispatched by the VST host. These events will get forwarded to
 * the VST host process running under Wine. The fields here mirror those
 * arguments sent to the `AEffect::dispatch` function.
 */
struct Event {
    int opcode;
    int index;
    native_intptr_t value;
    float option;
    /**
     * The event dispatch function has a void pointer parameter that's often
     * used to either pass additional data for the event or to provide a buffer
     * for the plugin to write a string into.
     *
     * The `VstEvents` struct passed for the `effProcessEvents` event contains
     * an array of pointers. This requires some special handling which is why we
     * have to use an `std::variant` instead of a simple string buffer. Luckily
     * Bitsery can do all the hard work for us.
     */
    EventPayload payload;

    template <typename S>
    void serialize(S& s) {
        s.value4b(opcode);
        s.value4b(index);
        // Hard coding pointer sizes to 8 bytes should be fine, right? Even if
        // we're hosting a 32 bit plugin the native VST plugin will still use 64
        // bit large pointers.
        s.value8b(value);
        s.value4b(option);

        s.object(payload);
    }
};

/**
 * The response for an event. This is usually either:
 *
 * - Nothing, on which case only the return value from the callback function
 *   gets passed along.
 * - A (short) string.
 * - Some binary blob stored as a byte vector. During `effGetChunk` this will
     contain some chunk data that should be written to `HostBridge::chunk_data`.
 * - A specific struct in response to an event such as `audioMasterGetTime` or
 *   `audioMasterIOChanged`.
 * - An X11 window pointer for the editor window.
 */
using EventResposnePayload = std::variant<std::nullptr_t,
                                          std::string,
                                          std::vector<uint8_t>,
                                          AEffect,
                                          VstIOProperties,
                                          VstMidiKeyName,
                                          VstParameterProperties,
                                          VstRect,
                                          VstTimeInfo>;

template <typename S>
void serialize(S& s, EventResposnePayload& payload) {
    s.ext(payload,
          bitsery::ext::StdVariant{
              [](S&, std::nullptr_t&) {},
              [](S& s, std::string& string) {
                  s.text1b(string, max_string_length);
              },
              [](S& s, std::vector<uint8_t>& buffer) {
                  s.container1b(buffer, binary_buffer_size);
              },
              [](S& s, AEffect& effect) { s.object(effect); },
              [](S& s, VstIOProperties& props) { s.object(props); },
              [](S& s, VstMidiKeyName& key_name) { s.object(key_name); },
              [](S& s, VstParameterProperties& props) { s.object(props); },
              [](S& s, VstRect& rect) { s.object(rect); },
              [](S& s, VstTimeInfo& time_info) { s.object(time_info); }});
}

/**
 * AN instance of this should be sent back as a response to an incoming event.
 */
struct EventResult {
    /**
     * The result that should be returned from the dispatch function.
     */
    native_intptr_t return_value;
    /**
     * Events typically either just return their return value or write a string
     * into the void pointer, but sometimes an event response should forward
     * some kind of special struct.
     */
    EventResposnePayload payload;

    template <typename S>
    void serialize(S& s) {
        s.value8b(return_value);

        s.object(payload);
    }
};

/**
 * Represents a call to either `getParameter` or `setParameter`, depending on
 * whether `value` contains a value or not.
 */
struct Parameter {
    int index;
    std::optional<float> value;

    template <typename S>
    void serialize(S& s) {
        s.value4b(index);
        s.ext(value, bitsery::ext::StdOptional(),
              [](S& s, auto& v) { s.value4b(v); });
    }
};

/**
 * The result of a `getParameter` or a `setParameter` call. For `setParameter`
 * this struct won't contain any values and mostly acts as an acknowledgement
 * from the Wine VST host.
 */
struct ParameterResult {
    std::optional<float> value;

    template <typename S>
    void serialize(S& s) {
        s.ext(value, bitsery::ext::StdOptional(),
              [](S& s, auto& v) { s.value4b(v); });
    }
};

/**
 * A buffer of audio for the plugin to process, or the response of that
 * processing. The number of samples is encoded in each audio buffer's length.
 */
struct AudioBuffers {
    /**
     * An audio buffer for each of the plugin's audio channels.
     */
    std::vector<std::vector<float>> buffers;

    /**
     * The number of frames in a sample. If buffers is not empty, then
     * `buffers[0].size() == sample_frames`.
     */
    int sample_frames;

    template <typename S>
    void serialize(S& s) {
        s.container(buffers, max_audio_channels,
                    [](S& s, auto& v) { s.container4b(v, max_buffer_size); });
        s.value4b(sample_frames);
    }
};
