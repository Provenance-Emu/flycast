#include "audiostream.h"
#include "audio_thread.h"
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
	if (currentBackend == nullptr)
		return;

	Buffer[writePtr].r = r;
	Buffer[writePtr].l = l;
	writePtr = (writePtr + 1) % SAMPLE_COUNT;

	if (writePtr == 0)
	{
		// Process any pending samples from the audio thread buffer
		ProcessAudioThreadBuffer();
		
		// Push the buffer to the audio backend
		currentBackend->push(Buffer, SAMPLE_COUNT, false);
		
		// Update buffer fullness
		float new_fullness = static_cast<float>(AUDIO_BUFFER_SIZE) / AUDIO_BUFFER_SIZE;
		buffer_fullness.store(new_fullness, std::memory_order_relaxed);
	}
}

// Forward declarations for audio thread functions
void InitAudioThread();
void TermAudioThread();
void ProcessAudioThreadBuffer();
void RequestAudioThreadProcessing();

void InitAudio()
{
	registerForEvents();

	if (currentBackend != nullptr)
		return;

	string audioBackend = config::AudioBackend.get();
	currentBackend = AudioBackend::getBackend(audioBackend);
	if (currentBackend == nullptr)
	{
		WARN_LOG(AUDIO, "WARNING: Selected audio backend \"%s\" not found. Using auto selection.", audioBackend.c_str());
		currentBackend = AudioBackend::getBackend("auto");
	}
	if (currentBackend == nullptr)
	{
		ERROR_LOG(AUDIO, "FATAL: No audio backends available.");
		return;
	}

	if (!currentBackend->init())
	{
		ERROR_LOG(AUDIO, "FATAL: Audio backend \"%s\" (%s) initialization failed.", currentBackend->slug.c_str(), currentBackend->name.c_str());
		currentBackend = nullptr;
		return;
	}

	INFO_LOG(AUDIO, "Audio backend \"%s\" (%s) initialized.", currentBackend->slug.c_str(), currentBackend->name.c_str());

	writePtr = 0;
	memset(Buffer, 0, sizeof(Buffer));
	
	// Initialize the audio thread system
	InitAudioThread();
}

void TermAudio()
{
	// Terminate the audio thread system
	TermAudioThread();
	
	if (currentBackend != nullptr)
	{
		currentBackend->term();
		currentBackend = nullptr;
	}
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
	// Function to get the current audio buffer fullness
// Returns a value between 0.0 (empty) and 1.0 (full)
float getAudioBufferFullness() {
    // If audio thread is running, use its buffer fullness
    if (AudioThread::isRunning()) {
        return AudioThread::getBufferFullness();
    }
    // Otherwise use the traditional buffer fullness
    return buffer_fullness.load(std::memory_order_relaxed);
}	};
	EventManager::listen(Event::Terminate, callback);
	EventManager::listen(Event::LoadState, callback);
}
