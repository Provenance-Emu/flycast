/*
    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "types.h"
#include "cfg/option.h"
#include "audio/audiostream.h"
#include "emulator.h"
#include "throttle.h"
#include "timestretch.h"

#include <libretro.h>

#include <vector>
#include <mutex>

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Detect output refresh rate changes by monitoring
 * the last 'VSYNC_SWAP_INTERVAL_FRAMES' frames:
 * - Measure average (mean) audio samples per upload
 *   operation
 * - Determine vsync swap interval based on
 *   expected samples at 60 (or 50) Hz
 * - Check that vsync swap interval remains
 *   'stable' for at least 'VSYNC_SWAP_INTERVAL_FRAMES' */
#define VSYNC_SWAP_INTERVAL_FRAMES 6
/* Calculated swap interval is 'valid' if it is
 * within 'VSYNC_SWAP_INTERVAL_THRESHOLD' of an integer
 * value */
#define VSYNC_SWAP_INTERVAL_THRESHOLD 0.05f

extern void setAVInfo(retro_system_av_info& avinfo);

extern retro_environment_t        environ_cb;
extern retro_audio_sample_batch_t audio_batch_cb;

extern float libretro_expected_audio_samples_per_run;
extern unsigned libretro_vsync_swap_interval;
extern bool libretro_detect_vsync_swap_interval;

static float audio_samples_per_frame_avg;
static unsigned vsync_swap_interval_last;
static unsigned vsync_swap_interval_conter;

static std::mutex audio_buffer_mutex;
static std::vector<int16_t> audio_buffer;
static size_t audio_buffer_idx;
static size_t audio_batch_frames_max;
static bool drop_samples = true;

static int16_t *audio_out_buffer = nullptr;

static TimeStretcher timeStretcher;
static std::vector<s16> stretch_buffer;
bool use_timestretch = true;

void retro_audio_init(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	/* Worst case is 25 fps content with an audio sample rate
	 * of 44.1 kHz -> 1764 stereo samples
	 * But flycast can stop rendering for arbitrary lengths of
	 * time, leading to multiple 'frames' worth of audio being
	 * uploaded in retro_run(). We therefore require some leniency,
	 * but must limit the total number of samples that can be
	 * uploaded since the libretro frontend can 'hang' if too
	 * many samples are sent during a single call of retro_run().
	 * We therefore (arbitrarily) choose to allow up to 10 frames
	 * worth of 'worst case' stereo samples... */
	size_t audio_buffer_size = (44100 / 25) * 2 * 10;

	audio_buffer.resize(audio_buffer_size);
	audio_buffer_idx = 0;
	audio_batch_frames_max = std::numeric_limits<size_t>::max();

	audio_out_buffer = (int16_t*)malloc(audio_buffer_size * sizeof(int16_t));
	stretch_buffer.resize(audio_buffer_size);

	drop_samples = false;

	audio_samples_per_frame_avg = 0.0f;
	vsync_swap_interval_last = 1;
	vsync_swap_interval_conter = 0;

	// Initialize time stretcher
	timeStretcher = TimeStretcher(2, 1024);
}

void retro_audio_deinit(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	audio_buffer.clear();
	audio_buffer_idx = 0;

	if (audio_out_buffer != nullptr)
		free(audio_out_buffer);

	audio_out_buffer = nullptr;

	drop_samples = true;

	audio_samples_per_frame_avg = 0.0f;
	vsync_swap_interval_last = 1;
	vsync_swap_interval_conter = 0;
}

void retro_audio_flush_buffer(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);
	audio_buffer_idx = 0;

	/* We are manually 'resetting' the audio buffer
	 * -> any 'drop samples' lock can be released */
	drop_samples = false;
}

