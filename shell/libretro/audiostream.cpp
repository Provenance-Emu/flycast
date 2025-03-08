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
#include <thread>
#include <atomic>
#include <condition_variable>

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

// Add a separate thread for audio processing
static std::thread audio_thread;
static std::atomic<bool> audio_thread_running;
static std::condition_variable audio_cv;
static std::mutex audio_thread_mutex;

void audio_thread_func()
{
	while (audio_thread_running)
	{
		// Wait for audio data or shutdown signal
		std::unique_lock<std::mutex> lock(audio_thread_mutex);
		audio_cv.wait(lock, []{ return !audio_thread_running || audio_buffer_idx > 0; });

		if (!audio_thread_running)
			break;

		// Process audio in a separate thread
		if (audio_buffer_idx > 0)
		{
			size_t num_frames = audio_buffer_idx >> 1;

			// Copy audio data to a local buffer
			std::vector<s16> local_buffer(audio_buffer.begin(), audio_buffer.begin() + audio_buffer_idx);

			// Reset the main audio buffer
			audio_buffer_idx = 0;
			drop_samples = false;

			// Release the lock while processing audio
			lock.unlock();

			// Process audio (time stretching, etc.)
			if (use_timestretch && (throttle_state == RETRO_THROTTLE_UNBLOCKED ||
				throttle_state == RETRO_THROTTLE_FAST_FORWARD))
			{
				// ... time stretching code ...
			}

			// Send audio to frontend
			audio_batch_cb(local_buffer.data(), num_frames);
		}
	}
}

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

	// Debug output to check if we're getting audio samples
	DEBUG_LOG(AUDIO, "Audio upload: %d frames", (int)num_frames);

	// In sync mode, use a larger buffer size to reduce CPU overhead
	if (throttle_state == RETRO_THROTTLE_NORMAL)
	{
		// Use a larger buffer size in sync mode
		static const size_t MAX_FRAMES_PER_BATCH = 1024;

		// Send audio in larger batches
		size_t frames_sent = 0;
		while (frames_sent < num_frames)
		{
			size_t frames_to_send = std::min(MAX_FRAMES_PER_BATCH, num_frames - frames_sent);
			audio_batch_cb(audio_buffer.data() + frames_sent * 2, frames_to_send);
			frames_sent += frames_to_send;
		}
	}
	else if (throttle_state == RETRO_THROTTLE_UNBLOCKED ||
			 throttle_state == RETRO_THROTTLE_FAST_FORWARD)
	{
		// In unthrottled mode, use time stretching or sample dropping
		if (!use_timestretch)
		{
			// Simple sample dropping - keep only 1/4 of the samples
			size_t output_frames = 0;
			for (size_t i = 0; i < num_frames; i += 4)
			{
				if (i < num_frames)
				{
					audio_out_buffer[output_frames * 2] = audio_buffer[i * 2];
					audio_out_buffer[output_frames * 2 + 1] = audio_buffer[i * 2 + 1];
					output_frames++;
				}
			}

			if (output_frames > 0)
			{
				DEBUG_LOG(AUDIO, "Sending %d frames (dropped from %d)", (int)output_frames, (int)num_frames);
				audio_batch_cb(audio_out_buffer, output_frames);
			}
		}
		else
		{
			// Use time stretching
			float stretch_factor = 0.25f; // Very aggressive for unthrottled mode

			if (throttle_rate > 0.0f && throttle_rate < 10.0f)
				stretch_factor = 1.0f / throttle_rate;

			timeStretcher.setStretchFactor(stretch_factor);

			// Process audio through time stretcher
			int stretched_frames = timeStretcher.process(
				(s16*)audio_buffer.data(),
				num_frames,
				(s16*)audio_out_buffer,
				audio_buffer.size() / 2);

			if (stretched_frames > 0)
			{
				DEBUG_LOG(AUDIO, "Sending %d stretched frames (from %d)", stretched_frames, (int)num_frames);
				audio_batch_cb(audio_out_buffer, stretched_frames);
			}
		}
	}
	else
	{
		// In normal mode, send audio directly
		DEBUG_LOG(AUDIO, "Sending %d frames directly", (int)num_frames);
		audio_batch_cb(audio_buffer.data(), num_frames);
	}

	// Reset audio buffer
	audio_buffer_idx = 0;
	drop_samples = false;
}

// Add this function to handle audio in synced mode
void WriteSample(s16 r, s16 l)
{
	// Use a mutex to ensure thread safety
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (drop_samples)
		return;

	// Check for buffer overflow
	if (audio_buffer.size() < audio_buffer_idx + 2)
	{
		// Audio buffer overflow...
		audio_buffer_idx = 0;
		drop_samples = true;
		return;
	}

	// Store samples directly - no special handling based on throttle state
	// This ensures we always capture audio samples
	audio_buffer[audio_buffer_idx++] = l;
	audio_buffer[audio_buffer_idx++] = r;
}

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
