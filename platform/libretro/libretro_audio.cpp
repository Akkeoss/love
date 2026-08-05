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

	// ONE frame's worth of samples per frame -- always, whatever the frame just
	// cost in wall-clock time. That is the libretro contract, and it is not an
	// approximation to be improved on: the frontend derives its playback rate
	// from this figure, so a core that varies it is telling the frontend the
	// audio clock itself changed speed.
	//
	// This was tried the other way. Measuring the real time the frame took and
	// rendering that much audio ("catch up on the sound owed during a stall")
	// is intuitive and wrong: RetroArch's dynamic rate control continuously
	// resamples to keep its buffer at a target level, and it reads a varying
	// sample count as a drifting clock to correct for. It then chases a target
	// that moves again the next frame. Measured with the headless tester on a
	// real game, the per-frame count ranged 735..4410 with 100% of batches
	// off-rate -- audible as a brief crackle and visible as a hitch, on x86 and
	// ARM alike, which is exactly the "micro-stutter on every board" symptom
	// that sent us looking for a driver or shader bug that was never there.
	// Holding the count fixed puts it at exactly 735 every frame, 0% off-rate.
	//
	// A frame that genuinely overran is handled by the frontend, which is the
	// only component that can: it has the buffer and the resampler. The core's
	// job is to keep the meaning of "one frame" constant.
	const double elapsed = 1.0 / fps;

	double wanted = SAMPLE_RATE * elapsed + sample_debt;
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