void retro_audio_upload(void)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (audio_buffer_idx == 0)
		return;

	size_t num_frames = audio_buffer_idx >> 1;

	/* When running in unthrottled mode, use time stretching */
	if (use_timestretch && (throttle_state == RETRO_THROTTLE_UNBLOCKED ||
		throttle_state == RETRO_THROTTLE_FAST_FORWARD))
	{
		// Set stretch factor based on throttle state
		float stretch_factor = 0.5f; // Default to double speed

		if (throttle_rate > 0.0f && throttle_rate < 10.0f)
			stretch_factor = 1.0f / throttle_rate;

		timeStretcher.setStretchFactor(stretch_factor);

		// Copy audio buffer to a temporary buffer for processing
		std::vector<s16> temp_buffer(audio_buffer.begin(), audio_buffer.begin() + audio_buffer_idx);

		// Process audio through time stretcher
		int stretched_frames = timeStretcher.process(
			temp_buffer.data(),
			num_frames,
			(s16*)audio_out_buffer,
			audio_buffer.size() / 2);

		// Use the stretched audio
		if (stretched_frames > 0)
		{
			// Send the processed audio directly to the frontend
			audio_batch_cb(audio_out_buffer, stretched_frames);

			// Reset audio buffer
			audio_buffer_idx = 0;
			drop_samples = false;
			return;
		}
	}

	/* Update vsync swap interval detection */
	if (libretro_detect_vsync_swap_interval)
	{
		/* Simple running average (leaky-integrator) */
		audio_samples_per_frame_avg = ((1.0f / (float)VSYNC_SWAP_INTERVAL_FRAMES) * (float)num_frames) +
				((1.0f - (1.0f / (float)VSYNC_SWAP_INTERVAL_FRAMES)) * audio_samples_per_frame_avg);

		float swap_ratio = audio_samples_per_frame_avg /
				libretro_expected_audio_samples_per_run;
		unsigned swap_integer;
		float swap_remainder;

		/* If internal frame rate is equal to (within threshold)
		 * or higher than the default 60 (or 50) Hz, fall back
		 * to a swap interval of 1 */
		if (swap_ratio < (1.0f + VSYNC_SWAP_INTERVAL_THRESHOLD))
		{
			swap_integer = 1;
			swap_remainder = 0.0f;
		}
		else
		{
			swap_integer = (unsigned)(swap_ratio + 0.5f);
			swap_remainder = swap_ratio - (float)swap_integer;
			swap_remainder = (swap_remainder < 0.0f) ?
					-swap_remainder : swap_remainder;
		}

		/* > Swap interval is considered 'valid' if it is
		 *   within VSYNC_SWAP_INTERVAL_THRESHOLD of an integer
		 *   value
		 * > If valid, check if new swap interval differs from
		 *   previously logged value */
		if ((swap_remainder <= VSYNC_SWAP_INTERVAL_THRESHOLD) &&
			 (swap_integer != libretro_vsync_swap_interval))
		{
			vsync_swap_interval_conter =
					(swap_integer == vsync_swap_interval_last) ?
							(vsync_swap_interval_conter + 1) : 0;

			/* Check whether swap interval is 'stable' */
			if (vsync_swap_interval_conter >= VSYNC_SWAP_INTERVAL_FRAMES)
			{
				libretro_vsync_swap_interval = swap_integer;
				vsync_swap_interval_conter = 0;

				/* Notify frontend */
				retro_system_av_info avinfo;
				setAVInfo(avinfo);
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &avinfo);
			}

			vsync_swap_interval_last = swap_integer;
		}
		else
			vsync_swap_interval_conter = 0;
	}

	// Copy audio buffer to output buffer
	memcpy(audio_out_buffer, audio_buffer.data(), audio_buffer_idx * sizeof(s16));

	// Send audio to frontend
	audio_batch_cb(audio_out_buffer, num_frames);

	/* Reset audio buffer */
	audio_buffer_idx = 0;
	drop_samples = false;
}

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
void WriteSample(s16 r, s16 l)
{
	// Use a mutex to ensure thread safety
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (drop_samples)
		return;

	if (audio_buffer.size() < audio_buffer_idx + 2)
	{
		// Audio buffer overflow...
		audio_buffer_idx = 0;
		drop_samples = true;
		return;
	}

	// Store samples directly without noise
	audio_buffer[audio_buffer_idx++] = l;
	audio_buffer[audio_buffer_idx++] = r;
}
#else
// Original implementation
void WriteSample(s16 r, s16 l)
{
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (drop_samples)
		return;

	if (audio_buffer.size() < audio_buffer_idx + 2)
	{
		/* Audio buffer overflow...
		 * > Drop any existing samples
		 * > Drop any future samples until the next
		 *   call of retro_audio_upload() */
		audio_buffer_idx = 0;
		drop_samples = true;
		return;
	}

	audio_buffer[audio_buffer_idx++] = l;
	audio_buffer[audio_buffer_idx++] = r;
}
#endif

void InitAudio()
{
}

void TermAudio()
{
}

void StartAudioRecording(bool eight_khz)
{
}

u32 RecordAudio(void *buffer, u32 samples)
{
	return 0;
}

void StopAudioRecording()
{
}
