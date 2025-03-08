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
#include "hw/sh4/sh4_sched.h"

#include <libretro.h>

#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <array>

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

// Increase the audio buffer size for sync mode
static const size_t MAX_FRAMES_PER_BATCH_NORMAL = 1024;
static const size_t MAX_FRAMES_PER_BATCH_SYNC = 2048;  // Larger buffer for sync mode

// Add a lockless ring buffer for audio samples
template<typename T, size_t Size>
class LocklessRingBuffer {
private:
	std::atomic<size_t> write_idx{0};
	std::atomic<size_t> read_idx{0};
	std::array<T, Size> buffer;

public:
	bool push(const T& item) {
		size_t current_write = write_idx.load(std::memory_order_relaxed);
		size_t next_write = (current_write + 1) % Size;
		if (next_write == read_idx.load(std::memory_order_acquire))
			return false; // Buffer full

		buffer[current_write] = item;
		write_idx.store(next_write, std::memory_order_release);
		return true;
	}

	bool pop(T& item) {
		size_t current_read = read_idx.load(std::memory_order_relaxed);
		if (current_read == write_idx.load(std::memory_order_acquire))
			return false; // Buffer empty

		item = buffer[current_read];
		read_idx.store((current_read + 1) % Size, std::memory_order_release);
		return true;
	}
};

// Create a lockless ring buffer for audio samples
static LocklessRingBuffer<s16, 8192> audio_ring_buffer;

// Add dynamic resampling based on CPU load
class DynamicResampler {
private:
	float target_ratio = 1.0f;
	float current_ratio = 1.0f;
	float phase = 0.0f;
	s16 prev_l = 0, prev_r = 0;

public:
	void setTargetRatio(float ratio) {
		target_ratio = ratio;
		// Gradually approach target ratio
		current_ratio = current_ratio * 0.95f + target_ratio * 0.05f;
	}

	// Resample input buffer to output buffer with current ratio
	int resample(const s16* input, int input_frames, s16* output, int max_output_frames) {
		int output_frames = 0;
		phase = 0.0f;

		while (output_frames < max_output_frames) {
			int input_idx = static_cast<int>(phase);
			if (input_idx >= input_frames - 1)
				break;

			float frac = phase - input_idx;

			// Linear interpolation for each channel
			for (int ch = 0; ch < 2; ch++) {
				float sample1 = static_cast<float>(input[input_idx * 2 + ch]);
				float sample2 = static_cast<float>(input[(input_idx + 1) * 2 + ch]);
				float interpolated = sample1 + frac * (sample2 - sample1);
				output[output_frames * 2 + ch] = static_cast<s16>(interpolated);
			}

			phase += current_ratio;
			output_frames++;
		}

		return output_frames;
	}
};

static DynamicResampler dynamic_resampler;

// Add audio frame skipping for better performance
class AudioFrameSkipper {
private:
	int skip_counter = 0;
	int skip_frames = 0;
	std::vector<s16> last_frame;

public:
	void setSkipFrames(int frames) {
		skip_frames = frames;
	}

	int getSkipFrames() const {
		return skip_frames;
	}

	bool shouldSkip() {
		if (skip_frames <= 0)
			return false;

		skip_counter = (skip_counter + 1) % (skip_frames + 1);
		return skip_counter != 0;
	}

	void saveLastFrame(const s16* data, int frames) {
		last_frame.resize(frames * 2);
		memcpy(last_frame.data(), data, frames * 2 * sizeof(s16));
	}

	const s16* getLastFrame() const {
		return last_frame.data();
	}

	int getLastFrameSize() const {
		return last_frame.size() / 2;
	}
};

static AudioFrameSkipper audio_frame_skipper;

