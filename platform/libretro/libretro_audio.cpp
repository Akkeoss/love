/**
 * libretro_audio.cpp -- pulling audio out of LOVE, once per frame.
 *
 * OpenAL pushes: it runs a thread that keeps a sound card fed. libretro pulls:
 * the frontend expects the core to hand back exactly the samples belonging to
 * the frame it just ran. Those are opposite models, and reconciling them looked
 * like the biggest job in this port -- rewriting LOVE's mixing (3D positioning,
 * effects, filters, resampling) against a buffer we own.
 *
 * None of that was necessary. OpenAL Soft has a loopback device: it does all of
 * its usual work but renders into a buffer on demand instead of driving
 * hardware. So LOVE's audio backend is untouched; only the destination changed.
 * See the LOVE_ENABLE_LIBRETRO branches in src/modules/audio/openal/Audio.cpp.
 *
 * What is left here is the arithmetic: how many samples is one frame worth, and
 * how to avoid drift when that number is not a whole one.
 */

#include "libretro_state.h"

#include "modules/audio/openal/Audio.h"
#include "common/Module.h"

#include <vector>

namespace love {
namespace libretro {

namespace {

// Interleaved stereo int16, which is what audio_batch_cb takes.
std::vector<int16_t> buffer;

// Samples per frame is rarely a whole number (44100 / 60 is 735, but 44100 /
// 59.94 is 735.7...). Truncating every frame would drop most of a sample each
// time, and the error accumulates into audible drift over minutes of play. So
// the fractional part is carried into the next frame rather than discarded.
double sample_debt = 0.0;

} // anonymous namespace

void render_audio(retro_audio_sample_batch_t audio_batch_cb)
{
	if (audio_batch_cb == nullptr)
		return;

	auto *audio = Module::getInstance<love::audio::openal::Audio>(Module::M_AUDIO);
	if (audio == nullptr)
		return;

	const double fps = state.fps > 0.0 ? state.fps : 60.0;

	double wanted = SAMPLE_RATE / fps + sample_debt;
	int samples = (int) wanted;
	sample_debt = wanted - (double) samples;

	if (samples <= 0)
		return;

	// Two shorts per sample frame (left, right).
	if ((int) buffer.size() < samples * 2)
		buffer.resize((size_t) samples * 2);

	audio->renderSamples(buffer.data(), samples);

	audio_batch_cb(buffer.data(), (size_t) samples);
}

} // namespace libretro
} // namespace love
