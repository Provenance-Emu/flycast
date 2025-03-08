#include "audiostream.h"
#include "cfg/option.h"
#include "emulator.h"

static void registerForEvents();

struct SoundFrame { s16 l; s16 r; };

static SoundFrame Buffer[SAMPLE_COUNT];
static u32 writePtr;  // next sample index

static AudioBackend *currentBackend;
std::vector<AudioBackend *> *AudioBackend::backends;

static bool audio_recording_started;
static bool eight_khz;

// Add audio prediction for CPU run-ahead
class AudioPredictor {
private:
	static const int HISTORY_SIZE = 16;
	std::array<s16, HISTORY_SIZE> left_history;
	std::array<s16, HISTORY_SIZE> right_history;
	int history_pos = 0;

public:
	void addSample(s16 l, s16 r) {
		left_history[history_pos] = l;
		right_history[history_pos] = r;
		history_pos = (history_pos + 1) % HISTORY_SIZE;
	}

	// Predict the next N audio samples based on recent history
	void predictSamples(int count, std::vector<s16>& output) {
		output.resize(count * 2);

		// Simple linear prediction
		s16 l_delta = 0, r_delta = 0;

		if (history_pos > 1) {
			int prev = (history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE;
			int prev2 = (history_pos - 2 + HISTORY_SIZE) % HISTORY_SIZE;
			l_delta = left_history[prev] - left_history[prev2];
			r_delta = right_history[prev] - right_history[prev2];
		}

		s16 last_l = left_history[(history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE];
		s16 last_r = right_history[(history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE];

		for (int i = 0; i < count; i++) {
			last_l += l_delta;
			last_r += r_delta;
			output[i*2] = last_l;
			output[i*2+1] = last_r;
		}
	}
};

static AudioPredictor audio_predictor;

AudioBackend *AudioBackend::getBackend(const std::string& slug)
{
	if (backends == nullptr)
		return nullptr;
	if (slug == "auto")
	{
		// Prefer sdl2 if available and avoid the null driver
		AudioBackend *sdlBackend = nullptr;
		AudioBackend *autoBackend = nullptr;
		for (auto backend : *backends)
		{
			if (backend->slug == "sdl2")
				sdlBackend = backend;
			if (backend->slug != "null" && autoBackend == nullptr)
				autoBackend = backend;
		}
		if (sdlBackend != nullptr)
			autoBackend = sdlBackend;
		if (autoBackend == nullptr)
			autoBackend = backends->front();
		INFO_LOG(AUDIO, "Auto-selected audio backend \"%s\" (%s).", autoBackend->slug.c_str(), autoBackend->name.c_str());

		return autoBackend;
	}
	for (auto backend : *backends)
	{
		if (backend->slug == slug)
			return backend;
	}
	WARN_LOG(AUDIO, "WARNING: Audio backend \"%s\" not found!", slug.c_str());
	return nullptr;
}

void WriteSample(s16 r, s16 l)
{
	Buffer[writePtr].r = r * config::AudioVolume.dbPower();
	Buffer[writePtr].l = l * config::AudioVolume.dbPower();

	if (++writePtr == SAMPLE_COUNT)
	{
		if (currentBackend != nullptr)
			currentBackend->push(Buffer, SAMPLE_COUNT, config::LimitFPS);
		writePtr = 0;
	}
}

void InitAudio()
{
	registerForEvents();
	TermAudio();

	std::string slug = config::AudioBackend;
	currentBackend = AudioBackend::getBackend(slug);
	if (currentBackend == nullptr && slug != "auto")
	{
		slug = "auto";
		currentBackend = AudioBackend::getBackend(slug);
	}
	if (currentBackend != nullptr)
	{
		INFO_LOG(AUDIO, "Initializing audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->name.c_str());
		if (!currentBackend->init())
		{
			currentBackend = nullptr;
			if (slug != "auto")
			{
				WARN_LOG(AUDIO, "Audio driver %s failed to initialize. Defaulting to 'auto'", slug.c_str());
				slug = "auto";
				currentBackend = AudioBackend::getBackend(slug);
				if (!currentBackend->init())
					currentBackend = nullptr;
			}
		}
	}

	if (currentBackend == nullptr)
	{
		WARN_LOG(AUDIO, "Running without audio!");
		return;
	}

	if (audio_recording_started)
	{
		// Restart recording
		audio_recording_started = false;
		StartAudioRecording(eight_khz);
	}
}

void TermAudio()
{
	if (currentBackend == nullptr)
		return;

	// Save recording state before stopping
	bool rec_started = audio_recording_started;
	StopAudioRecording();
	audio_recording_started = rec_started;
	currentBackend->term();
	INFO_LOG(AUDIO, "Terminating audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->name.c_str());
	currentBackend = nullptr;
}

void StartAudioRecording(bool eight_khz)
{
	::eight_khz = eight_khz;
	if (currentBackend != nullptr)
		audio_recording_started = currentBackend->initRecord(eight_khz ? 8000 : 11025);
	else
		// might be called between TermAudio/InitAudio
		audio_recording_started = true;
}

u32 RecordAudio(void *buffer, u32 samples)
{
	if (!audio_recording_started || currentBackend == nullptr)
		return 0;
	return currentBackend->record(buffer, samples);
}

void StopAudioRecording()
{
	// might be called between TermAudio/InitAudio
	if (audio_recording_started && currentBackend != nullptr)
		currentBackend->termRecord();
	audio_recording_started = false;
}

static void registerForEvents()
{
	static bool done;
	if (done)
		return;
	done = true;
	// Empty the audio buffer when loading a state or terminating the game
	const auto& callback = [](Event, void *) {
		writePtr = 0;
	};
	EventManager::listen(Event::Terminate, callback);
	EventManager::listen(Event::LoadState, callback);
}

// Use this in the CPU run-ahead function
bool Emulator::run_cpu_frame_with_audio_prediction() {
	// Run the CPU for one frame
	bool result = run_cpu_frame();

	// Predict audio for the next frame
	std::vector<s16> predicted_audio;
	audio_predictor.predictSamples(1764, predicted_audio); // 44100/25 samples

	// Store the predicted audio for later use
	predicted_audio_buffer = predicted_audio;

	return result;
}