// Dedicated audio thread function
static void audio_thread_func() {
	std::vector<s16> temp_buffer(2048);

	while (audio_thread_running) {
		size_t samples_read = 0;
		s16 sample;

		// Try to fill the temp buffer with samples from the ring buffer
		while (samples_read < temp_buffer.size() && audio_ring_buffer.pop(sample)) {
			temp_buffer[samples_read++] = sample;
		}

		if (samples_read > 0) {
			// Process and send audio to frontend
			if (samples_read % 2 != 0)
				samples_read--; // Ensure even number of samples (stereo)

			size_t frames = samples_read / 2;

			if (throttle_state == RETRO_THROTTLE_UNBLOCKED ||
				throttle_state == RETRO_THROTTLE_FAST_FORWARD) {
				// Apply time stretching for fast-forward
				// ...
			}

			audio_batch_cb(temp_buffer.data(), frames);
		}

		// Sleep a bit to avoid spinning too much
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

	// Start audio thread
	audio_thread_running = true;
	audio_thread = std::thread(audio_thread_func);
}

void retro_audio_deinit(void)
{
	// Stop audio thread
	audio_thread_running = false;
	if (audio_thread.joinable())
		audio_thread.join();

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

	// In sync mode, use a larger buffer size and process less frequently
	if (throttle_state == RETRO_THROTTLE_NORMAL)
	{
		// Use a much larger buffer size in sync mode
		static const size_t MAX_FRAMES_PER_BATCH = 4096;

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
		// In unthrottled mode, drop more samples
		size_t output_frames = 0;
		for (size_t i = 0; i < num_frames; i += 8) // Drop 7/8 of samples
		{
			if (i < num_frames)
			{
				audio_out_buffer[output_frames * 2] = audio_buffer[i * 2];
				audio_out_buffer[output_frames * 2 + 1] = audio_buffer[i * 2 + 1];
				output_frames++;
			}
		}

		if (output_frames > 0)
			audio_batch_cb(audio_out_buffer, output_frames);
	}
	else
	{
		// In normal mode, send audio directly
		audio_batch_cb(audio_buffer.data(), num_frames);
	}

	// Reset audio buffer
	audio_buffer_idx = 0;
	drop_samples = false;
}

// Optimize the WriteSample function
void WriteSample(s16 r, s16 l)
{
	// Use a mutex to ensure thread safety
	const std::lock_guard<std::mutex> lock(audio_buffer_mutex);

	if (drop_samples)
		return;

	// In throttled mode, downsample audio to reduce CPU load
	if (throttle_state == RETRO_THROTTLE_NORMAL)
	{
		// Only process every other sample
		static bool skip_sample = false;
		skip_sample = !skip_sample;
		if (skip_sample)
			return;
	}
	else if (throttle_state == RETRO_THROTTLE_UNBLOCKED ||
			 throttle_state == RETRO_THROTTLE_FAST_FORWARD)
	{
		// In unthrottled mode, drop even more samples
		static int sample_counter = 0;
		if (++sample_counter % 4 != 0) // Keep only 1/4 of samples
			return;
	}

	// Check for buffer overflow
	if (audio_buffer.size() < audio_buffer_idx + 2)
	{
		// Audio buffer overflow...
		audio_buffer_idx = 0;
		drop_samples = true;
		return;
	}

	// Store samples
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

// Add audio compression for CPU run-ahead
class AudioCompressor {
private:
	static const int BLOCK_SIZE = 32;

public:
	// Compress audio data to reduce memory usage during run-ahead
	std::vector<u8> compress(const s16* data, int frames) {
		std::vector<u8> compressed;
		compressed.reserve(frames * 2 / 4); // Estimate compressed size

		for (int i = 0; i < frames; i += BLOCK_SIZE) {
			int block_frames = std::min(BLOCK_SIZE, frames - i);

			// Find min/max values for this block
			s16 min_l = 32767, max_l = -32768;
			s16 min_r = 32767, max_r = -32768;

			for (int j = 0; j < block_frames; j++) {
				s16 l = data[(i + j) * 2];
				s16 r = data[(i + j) * 2 + 1];

				min_l = std::min(min_l, l);
				max_l = std::max(max_l, l);
				min_r = std::min(min_r, r);
				max_r = std::max(max_r, r);
			}

			// Store min/max values
			compressed.push_back(min_l & 0xFF);
			compressed.push_back((min_l >> 8) & 0xFF);
			compressed.push_back(max_l & 0xFF);
			compressed.push_back((max_l >> 8) & 0xFF);
			compressed.push_back(min_r & 0xFF);
			compressed.push_back((min_r >> 8) & 0xFF);
			compressed.push_back(max_r & 0xFF);
			compressed.push_back((max_r >> 8) & 0xFF);

			// Store quantized samples
			for (int j = 0; j < block_frames; j++) {
				s16 l = data[(i + j) * 2];
				s16 r = data[(i + j) * 2 + 1];

				// Quantize to 4 bits per channel
				u8 l_quant = (l - min_l) * 15 / (max_l - min_l + 1);
				u8 r_quant = (r - min_r) * 15 / (max_r - min_r + 1);

				// Pack two samples into one byte
				compressed.push_back((l_quant << 4) | r_quant);
			}
		}

		return compressed;
	}

	// Decompress audio data
	std::vector<s16> decompress(const std::vector<u8>& compressed) {
		std::vector<s16> decompressed;

		size_t pos = 0;
		while (pos + 8 < compressed.size()) {
			// Read min/max values
			s16 min_l = compressed[pos] | (compressed[pos + 1] << 8);
			s16 max_l = compressed[pos + 2] | (compressed[pos + 3] << 8);
			s16 min_r = compressed[pos + 4] | (compressed[pos + 5] << 8);
			s16 max_r = compressed[pos + 6] | (compressed[pos + 7] << 8);
			pos += 8;

			// Read quantized samples
			int block_samples = std::min<int>(BLOCK_SIZE, (compressed.size() - pos));

			for (int i = 0; i < block_samples; i++) {
				if (pos >= compressed.size())
					break;

				u8 packed = compressed[pos++];
				u8 l_quant = packed >> 4;
				u8 r_quant = packed & 0x0F;

				// Dequantize
				s16 l = min_l + l_quant * (max_l - min_l) / 15;
				s16 r = min_r + r_quant * (max_r - min_r) / 15;

				decompressed.push_back(l);
				decompressed.push_back(r);
			}
		}

		return decompressed;
	}
};

static AudioCompressor audio_compressor;

// Use this in CPU run-ahead to save memory
std::vector<u8> compressed_audio_buffer;

// Add this class definition at the top of the file
class AdaptiveAudioQuality {
private:
	enum QualityLevel {
		HIGH,   // Full quality, 44.1kHz stereo
		MEDIUM, // Medium quality, 22.05kHz stereo
		LOW     // Low quality, 11.025kHz mono
	};

	QualityLevel current_level = HIGH;
	float cpu_load = 0.0f;

public:
	void updateCpuLoad(float load) {
		// Smooth the CPU load value
		cpu_load = cpu_load * 0.9f + load * 0.1f;

		// Adjust quality level based on CPU load
		if (cpu_load > 0.9f) {
			current_level = LOW;
		} else if (cpu_load > 0.7f) {
			current_level = MEDIUM;
		} else {
			current_level = HIGH;
		}
	}

	// Process audio based on current quality level
	int process(const s16* input, int input_frames, s16* output, int max_output_frames) {
		switch (current_level) {
			case HIGH:
				// Full quality, just copy
				memcpy(output, input, std::min(input_frames, max_output_frames) * 2 * sizeof(s16));
				return std::min(input_frames, max_output_frames);

			case MEDIUM:
				// Medium quality, downsample to 22.05kHz
				for (int i = 0; i < input_frames / 2 && i < max_output_frames; i++) {
					output[i*2] = input[i*4];
					output[i*2+1] = input[i*4+1];
				}
				return input_frames / 2;

			case LOW:
				// Low quality, downsample to 11.025kHz mono
				for (int i = 0; i < input_frames / 4 && i < max_output_frames; i++) {
					s16 mono = (input[i*8] + input[i*8+1]) / 2;
					output[i*2] = mono;
					output[i*2+1] = mono;
				}
				return input_frames / 4;
		}

		return 0;
	}
};

static AdaptiveAudioQuality adaptive_audio_quality;
