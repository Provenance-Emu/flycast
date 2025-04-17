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


// Add a separate thread for audio processing
static std::thread audio_thread;
static std::atomic<bool> audio_thread_running;
static std::condition_variable audio_cv;
static std::mutex audio_thread_mutex;

// Forward declarations for NEON-optimized functions
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
static void process_audio_samples_neon(s16 *output, const s16 *input, int count);
#endif

// Class for adaptive audio quality
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

			// Send audio to frontend
			audio_batch_cb(local_buffer.data(), num_frames);
		}
	}
}

// Add these variables at the top with other globals
static std::atomic<float> buffer_fullness{0.5f};
static size_t optimal_buffer_size = (44100 / 60) * 2 * 3; // 3 frames worth at 60Hz

// Function to get the current audio buffer fullness
// Returns a value between 0.0 (empty) and 1.0 (full)
float getAudioBufferFullness() {
    return buffer_fullness.load(std::memory_order_relaxed);
}

// Define a single consistent buffer size
#define AUDIO_BUFFER_SIZE 8192

// Global audio buffer variables
static s16 audio_ring_buffer[AUDIO_BUFFER_SIZE];
static std::atomic<size_t> write_pos(0);
static std::atomic<size_t> read_pos(0);

// Helper functions to access the audio buffer
static s16* getTempBuffer() {
	return audio_ring_buffer;
}

static std::atomic<size_t>& getWritePos() {
	return write_pos;
}

static std::atomic<size_t>& getReadPos() {
	return read_pos;
}

// void WriteSample(s16 r, s16 l)
// {
// 	// Calculate next write position
// 	size_t current_write = write_pos.load(std::memory_order_relaxed);
// 	size_t next_write = (current_write + 2) % AUDIO_BUFFER_SIZE;

// 	// Check if buffer is full
// 	size_t current_read = read_pos.load(std::memory_order_acquire);
// 	if (next_write == current_read)
// 		return; // Buffer full, drop sample

// 	// Write sample to buffer
// 	audio_ring_buffer[current_write] = l;
// 	audio_ring_buffer[current_write + 1] = r;

// 	// Update write position
// 	write_pos.store(next_write, std::memory_order_release);
// }

void retro_audio_upload(void)
{
	// Get current positions
	size_t current_write = write_pos.load(std::memory_order_acquire);
	size_t current_read = read_pos.load(std::memory_order_relaxed);

	// Calculate available frames
	size_t available;
	if (current_write >= current_read)
		available = (current_write - current_read) / 2;
	else
		available = (AUDIO_BUFFER_SIZE - current_read + current_write) / 2;

	// Update buffer fullness
	float new_fullness = static_cast<float>(available) / AUDIO_BUFFER_SIZE;
	buffer_fullness.store(new_fullness, std::memory_order_relaxed);

	if (available == 0)
		return;

	// Limit to a reasonable batch size
	size_t frames_to_process = std::min(available, (size_t)1024);

	// Copy to a contiguous buffer for processing
	static s16 temp_buffer[2048];
	size_t frames_copied = 0;

	while (frames_copied < frames_to_process)
	{
		// Copy from ring buffer to temp buffer
		size_t frames_to_copy = std::min(frames_to_process - frames_copied,
										(AUDIO_BUFFER_SIZE - current_read) / 2);

		memcpy(temp_buffer + frames_copied * 2,
			   audio_ring_buffer + current_read,
			   frames_to_copy * 4);

		// Update read position
		current_read = (current_read + frames_to_copy * 2) % AUDIO_BUFFER_SIZE;
		frames_copied += frames_to_copy;
	}

	// Update read position atomically
	read_pos.store(current_read, std::memory_order_release);

	// Send to frontend
	if (frames_copied > 0)
		audio_batch_cb(temp_buffer, frames_copied);
}

void retro_audio_init(void)
{
	// Initialize ring buffer
	for (size_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
		audio_ring_buffer[i] = 0;

	write_pos.store(0, std::memory_order_relaxed);
	read_pos.store(0, std::memory_order_relaxed);
}

void retro_audio_deinit(void)
{
}

void retro_audio_flush_buffer(void)
{
}

// NEON-optimized audio processing functions
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
static void process_audio_samples_neon(s16 *output, const s16 *input, int count)
{
	// Process 8 stereo samples (16 values) at a time
	int i = 0;
	for (; i < count - 7; i += 8)
	{
		// Load 8 stereo samples (16 values)
		int16x8x2_t stereo_samples = vld2q_s16(input + i * 2);

		// No volume scaling - just copy the samples
		vst2q_s16(output + i * 2, stereo_samples);
	}

	// Process remaining samples
	for (; i < count; i++)
	{
		output[i*2] = input[i*2];
		output[i*2+1] = input[i*2+1];
	}
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
