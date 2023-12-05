
/*
 * SoxFilter plugin for AviSynth by Ferenc Pintér
 *

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .
//
// Linking Avisynth statically or dynamically with other modules is making a
// combined work based on Avisynth.  Thus, the terms and conditions of the GNU
// General Public License cover the whole combination.
//
// As a special exception, the copyright holders of Avisynth give you
// permission to link Avisynth with independent modules that communicate with
// Avisynth solely through the interfaces defined in avisynth.h, regardless of the license
// terms of these independent modules, and to copy and distribute the
// resulting combined work under terms of your choice, provided that
// every copy of the combined work is accompanied by a complete copy of
// the source code of Avisynth (the version of Avisynth used to produce the
// combined work), being distributed under the terms of the GNU General
// Public License plus this exception.  An independent module is a module
// which is not derived from or based on Avisynth, such as 3rd-party filters,
// import and export plugins, or graphical user interfaces.

*/


// needs /Zc:__cplusplus on MSVC:
#ifdef AVS_WINDOWS
#include "avisynth.h"
#include "avs/win.h"
#include "avs/filesystem.h"
#include "avs/minmax.h"
#include "../libsox/sox.h"
#else
#include <avisynth.h>
#include <avs/posix.h>
#include <avs/filesystem.h>
#include <avs/minmax.h>
#include <sox.h>
#endif
#include <vector>
#include <algorithm>
#include <string>
#include <sstream>
#include <atomic>

#define OUTPUT_MESSAGE_HANDLER_BUFFERS

static std::atomic<int> sox_init_counter = 0;

class SoxFilter; // forward
sox_effect_handler_t const* input_handler(void);
sox_effect_handler_t const* output_handler(void);

typedef struct avs_privdata_t {
  SoxFilter* caller;
} avs_privdata_t;

class SimpleBuf {
private:
  size_t read_ptr;
public:
  SimpleBuf() {
    reset();
  };

  void reset() {
    read_ptr = 0;
    avs_start = 0;
    avs_count = 0;
    avs_channels = 1;
  }
  size_t used_count() { return read_ptr / avs_channels; }
  size_t free_count_all_channels() { return read_buffer.size() - read_ptr; }
  size_t free_count() { return (read_buffer.size() - read_ptr) / avs_channels; }
  size_t size() { return read_buffer.size(); }

  // copies up to count elements from buffer to target
  // returns the actually copied element count
  size_t read(sox_sample_t *target, size_t count) {
    size_t copy_count = std::min(count, free_count_all_channels());
    std::copy(read_buffer.data() + read_ptr, read_buffer.data() + read_ptr + copy_count, target);
    read_ptr += copy_count;
    return copy_count;
  }

  // reinits and sets whole buffer from source
  void setdata_info(int64_t _avs_start, size_t count, int channels) {
    read_buffer.resize(count * channels);
    avs_channels = channels;
    read_ptr = 0;
    avs_start = _avs_start;
    avs_count = count;
  }

  // reinits and sets whole buffer from source
  void setdata(sox_sample_t* source, int64_t _avs_start, size_t count, int channels) {
    read_buffer.resize(count * channels);
    std::copy(source, source + count * channels, read_buffer.data());
    read_ptr = 0;
    avs_start = _avs_start;
    avs_count = count;
    avs_channels = channels;
  }

  int64_t next_start() {
    return avs_start + avs_count; // is not ChannelCount aware
  }

  std::vector<sox_sample_t> read_buffer; // sox_sample_t = int32_t
  int64_t avs_start;
  int64_t avs_count;
  int avs_channels;
};

typedef struct avs_in_info_t {
  // general
  PClip child;
  int AudioChannels;
  IScriptEnvironment* env;
  size_t buffersize_for_samples; // size of read_buffer
  SimpleBuf inputbuf;
} avs_in_info_t; // helper for Async child->GetAudio

typedef struct avs_out_info_t {
  size_t sample_count_getaudio;
  size_t output_sample_counter;
  size_t remaining_precalculated_samples;
  size_t precalc_ptr;
  sox_sample_t* output_sample_buf;
  std::vector<sox_sample_t> precalc_buf; // sox_sample_t = int32_t
} avs_out_info_t;

class SoxFilter : public GenericVideoFilter
{
public:
  SoxFilter(PClip _child, const AVSValue args, IScriptEnvironment* _env);
  virtual ~SoxFilter();
  void __stdcall GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* env);
  
  int __stdcall SetCacheHints(int cachehints, int frame_range) override {

    switch (cachehints) {
    case CACHE_GET_MTMODE:
      return MT_SERIALIZED; // Auto register AVS+ MT mode: serialized
    // Note: Avisynth+ has audio cache only since 2023 (3.7.3).
    // Sox requires strict sequential access.
    // Sequential access fails e.g. when a SoxFilter instance is referenced by
    // multiple following filters. This is why audio cache support is really needed.
    // Without an audio cache the same start/count sample range would be requested from 
    // SoxFilter's GetAudio many times.
    // And this results in losing synchron when processing buffers in SOX filter chain.
    case CACHE_GETCHILD_AUDIO_MODE:
      return CACHE_AUDIO;
    case CACHE_GETCHILD_AUDIO_SIZE:
      return std::max(256 * 1024, (int)(avs_in_info.buffersize_for_samples * sizeof(int32_t)));
    default:
      break;
    }
    return 0;
  }

  avs_in_info_t avs_in_info;
  avs_out_info_t out_info;

private:
  bool has_at_least_v10;
  sox_effects_chain_t* chain;
};

#ifdef OUTPUT_MESSAGE_HANDLER_BUFFERS
#include <mutex>
std::mutex errormessagemutex;
static std::string errormessage;
// Custom output message handler.
// Sox is sending 'usage' text and 'option' parse errors to stderr.
// If OUTPUT_MESSAGE_HANDLER_BUFFERS is defined, we can return the detailed error text, rather than a simple "error in filter X"
static void my_output_message(unsigned level, const char* filename, const char* fmt, va_list ap)
{
  char const* const str[] = { "FAIL", "WARN", "INFO", "DBUG" };
  if (sox_globals.verbosity >= level) {
    errormessage.clear();
    char err_info_buf[200];
    char err_text_buf[4096];
    // we don't spoil stderr, try to catch
    std::string fname = filename;
    std::string filename_noext = fs::path(filename).stem().string();
    sprintf(err_info_buf, "%s %s: ", str[min((int)level - 1, 3)], filename_noext.c_str());
    vsprintf(err_text_buf, fmt, ap);
    errormessage += "error: ";
    errormessage += err_info_buf;
    errormessage += err_text_buf;
    errormessage += "\n";
  }
}
/*
// original version
static void output_message(unsigned level, const char *filename, const char *fmt, va_list ap)
{
  char const * const str[] = {"FAIL", "WARN", "INFO", "DBUG"};
  if (sox_globals.verbosity >= level) {
    char base_name[128];
    sox_basename(base_name, sizeof(base_name), filename);
    fprintf(stderr, "%s %s: ", str[min(level - 1, 3)], base_name);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }
}
*/
#endif

SoxFilter::SoxFilter(PClip _child, const AVSValue args_avs, IScriptEnvironment* env) :
  GenericVideoFilter(_child),
  chain(nullptr)
{

  has_at_least_v10 = true; // for audio channel speaker masks
  try { env->CheckVersion(10); }
  catch (const AvisynthError&) { has_at_least_v10 = false; }

  int sox_errno;

  // All libSoX applications must start by initialising the SoX library
  // But only once, for all filter instances!
  // sox_init_counter is atomic.
  if (sox_init_counter++ == 0) {
    sox_errno = sox_init();
    if (sox_errno != SOX_SUCCESS)
      env->ThrowError("SoxFilter: sox_init failed: %d %s\n", sox_errno, sox_strerror(sox_errno));
    sox_globals.verbosity = 1; // 1:only report FAIL; 4: debug
  }

  // fields known at filter creation time
  avs_in_info.child = _child;
  avs_in_info.AudioChannels = vi.AudioChannels();
  // sample count for holding all channels' samples in 1 seconds
  avs_in_info.buffersize_for_samples = vi.audio_samples_per_second * vi.AudioChannels();

  out_info.remaining_precalculated_samples = 0;
  out_info.precalc_ptr = 0;
  out_info.precalc_buf.resize(avs_in_info.buffersize_for_samples); // 1 sec

  // process effects, one effect in each AviSynth string parameter
  AVSValue args_effectlist = args_avs[1];

  const int num_args = args_effectlist.ArraySize();

  if (!num_args)
    env->ThrowError("SoxFilter: No effects specified");

  // ------------------------------------------------------------
  sox_signalinfo_t signalinfo_in;
  signalinfo_in.rate = vi.audio_samples_per_second;
  signalinfo_in.channels = vi.AudioChannels();
  signalinfo_in.precision = 32;
  signalinfo_in.length = vi.num_audio_samples * vi.AudioChannels(); // samples*channels in file; 0 if unknown
  signalinfo_in.mult = nullptr; // effect headroom multiplier, can be NULL

  sox_encodinginfo_t encodinginfo_in;
  encodinginfo_in.encoding = sox_encoding_t::SOX_ENCODING_SIGN2;
  encodinginfo_in.bits_per_sample = 32;
  encodinginfo_in.compression = 0.0;
  encodinginfo_in.reverse_bytes = sox_option_t::sox_option_default;
  encodinginfo_in.reverse_bits = sox_option_t::sox_option_default;
  encodinginfo_in.reverse_nibbles = sox_option_t::sox_option_default;
  encodinginfo_in.opposite_endian = sox_false;

  // output signal characteristics are the same as the input characteristics 
  sox_encodinginfo_t encodinginfo_out = encodinginfo_in;
  sox_signalinfo_t signalinfo_out = signalinfo_in;

  // Create an effects chain; some effects need to know about the input
  // or output file encoding so we provide that information here
  // In Avisynth this is fixed and the setting is the same for both in and out
  chain = sox_create_effects_chain(&encodinginfo_in, &encodinginfo_out);
  if (!chain)
    env->ThrowError("SoxFilter: error creating effect chain\n");

  sox_effect_t* e;

  // -------------- input ------------------------------------------
  // The first effect in the effect chain: source.
  // This input handler will feed the sox filter flow
  // from our internal buffer.
  // This buffer is filled by calling child's GetAudio
  // on demand, asynchronously.
  e = sox_create_effect(input_handler());
  if (!e) {
    sox_delete_effects_chain(chain);
    env->ThrowError("SoxFilter: error creating input handler\n");
  }
  avs_privdata_t priv_for_input;
  priv_for_input.caller = this; // to access the Avisynth SoxFilter class variables 
  *reinterpret_cast<avs_privdata_t*>(e->priv) = priv_for_input; // whole struct copy
  // This input drain becomes the first effect in the chain
  sox_errno = sox_add_effect(chain, e, &signalinfo_in, &signalinfo_in);
  free(e);
  if (sox_errno != SOX_SUCCESS) {
    sox_delete_effects_chain(chain);
    env->ThrowError("Error in creating effect 'input' as input_handler: %d %s\n", sox_errno, sox_strerror(sox_errno));
  }

  // --------------- effects ----------------------------------------
  // Add effects one by one from SoxFilter's parameter(s)
  for (int curr_eff = 0; curr_eff < num_args; curr_eff++)
  {
    std::string arg_str = args_effectlist[curr_eff].AsString();

    // magic: remove multiple spaces and convert them into a single one
    arg_str.erase(std::unique(arg_str.begin(), arg_str.end(),
      [](char a, char b) { return a == ' ' && b == ' '; }), arg_str.end());

    std::vector<std::string> arg_list_array;

    std::istringstream find_in_this(arg_str);
    std::string one_string;
    while (std::getline(find_in_this, one_string, ' ')) {
      arg_list_array.push_back(one_string);
    }

    // First argument is the effect name
    const char* effect_name = arg_list_array[0].c_str();

    // create char * array from arglist[] elements
    // The rest (size-1) strings are effect parameters
    int num_params = (int)arg_list_array.size() - 1;
    std::vector<char*> arglist_ptr(num_params);
    for (int i = 0; i < num_params; i++)
      arglist_ptr[i] = (char*)arg_list_array[i + 1].c_str();

    std::string error_text = "SoxFilter: (" + std::string(effect_name) + ") ";
    bool ok = false;

    // Find a named effect in the effects library 
    const sox_effect_handler_t* effect_handler = sox_find_effect(effect_name);
    if (effect_handler == nullptr)
      error_text += "Could not find effect.";
    /* v2.1: Let's allow them, we can change the VideoInfo audio properties at the end.
    // some checking on possible incompatibility
    else if (effect_handler->flags & SOX_EFF_CHAN)
      error_text += "Cannot run effects that change the number of channels.";
    else if (effect_handler->flags & SOX_EFF_RATE)
      error_text += "Cannot run effects that change the samplerate.";
    */
    else {
      // Create the effect, and initialise it with the parameters
      e = sox_create_effect(effect_handler);
      if (!e)
        error_text += "Cannot create effect: sox_create_effect failed.\n";
      else
        ok = true;
    }

    if (!ok) {
      sox_delete_effects_chain(chain);
      env->ThrowError(error_text.c_str());
    }


    { // mutex scope
#ifdef OUTPUT_MESSAGE_HANDLER_BUFFERS
      std::lock_guard<std::mutex> lock(errormessagemutex);
      // Hijack it only for catching errors in 'options' processing
      auto tmp_output_message_handler = sox_globals.output_message_handler;
      sox_globals.output_message_handler = my_output_message;
#endif
      sox_errno = sox_effect_options(e, num_params, arglist_ptr.data());
      if (sox_errno != SOX_SUCCESS) {
        // "my_output_message" will add a more detailed error beforehand.
        free(e);
        sox_delete_effects_chain(chain);
        error_text += "Error in options.\n" + errormessage;
        env->ThrowError(error_text.c_str());
      }
#ifdef OUTPUT_MESSAGE_HANDLER_BUFFERS
      sox_globals.output_message_handler = tmp_output_message_handler;
    }
#endif

    // sox_add_effect:
    // signalinfo_in specifies the input signal info for this effect. 
    // signalinfo_out is a suggestion as to what the output signal should be 
    // but depending on the effects given options and on in the effect can choose 
    // to do differently; we pass the same signalinfo_in for that.
    // Whatever output rate and channels the effect does produce are written back to 
    // signalinfo_in. 
    // It is meant that in be stored and passed to each new call to sox_add_effect so 
    // that changes will be propagated to each new effect.

    // Add the effect to the end of the effects processing chain
    sox_errno = sox_add_effect(chain, e, &signalinfo_in, &signalinfo_in);
    free(e);
    if (sox_errno != SOX_SUCCESS) {
      sox_delete_effects_chain(chain);
      error_text += "Cannot add effect to the chain.";
      env->ThrowError(error_text.c_str());
    }
    // sanity test: may fail.
    if ((signalinfo_in.length % signalinfo_in.channels) != 0) {
      // Opps, unfortunately this can occur: odd number of samples for two channels.
      // A bug? Maybe.
      // sox_add_effects recalculates the length without considering the channel count.
      //   if (effp->handler.flags & SOX_EFF_RATE)
      //     effp->out_signal.length = effp->out_signal.length / in->rate * effp->out_signal.rate + .5;
      // I'd calculate the number of samples which are available for all channels.
      // Example (old rate = 48000; new rate = 48100)
      // in: sample_count = 2884481, channels=2 => length = 5768962
      // out (ideal): 2 * round(2884481 / 48000.0 * 48100.0) = 2 * round(2890490,335) = 2 * 2890490 = 5780980
      // out (sox)  : round(5768962 / 48000.0 * 48100.0) = round (5780980,670) = 5780981 (odd for two channels?!)
      signalinfo_in.length -= (signalinfo_in.length % signalinfo_in.channels); // make it multiple of channels.
      /*
      So we don't throw fatal error.
      error_text += "The number of samples the effect returned is not multiple of number of channels.";
      env->ThrowError(error_text.c_str());
      */
    }
    }

  const int input_AudioChannels = vi.AudioChannels();
  // write back the resulting rate and channel count to VideoInfo format
  vi.audio_samples_per_second = (int)(signalinfo_in.rate + 0.5); // effects can change sampling rate
  vi.nchannels = signalinfo_in.channels; // effects can change number of channels, e.g. remix stereo to mono
  vi.num_audio_samples = signalinfo_in.length / vi.AudioChannels();

  // Clear channel speaker mask if the number of channels has been changed.
  // Better than guessing
  if (input_AudioChannels != vi.AudioChannels()) {
    if (has_at_least_v10) {
      vi.SetChannelMask(false /* mask is unknown */, 0 /* n/a */);
    }
  }

  // ------------------------ output ------------------------------
  // Final 'effect' in the chain: output, copy back to Avisynth GetAudio buffer
  e = sox_create_effect(output_handler());
  if (!e) {
    sox_delete_effects_chain(chain);
    env->ThrowError("SoxFilter: error creating output handler\n");
  }
  avs_privdata_t priv_for_output;
  priv_for_output.caller = this; // to access the Avisynth SoxFilter class variables 
  *reinterpret_cast<avs_privdata_t*>(e->priv) = priv_for_output; // whole struct copy
  sox_errno = sox_add_effect(chain, e, &signalinfo_in, &signalinfo_in);
  free(e);
  if (sox_errno != SOX_SUCCESS) {
    sox_delete_effects_chain(chain);
    env->ThrowError("Error in creating effect 'output' as output_handler: %d %s\n", sox_errno, sox_strerror(sox_errno));
  }
}


SoxFilter::~SoxFilter()
{
  sox_delete_effects_chain(chain);
  // call quit only once for all filter instances
  if(--sox_init_counter == 0)
    sox_quit();
}

// Special 'effect': callback to input the samples at the beginning of the effects chain.
// The function that will be called to input samples into the effects chain.
// It will use the child->GetAudio() of the calling class in an asynchronous, 
// on-demand way.
int input_drain(sox_effect_t* effp, sox_sample_t* obuf, size_t* osamp)
{
  (void)effp; // n/a

  // access the owner Avisynth filter class instance
  avs_privdata_t* privdata = reinterpret_cast<avs_privdata_t*>(effp->priv);
  avs_in_info_t* avs_in_info = &privdata->caller->avs_in_info;

  /*
  _RPT4(0, "input_drain: BEGIN osamp=%d channels=%d next_start=%d input_samples_used=%d\n",
    (int)*osamp,
    (int)effp->out_signal.channels,
    (int)avs_in_info->next_start,
    (int)avs_in_info->input_samples_used
    );
  */

  // safety
  if (*osamp > avs_in_info->buffersize_for_samples)
    *osamp = avs_in_info->buffersize_for_samples;

  // ensure that *osamp is a multiple of the number of channels.
  *osamp -= *osamp % effp->out_signal.channels;

  if (avs_in_info->inputbuf.free_count() == 0) {
    size_t count = avs_in_info->buffersize_for_samples / avs_in_info->AudioChannels;
    avs_in_info->inputbuf.setdata_info(avs_in_info->inputbuf.next_start(), count, avs_in_info->AudioChannels); // resets internal read_ptr as well
    avs_in_info->child->GetAudio(&avs_in_info->inputbuf.read_buffer[0], avs_in_info->inputbuf.avs_start, count, avs_in_info->env);
  }

  size_t samples_available_all_channels = avs_in_info->inputbuf.free_count_all_channels();

  // don't provide more samples than it was requested
  *osamp = std::min(*osamp, samples_available_all_channels);

  // Read up to *osamp samples into obuf; update pointers
  avs_in_info->inputbuf.read(obuf, *osamp);

  _RPT5(0, "input_drain: _END_ osamp=%d channels=%d next_start=%d input_samples_used=%d samples_available=%d\n",
    (int)*osamp,
    (int)effp->out_signal.channels,
    (int)avs_in_info->inputbuf.next_start(),
    (int)avs_in_info->inputbuf.used_count(),
    (int)avs_in_info->inputbuf.free_count()
  );

  return *osamp ? SOX_SUCCESS : SOX_EOF;
}

// A stub effect handler to handle inputting samples to the effects chain.
// The only function needed is 'drain'.
sox_effect_handler_t const* input_handler(void)
{ 
  static sox_effect_handler_t handler = {
    "input", // effect name
    NULL, // short usage text
    SOX_EFF_MCHAN, // Combination of SOX_EFF_* flags
    NULL, // getopts Called to parse command-line arguments (called once per effect).
    NULL, // flow start Called to initialize effect (called once per flow)
    NULL, // Called to process samples.
    input_drain, // drain Called to finish getting output after input is complete
    NULL, // stop Called to shut down effect (called once per flow)
    NULL, // kill Called to shut down effect (called once per effect)
    sizeof(avs_privdata_t) // Size of private data SoX should pre-allocate for effect
  };
  return &handler;
}

// Special 'effect': callback to output the samples at the end of the effects chain.
static int output_flow(sox_effect_t* effp LSX_UNUSED, sox_sample_t const* ibuf,
  sox_sample_t* obuf LSX_UNUSED, size_t* isamp, size_t* osamp)
{
  (void)effp; // unused

  // Debugging shows that (surprisingly) output_flow is called at the very beginning,
  // after (re)starting the effect flow, before any input drain happens. 
  // In this case *isamp == 0.
  // This can handle to flush remaining content of output buffer.

  avs_privdata_t* privdata = reinterpret_cast<avs_privdata_t*>(effp->priv);
  avs_out_info_t* out_info = &privdata->caller->out_info;

  size_t samplecount_for_buffer_full = out_info->sample_count_getaudio - out_info->output_sample_counter;
  size_t samples_to_copy;
  
  _RPT4(0, "output_flow: BEGIN isamp=%d channels=%d samplecount_for_buffer_full=%d output_sample_counter=%d\n",
    (int)*isamp,
    (int)effp->in_signal.channels,
    (int)samplecount_for_buffer_full,
    (int)out_info->output_sample_counter
  );

  // If samplecount_for_buffer_full < *isamp then we don't need the whole data yet.
  // This occurs e.g.when FFMPEG requests only 80 bytes at a time, but *isamp would already
  // have 4096 bytes pre-processed ready-made in the background.
  samples_to_copy = std::min(*isamp, samplecount_for_buffer_full);

  size_t remaining_samples = *isamp - samples_to_copy;
  // When not all pre-processed data is needed now, they are copied into a precalc_buf.
  // If the process has precalculated more data than is needed at the moment, then the
  // next GetAudio won't start another 'flow', GetAudio will consume data from this
  // buffer as long as it exists. This is an independent FIFO buffer.

  // Write out *isamp samples from ibuf
  if (samples_to_copy > 0) {
    // copy the amount that was requested
    memcpy(&out_info->output_sample_buf[out_info->output_sample_counter], ibuf, samples_to_copy * sizeof(sox_sample_t));
    // copy the rest into the precalc buffer
    memcpy(&out_info->precalc_buf[0], &ibuf[samples_to_copy], remaining_samples * sizeof(sox_sample_t));
    _RPT2(0, "output_flow: copying samples %d->[output_sample_buf] %d->[output_excess_sample_buffer]\n", 
      (int)samples_to_copy,
      (int)remaining_samples
    );
    out_info->remaining_precalculated_samples = remaining_samples;
    out_info->precalc_ptr = 0;
    out_info->output_sample_counter += samples_to_copy;
  }
  else {
    _RPT0(0, "output_flow: NOTHING TO COPY, reset excess counters (no excess)\n");
    out_info->precalc_ptr = 0;
    out_info->remaining_precalculated_samples = 0;
  }

  /* Outputting is the last `effect' in the effect chain so always passes
   * 0 samples on to the next effect (as there isn't one!) */
  *osamp = 0;

  _RPT4(0, "output_flow: _END_ isamp=%d channels=%d samplecount_for_buffer_full=%d output_sample_counter=%d\n",
    (int)*isamp,
    (int)effp->in_signal.channels,
    (int)samplecount_for_buffer_full,
    (int)out_info->output_sample_counter
  );

  // FIXME: return also if greater than? Can we have such case?
  if (out_info->output_sample_counter == out_info->sample_count_getaudio) {
    _RPT0(0, "output_flow: _END_ return EOF (buffer ready)\n");
    // We've filled the whole buffer requested by SoxFilter::GetAudio().
    // Return EOF to tell effect chain flow (sox_flow_effects) to exit from its loop
    return SOX_EOF;
  }
  _RPT0(0, "output_flow: _END_ return SUCCESS (buffer not ready)\n");

  return SOX_SUCCESS; // All samples output to the buffer successfully, but there remained some
}

// A stub effect handler to handle outputting samples from the effects chain.
// The only function needed for this is 'flow'
sox_effect_handler_t const* output_handler(void)
{
  static sox_effect_handler_t handler = {
    "output", 
    NULL, // short usage text
    SOX_EFF_MCHAN, // Combination of SOX_EFF_* flags
    NULL, // getopts Called to parse command-line arguments (called once per effect).
    NULL, // flow start Called to initialize effect (called once per flow)
    output_flow, // Called to process samples.
    NULL, // drain Called to finish getting output after input is complete
    NULL, // stop Called to shut down effect (called once per flow)
    NULL, // kill Called to shut down effect (called once per effect)
    sizeof(avs_privdata_t) // Size of private data SoX should pre-allocate for effect
  };
  return &handler;
}

static void DebugFilterInfos(sox_effects_chain_t* chain)
{
  // debug filter infos
  for (size_t i = 0; i < chain->length; i++)
  {
    sox_effect_t* current_effect = chain->effects[i];
    _RPT2(0, "Filter [%s] flows=%d\n", current_effect->handler.name, current_effect->flows);
    for (size_t eff = 0; eff < current_effect->flows; eff++)
    {
      sox_effect_t* current_flow_effect = &chain->effects[i][eff];
      _RPT5(0, "   #%d obeg=%d oend=%d i_signal.length=%d imin=%d\n",
        eff,
        (int)current_effect->obeg,
        (int)current_effect->oend,
        (int)current_effect->in_signal.length,
        (int)current_effect->imin
        //(int)current_effect->out_signal.length
      );
    }
  }
}

static void RestartEffects(sox_effects_chain_t *chain, IScriptEnvironment *env) {
  _RPT0(0, "RESTART EFFECTS!\n");
  for (size_t i = 0; i < chain->length; i++)
  {
    sox_effect_t* current_effect = chain->effects[i];
    _RPT1(0, "  stopping %s!\n", chain->effects[i]->handler.name);
    sox_stop_effect(current_effect); // stops all flows, but start each flow separately

    const size_t flowcount = chain->effects[i]->flows;
    for (size_t flow = 0; flow < flowcount; flow++) {
      current_effect = &chain->effects[i][flow];
      int sox_errno = (current_effect->handler.start)(current_effect);
      if (sox_errno != SOX_SUCCESS) {
        env->ThrowError("SoxFilter:  (%s) Could not restart filter: \n\n%d %s\n", current_effect->handler.name, sox_errno, sox_strerror(sox_errno));
      }
    }
  }
  _RPT0(0, "RESTART EFFECTS done!\n");
}

// Debugging (avsmeter does not use audio): ffmpeg  -i s2.avs -c:a copy valami2.wav
void __stdcall SoxFilter::GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* env)
{
  // Everything in SOX is single samples, not accounting for channels.
  out_info.sample_count_getaudio = (size_t)count * vi.AudioChannels();
  out_info.output_sample_counter = 0;
  out_info.output_sample_buf = (sox_sample_t*)buf; // int32_t *

  _RPT4(0, "\nSoxFilter::GetAudio: start=%d, count=%d, samplecount_mul_chn=%d next_start=%d\n",
    (int)start,
    (int)count,
    (int)out_info.sample_count_getaudio,
    (int)avs_in_info.inputbuf.next_start()
  );

  // DebugFilterInfos();

/*
    // Illustrating the issue w/o Avisynth Audio Cache.
    // For such an Avisynth script, without audio cache, SoxFilter is called 4 times, with the same parameter!

    a = KillVideo(clp)
    back = a.soxfilter("sinc 100-7000")
    fl = a.GetLeftChannel()
    fr = a.GetRightChannel()
    cc = mixaudio(a.GetRightChannel(), a.GetLeftChannel(), 0.5, 0.5)
    lfe = ConvertToMono(a)#.SoxFilter("lowpass 120", "vol -0.5")
    sl = mixaudio(back.GetLeftChannel(), back.GetRightChannel(), 0.668, -0.668)
    sr = mixaudio(back.GetRightChannel(), back.GetLeftChannel(), 0.668, -0.668)

    For ensuring script linear access, SoxFilter calls "EnsureVBRMp3Sync" (part of Avisynth+).

    When "EnsureVBRMp3Sync" (something like "RequestLinear" for frames) is applied after this filter,
    all samples are re-requested from SoxFilter :( from start=0 to start-1.

    Example requests: (from_sample sample_count, from_sample sample_count, ...)

    Without "EnsureVBRMp3Sync", no audio cache:
      0    1000,    0 1000, 0    1000,    0 1000 (4 times from 0 count=1000)

      1000 1000, 1000 1000, 1000 1000, 1000 1000 (4 times from 1000 count=1000)

      2000 1000, 2000 1000, 2000 1000, 2000 1000 (4 times from 2000 count=1000)
      ...

    With "EnsureVBRMp3Sync", no audio cache:
      0    1000,    0 1000, 0    1000,    0 1000 (4 times from 0 count=1000)

      needed only from 1000 count=1000 --> restart from zero
      0    1000, 1000 1000
      0    1000, 1000 1000
      0    1000, 1000 1000
      0    1000, 1000 1000

      needed only from 82000 count=1000 --> restart from zero
      0    1000, 1000 1000, 2000 1000, 3000 1000, ... 82000 1000
      0    1000, 1000 1000, 2000 1000, 3000 1000, ... 82000 1000
      0    1000, 1000 1000, 2000 1000, 3000 1000, ... 82000 1000
      0    1000, 1000 1000, 2000 1000, 3000 1000, ... 82000 1000

      So when the same sample range would be requested multiple times,
      it would re-process all previous data from (e.g. from 0 to 81999) 
      then the one that was really needed (82000-82999)
*/

  // First we check if we should reinitialize filters.
  if (start <= 0 && avs_in_info.inputbuf.next_start()>0) {
    // The stream is restarted every time when a sample previous to the last one is requested.
    // Q: or start != prev_start+prev_count ?
    // A: no, in such cases EnsureVBRMp3Sync requests samples from zero: start=0
    RestartEffects(chain, env);
    // Initialize input filter to force full buffer read
    avs_in_info.inputbuf.setdata_info(0, 0, vi.AudioChannels()); // start=0, count = 0
    out_info.precalc_ptr = 0;
    out_info.remaining_precalculated_samples = 0;
    /*
    _RPT4(0, "\nSoxFilter::GetAudio: AFTER RESTART start=%d, count=%d, samplecount_mul_chn=%d next_start=%d\n",
      (int)start,
      (int)count,
      (int)out_info.sample_count_getaudio,
      (int)avs_in_info.next_start
    );
    */
    // DebugFilterInfos()
  }

  // While there are precalculated output samples in our output buffer, consume them up.
  // See remarks in 'output_flow' as well.
  // Effect flow is not started while precalculated samples still exist.
  if (out_info.remaining_precalculated_samples > 0) {
    
    _RPT3(0, "SoxFilter::GetAudio: BEFORE excess: samplecount=%d samplecount_mul_chn=%d mod=%d\n",
      (int)out_info.remaining_precalculated_samples / vi.AudioChannels(),
      (int)out_info.remaining_precalculated_samples,
      (int)out_info.remaining_precalculated_samples % vi.AudioChannels());
    
    size_t samplecount_to_copy_from_precalc_buf = std::min((size_t)count * vi.AudioChannels(), out_info.remaining_precalculated_samples);
    memcpy(
      &out_info.output_sample_buf[out_info.output_sample_counter], 
      &out_info.precalc_buf[out_info.precalc_ptr], 
      samplecount_to_copy_from_precalc_buf * sizeof(sox_sample_t));
    out_info.output_sample_counter += samplecount_to_copy_from_precalc_buf;
    out_info.precalc_ptr += samplecount_to_copy_from_precalc_buf;
    out_info.remaining_precalculated_samples -= samplecount_to_copy_from_precalc_buf;
    count -= samplecount_to_copy_from_precalc_buf / vi.AudioChannels();
    
    _RPT3(0, "SoxFilter::GetAudio: AFTER excess: samplecount=%d samplecount_mul_chn=%d mod=%d\n",
      (int)out_info.remaining_precalculated_samples / vi.AudioChannels(),
      (int)out_info.remaining_precalculated_samples,
      (int)out_info.remaining_precalculated_samples % vi.AudioChannels());
  }

  // Save env for GetAudio which is invoked in 'input' effect 
  // which is called from sox_flow_effects main loop.
  avs_in_info.env = env; // to be able to use env->GetAudio in input drain

  // output_sample_counter is increased in the output 'effect'
  while (out_info.output_sample_counter < out_info.sample_count_getaudio)
  {
    _RPT4(0, "SoxFilter::GetAudio: BEFORE flow: output_sample_counter_mul_chn=%d total_needed_sample_count_mul_chn=%d next_start=%d\n",
      (int)out_info.output_sample_counter,
      (int)out_info.sample_count_getaudio,
      (int)out_info.remaining_precalculated_samples % vi.AudioChannels(),
      (int)avs_in_info.inputbuf.next_start());
    
    int sox_errno = sox_flow_effects(chain, NULL, NULL);

    _RPT3(0, "SoxFilter::GetAudio: AFTER flow debug1/2: output_sample_counter_mul_chn=%d total_needed_sample_count_mul_chn=%d next_start=%d\n",
      (int)out_info.output_sample_counter,
      (int)out_info.sample_count_getaudio,
      (int)out_info.remaining_precalculated_samples % vi.AudioChannels());
    _RPT4(0, "SoxFilter::GetAudio: AFTER flow debug2/2: samplecount=%d samplecount_mul_chn=%d mod=%d\n",
      (int)out_info.remaining_precalculated_samples / vi.AudioChannels(),
      (int)out_info.remaining_precalculated_samples,
      (int)out_info.remaining_precalculated_samples % vi.AudioChannels(),
      (int)avs_in_info.inputbuf.next_start());
    // EOF: a buffer is fully exported into Avisynth's GetAudio buffer
    // EOF means that output_sample_counter == sample_count_getaudio, so we'll exit from this loop
    // SUCCESS: buffer is not filled 100% yet, output_sample_counter is still < sample_count_getaudio
    // EOF and SUCCESS are set by 'output_flow' return value
    if (sox_errno != SOX_SUCCESS && sox_errno != SOX_EOF)
      env->ThrowError("SoxFilter: sox_flow_effects error: \n\n%d %s\n", sox_errno, sox_strerror(sox_errno));
    if (sox_errno == SOX_EOF) {
      if (out_info.output_sample_counter != out_info.sample_count_getaudio)
        env->ThrowError("SoxFilter: sox_flow_effects error EOF received but buffer for GetAudio is not finished:\n");
      break;
    }
  }
}

// Example:
// SoxFilter("lowpass 120", "vol -0.5", "sinc -n 29 -b 100 7000", "vol -3dB", "reverb 30 20", "compand 1.0,0.6 -1.3,-0.1")
AVSValue __cdecl Create_SoxFilter(AVSValue args, void* user_data, IScriptEnvironment* env)
{
  // sox works with 32 bit integers: sox_sample_t = int32_t
  // Any input must be converted into that
  PClip clip = args[0].AsClip();
  AVSValue new_args[3] = { clip, AvsSampleType::SAMPLE_INT32, AvsSampleType::SAMPLE_INT32 };
  clip = env->Invoke("ConvertAudio", AVSValue(new_args, 3)).AsClip();

  clip = new SoxFilter(clip, args, env);
  
  // This filter is inserted in the chain for strict sequential access, 
  // but gives big penalty for any out-of-sequence sample request;
  // restarts audio read from the very beginning sample if such condition
  // is encountered.
  AVSValue Ia[1] = { clip }; 
  clip = env->Invoke("EnsureVBRMp3Sync", AVSValue(Ia, 1)).AsClip();
  
  return clip;

}

// returns an LF separated string on all internal filter names
// List is long, SubTitle requires literal "\n" for line break, so use like this:
//   BlankClip(10000, 1920, 1080)
//   SubTitle(ReplaceStr(SoxFilter_ListEffects(), e"\n", "\n"), lsp = 0, size = 10)
AVSValue SoxFilter_ListEffects(AVSValue args, void*, IScriptEnvironment* env)
{
  std::string s;

  if (sox_init_counter++ == 0) {
    int sox_errno = sox_init();
    if (sox_errno != SOX_SUCCESS) {
      --sox_init_counter;
      env->ThrowError("SoxFilter_ListEffects: sox_init failed: %d %s\n", sox_errno, sox_strerror(sox_errno));
    }
  }

  sox_effect_fn_t const* fns = sox_get_effect_fns();
  for (int e = 0; fns[e]; ++e) {
    const sox_effect_handler_t* eh = fns[e]();
    if (eh && eh->name)
      s += std::string(eh->name) + "\n";
  }

  if (--sox_init_counter == 0)
    sox_quit();
  return env->SaveString(s.c_str());
}

// returns an array, with each element containing a two-dimensional subarray
// 0th element: effect name, 1st element: usage
AVSValue SoxFilter_GetAllEffects(AVSValue args, void*, IScriptEnvironment* env)
{
  std::string s;

  if (sox_init_counter++ == 0) {
    int sox_errno = sox_init();
    if (sox_errno != SOX_SUCCESS) {
      --sox_init_counter;
      env->ThrowError("SoxFilter_GetAllEffects: sox_init failed: %d %s\n", sox_errno, sox_strerror(sox_errno));
    }
  }

  sox_effect_fn_t const* fns = sox_get_effect_fns();
  for (int e = 0; fns[e]; ++e) {
    const sox_effect_handler_t* eh = fns[e]();
    if (eh && eh->name)
      s += std::string(eh->name) + "\n";
  }

  std::vector<std::string> splits;
  std::string split;

  std::istringstream ss(s);
  while (std::getline(ss, split, '\n'))
    splits.push_back(split);

  const int size = (int)splits.size();
  std::vector<AVSValue> result(size, AVSValue());

  for (auto i = 0; i < size; i++) {
    std::vector<AVSValue> aEffect(2); // effect_name, usage
    aEffect[0] = env->SaveString(splits[i].c_str());

    const sox_effect_handler_t* effect_handler = sox_find_effect(splits[i].c_str());
    if (effect_handler != nullptr && effect_handler->usage != nullptr)
      aEffect[1] = env->SaveString(effect_handler->usage);
    else
      aEffect[1] = env->SaveString("");
    // not found is not an error, just returns empty string
    result[i] = AVSValue(aEffect.data(), 2);
  }

  if (--sox_init_counter == 0)
    sox_quit();
  return AVSValue(result.data(), size);
}




// Each Sox effect has a short "usage" string.
// Empty string is returned if not found or no usage info.
// The string contains LF as line separator.
// SubTitle requires literal "\n" for line break, so use like this:
//   BlankClip(10000, 1920, 1080)
//   SubTitle(ReplaceStr(SoxFilter_GetEffectUsage("compand"), e"\n","\n"), lsp=0)
AVSValue SoxFilter_GetEffectUsage(AVSValue args, void*, IScriptEnvironment* env)
{
  const char * effect_name = args[0].AsString();

  AVSValue result = "";

  if (sox_init_counter++ == 0) {
    int sox_errno = sox_init();
    if (sox_errno != SOX_SUCCESS) {
      --sox_init_counter;
      env->ThrowError("SoxFilter_GetEffectUsage: sox_init failed: %d %s\n", sox_errno, sox_strerror(sox_errno));
    }
  }

  const sox_effect_handler_t* effect_handler = sox_find_effect(effect_name);
  if (effect_handler != nullptr) {
    result = env->SaveString(effect_handler->usage);
  }
  // not found is not an error, just returns empty string

  if (--sox_init_counter == 0)
    sox_quit();

  return result;
}

const AVS_Linkage* AVS_linkage;

extern "C" __declspec(dllexport)
const char* __stdcall AvisynthPluginInit3(IScriptEnvironment * env, const AVS_Linkage* const vectors)
{
  AVS_linkage = vectors;
  env->AddFunction("SoxFilter", "cs+", Create_SoxFilter, NULL);
  env->AddFunction("SoxFilter_ListEffects", "", SoxFilter_ListEffects, NULL);
  env->AddFunction("SoxFilter_GetAllEffects", "", SoxFilter_GetAllEffects, NULL);
  env->AddFunction("SoxFilter_GetEffectUsage", "s", SoxFilter_GetEffectUsage, NULL);
  return "SoxFilter";
}

