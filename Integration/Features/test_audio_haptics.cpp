// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
// Project: GamepadCore
// Description: Integration test for Audio Haptics using a .wav file as input.
// Reference: Based on AudioHapticsListener implementation for USB/BT audio processing.

#ifdef BUILD_GAMEPAD_CORE_TESTS
#include "GCore/Utils/SoDefines.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

// miniaudio for audio playback and WAV decoding
#if GAMEPAD_CORE_HAS_AUDIO
#include "miniaudio.h"
#endif
#include "GCore/Interfaces/IPlatformHardware.h"
#include "GCore/Interfaces/Segregations/IGamepadBase.h"
#include "GCore/Interfaces/Segregations/IGamepadHaptics.h"
#include "GCore/Templates/TBasicDeviceRegistry.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "opus.h"
#include "test_utils.h"

static OpusEncoder* encoder;
struct BTPacket
{
	std::vector<uint8_t> haptics; // 3000Hz data
	std::vector<float> opus;    // 200 data
	std::vector<uint8_t> signal;    // 200 data
};

struct samplesSend {
	BTPacket pack1, pack2;
};

// ============================================================================
// Audio Haptics Constants (Based on AudioHapticsListener)
// ============================================================================
constexpr float kLowPassAlpha = 1.0f;
constexpr float kOneMinusAlpha = 1.0f - kLowPassAlpha;

constexpr float kLowPassAlphaBt = 1.0f;
constexpr float kOneMinusAlphaBt = 1.0f - kLowPassAlphaBt;

// ============================================================================
// Thread-safe queue for audio packets
// ============================================================================
template<typename T>
class thread_safe_queue
{
public:
	void push(const T& item)
	{
		gc_lock::lock_guard<gc_lock::mutex> lock(mMutex);
		mQueue.push(item);
	}

	bool pop(T& item)
	{
		gc_lock::lock_guard<gc_lock::mutex> lock(mMutex);
		if (mQueue.empty())
		{
			return false;
		}
		item = mQueue.front();
		mQueue.pop();
		return true;
	}

	bool empty()
	{
		gc_lock::lock_guard<gc_lock::mutex> lock(mMutex);
		return mQueue.empty();
	}

private:
	std::queue<T> mQueue;
	gc_lock::mutex mMutex;
};

// ============================================================================
// Global state for audio callback
// ============================================================================
struct audio_callback_data
{
#if GAMEPAD_CORE_HAS_AUDIO
	ma_decoder* pDecoder = nullptr;
#else
	void* pDecoder = nullptr;
#endif
	bool bIsSystemAudio = false;
	float LowPassStateLeft = 0.0f;
	float LowPassStateRight = 0.0f;
	bool bFinished = false;
	int framesPlayed = 0;
	bool bIsWireless = false;

	// Queues for haptics (like AudioHapticsListener)
	thread_safe_queue<BTPacket> btPacketQueue;
	thread_safe_queue<std::vector<std::vector<float>>> btAccumulator;
	thread_safe_queue<std::vector<int16_t>> usbSampleQueue;
	// Accumulator for Bluetooth - need 480 frames for 10ms Opus frame @ 48kHz
};

// Audio callback - plays audio on speakers and queues haptics data
#if GAMEPAD_CORE_HAS_AUDIO
void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	auto* pData = static_cast<audio_callback_data*>(pDevice->pUserData);
	if (!pData)
	{
		return;
	}

	pData->framesPlayed += frameCount;

	uint64_t framesRead = 0;
	std::vector<float> tempBuffer(frameCount * 2 * sizeof(float));
	if (pData->bIsSystemAudio)
	{
		// Capture from system audio (loopback)
		if (pInput == nullptr)
		{
			return;
		}

		// const float* samples = static_cast<const float*>(pInput);
		// std::cout << "Sample L: " << samples[0] << " | Sample R: " << samples[1] << std::endl;

		static int fk = 0;
		static std::vector<std::vector<float>> dualChannel = {};
		if (fk % 2 == 0 && dualChannel.size() == 2)
		{
			// miniaudio already provides the captured audio in pInput
			std::cout << "System audio captured" << std::endl;
			pData->btAccumulator.push(dualChannel);
			dualChannel.clear();
		}

		dualChannel.push_back(std::vector<float>(static_cast<const float*>(pInput), static_cast<const float*>(pInput) + frameCount * 2));
	}
	else
	{
		// Read from decoder
		if (!pData->pDecoder)
		{
			if (pOutput)
			{
				std::memset(pOutput, 0, frameCount * pDevice->playback.channels * ma_get_bytes_per_sample(pDevice->playback.format));
			}
			return;
		}

		ma_result result = ma_decoder_read_pcm_frames(pData->pDecoder, tempBuffer.data(), frameCount, &framesRead);
		if (result != MA_SUCCESS || framesRead == 0)
		{
			pData->bFinished = true;
			if (pOutput)
			{
				std::memset(pOutput, 0, frameCount * pDevice->playback.channels * ma_get_bytes_per_sample(pDevice->playback.format));
			}
			return;
		}
	}


	if (!pData->bIsWireless)
	{
		// USB: Queue 16-bit stereo samples with high-pass filter
		for (ma_uint64 i = 0; i < frameCount; ++i)
		{
			float inLeft = tempBuffer[i * 2];
			float inRight = tempBuffer[i * 2 + 1];

			pData->LowPassStateLeft = kOneMinusAlpha * inLeft + kLowPassAlpha * pData->LowPassStateLeft;
			pData->LowPassStateRight = kOneMinusAlpha * inRight + kLowPassAlpha * pData->LowPassStateRight;

			float outLeft = std::clamp(inLeft - pData->LowPassStateLeft, -1.0f, 1.0f);
			float outRight = std::clamp(inRight - pData->LowPassStateRight, -1.0f, 1.0f);

			std::vector<int16_t> stereoSample = {
				static_cast<int16_t>(outLeft * 32767.0f),
				static_cast<int16_t>(outRight * 32767.0f)};
			pData->usbSampleQueue.push(stereoSample);
		}
	}

	// Process for haptics
	if (pData->bIsWireless)
	{
		// samplesSend samples;
		std::vector<std::vector<float>> item;
		while (pData->btAccumulator.pop(item))
		{
			std::vector<std::vector<BTPacket>> packs;
			packs.reserve(item.size());

			std::cout << "item size: " << item.size() << std::endl;
			for (int bt = 0; bt < item.size(); bt++)
			{
				std::vector<float> processed;
				for (int i = 0; i < frameCount; i++) {
					float inLeft = item[bt][i * 2];
					float inRight = item[bt][i * 2 + 1];

					pData->LowPassStateLeft = kOneMinusAlpha * inLeft + kLowPassAlpha * pData->LowPassStateLeft;
					pData->LowPassStateRight = kOneMinusAlpha * inRight + kLowPassAlpha * pData->LowPassStateRight;

					if (std::isnan(inLeft) || std::isnan(inRight))
						continue;

					float outLeft = std::clamp(inLeft - pData->LowPassStateLeft, -1.0f, 1.0f);
					float outRight = std::clamp(inRight - pData->LowPassStateRight, -1.0f, 1.0f);
					processed.push_back(outLeft);
					processed.push_back(outRight);
				}

				std::cout << "Opus encoding: " <<  std::endl;
				std::vector<uint8_t>  opData = std::vector<uint8_t>(200);
				int encodedBytes = opus_encode_float(encoder, processed.data(), processed.size() / 2, opData.data(), 200);
				if (encodedBytes <= 0)
				{
					std::cout << "Opus encoding failed: " << encodedBytes << std::endl;
					continue;
				}

				std::vector<uint8_t> hapticsData(60);

				BTPacket packet;
				packet.opus = processed;
				packet.signal = opData;
				packet.haptics = hapticsData;
				packs.push_back({packet});
				std::cout << "btAccumulator popped" << std::endl;
				//pData->btPacketQueue.push(packet);
				std::cout << "Packet queued" << std::endl;
			}
			// std::cout << "Pack size: " << packs.size() << std::endl;
			pData->btPacketQueue.push(packs[0][0]);
			pData->btPacketQueue.push(packs[1][0]);
			// pData->btPacketQueue.push(packs[2][0]);
			// pData->btPacketQueue.push(packs[3][0]);
		}
	}
}
#endif

// Consume haptics queue and send to controller
void consume_haptics_queue(IGamepadHaptics* AudioHaptics, audio_callback_data& callbackData)
{
	if (callbackData.bIsWireless)
	{
		BTPacket chunk;
		while (callbackData.btPacketQueue.pop(chunk))
		{
			AudioHaptics->AudioHapticUpdate(chunk.haptics, chunk.signal);
		}
	}
	else
	{
		std::vector<std::int16_t> allSamples;
		allSamples.reserve(2048 * 2);

		std::vector<std::int16_t> stereoSample;
		while (callbackData.usbSampleQueue.pop(stereoSample))
		{
			if (stereoSample.size() >= 2)
			{
				allSamples.push_back(stereoSample[0]);
				allSamples.push_back(stereoSample[1]);
			}
		}

		if (!allSamples.empty())
		{
			AudioHaptics->AudioHapticUpdate(allSamples);
		}
	}
}

// ============================================================================
// GamepadBase Audio Worker - Manages audio/haptics for a single controller
// ============================================================================
class gamepad_audio_worker
{
public:
	gamepad_audio_worker(IGamepadBase* InGamepad, const std::string& InWavPath, bool InUseSystemAudio)
	    : Gamepad(InGamepad)
	    , WavFilePath(InWavPath)
	    , bUseSystemAudio(InUseSystemAudio)
	{
		bFinished.store(false);
	}

	~gamepad_audio_worker()
	{
		stop();
	}

	void start()
	{
		WorkerThread = std::thread(&gamepad_audio_worker::run, this);
	}

	void stop()
	{
		bFinished.store(true);
		if (WorkerThread.joinable())
		{
			WorkerThread.join();
		}
	}

	bool is_finished() const { return bFinished.load(); }

private:
	void run()
	{
		if (!Gamepad)
		{
			return;
		}

		bool bIsWireless = Gamepad->GetConnectionType() == EDSDeviceConnection::Bluetooth;

		// Get Audio Haptics interface
		IGamepadHaptics* AudioHaptics = Gamepad->GetIGamepadHaptics();
		if (!AudioHaptics)
		{
			std::cerr << "[Worker Error] Audio haptics interface not available." << std::endl;
			return;
		}

		// Initialize AudioContext for USB haptics
		FDeviceContext* Context = Gamepad->GetMutableDeviceContext();
		if (!bIsWireless && Context)
		{
			IAudioDevice::Get().InitializeAudioContainer(Context);
		}

#if GAMEPAD_CORE_HAS_AUDIO
		ma_decoder decoder;
		ma_uint64 totalFrames = 0;
#endif
		bool bDecoderInitialized = false;

		if (!bUseSystemAudio)
		{
			fs::path p(WavFilePath);
			if (!p.is_absolute() && !fs::exists(p))
			{
				fs::path alternativePath = fs::path(GAMEPAD_CORE_PROJECT_ROOT) / WavFilePath;
				if (fs::exists(alternativePath))
				{
					WavFilePath = alternativePath.string();
					std::cout << "[Worker] Resolved path to: " << WavFilePath << std::endl;
				}
			}

#if GAMEPAD_CORE_HAS_AUDIO
			ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 2, 48000);
			if (ma_decoder_init_file(WavFilePath.c_str(), &decoderConfig, &decoder) == MA_SUCCESS)
			{
				ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
				bDecoderInitialized = true;
			}
			else
			{
				std::cerr << "[Worker Error] Failed to load WAV file: " << WavFilePath << std::endl;
				return;
			}
#endif
		}

		// Setup callback data
		audio_callback_data callbackData;
#if GAMEPAD_CORE_HAS_AUDIO
		callbackData.pDecoder = bDecoderInitialized ? &decoder : nullptr;
#else
		callbackData.pDecoder = nullptr;
#endif
		callbackData.bIsSystemAudio = bUseSystemAudio;
		callbackData.bIsWireless = bIsWireless;

		// Initialize playback device
#if GAMEPAD_CORE_HAS_AUDIO
		ma_device_config deviceConfig;
		if (bUseSystemAudio)
		{
			deviceConfig = ma_device_config_init(ma_device_type_loopback);
			deviceConfig.capture.format = ma_format_f32;
			deviceConfig.capture.channels = 2;
			deviceConfig.wasapi.loopbackProcessID = 0;
		}
		else
		{
			deviceConfig = ma_device_config_init(ma_device_type_playback);
			deviceConfig.playback.format = ma_format_f32;
			deviceConfig.playback.channels = 2;
		}

		deviceConfig.sampleRate = 48000;
		deviceConfig.dataCallback = audio_data_callback;
		deviceConfig.pUserData = &callbackData;

		ma_device device;
		if (ma_device_init(nullptr, &deviceConfig, &device) != MA_SUCCESS)
		{
			std::cerr << "[Worker Error] Failed to initialize audio device." << std::endl;
			if (bDecoderInitialized)
			{
				ma_decoder_uninit(&decoder);
			}
			return;
		}

		if (ma_device_start(&device) != MA_SUCCESS)
		{
			std::cerr << "[Worker Error] Failed to start audio device." << std::endl;
			ma_device_uninit(&device);
			if (bDecoderInitialized)
			{
				ma_decoder_uninit(&decoder);
			}
			return;
		}
#endif

		// Main loop for this controller
		while (!callbackData.bFinished && !bFinished.load() && Gamepad->IsConnected())
		{
			consume_haptics_queue(AudioHaptics, callbackData);
		}

		// Cleanup
#if GAMEPAD_CORE_HAS_AUDIO
		ma_device_uninit(&device);
		if (bDecoderInitialized)
		{
			ma_decoder_uninit(&decoder);
		}
#endif

		if (Gamepad->IsConnected())
		{
			if (auto* Lightbar = Gamepad->GetIGamepadLightbar())
			{
				Lightbar->SetLightbar({0, 255, 0});
			}
			Gamepad->UpdateOutput();
		}

		std::cout << "[Worker] Audio worker finished." << std::endl;
		bFinished.store(true);
	}

	IGamepadBase* Gamepad;
	std::string WavFilePath;
	bool bUseSystemAudio;
	std::atomic<bool> bFinished;
	std::thread WorkerThread;
};

struct audio_test_registry_policy : public test_utils::test_registry_policy
{
	std::vector<uint32_t> NewGamepads;
	gc_lock::mutex PolicyMutex;

	void DispatchNewGamepad(uint32_t GamepadId)
	{
		gc_lock::lock_guard<gc_lock::mutex> Lock(PolicyMutex);
		NewGamepads.push_back(GamepadId);
		std::cout << "[Policy] New Gamepad Registered: " << GamepadId << std::endl;
	}
};

using test_device_registry = GamepadCore::TBasicDeviceRegistry<audio_test_registry_policy>;

/**
 * @brief Audio device context using miniaudio for cross-platform audio playback.
 *
 * This replaces the previous WASAPI-specific implementation to support
 * Windows, Linux, and macOS platforms.
 */
struct FAudioDeviceContext
{
	using DevicePathType = std::string;
	using AudioDeviceType = ma_device;
	using AudioDeviceIdType = ma_device_id;
	using AudioRingBufferType = ma_pcm_rb;
	using AudioFrameCountType = ma_uint32;

	int NumChannels = 2;
	int SampleRate = 48000;
	bool bInitialized = false;
	bool bHasDeviceId = false;
	bool bRingBufferInitialized = false;

	DevicePathType DevicePath;
	AudioDeviceType Device{};
	AudioRingBufferType RingBuffer{};
	const AudioDeviceIdType* DeviceId = nullptr;

	FAudioDeviceContext() = default;

	~FAudioDeviceContext()
	{
		Close();
	}

	static void DataCallback(AudioDeviceType* pDevice, void* pOutput, const void* /*pInput*/, AudioFrameCountType frameCount)
	{
		auto pContext = static_cast<FAudioDeviceContext*>(pDevice->pUserData);
		if (!pContext || !pContext->bInitialized)
		{
			std::memset(pOutput, 0, frameCount * pDevice->playback.channels * sizeof(float));
			return;
		}

		AudioFrameCountType framesAvailable = ma_pcm_rb_available_read(&pContext->RingBuffer);
		AudioFrameCountType framesToRead = frameCount;

		if (framesAvailable < framesToRead)
		{
			framesToRead = framesAvailable;
		}

		if (framesToRead > 0)
		{
			void* pReadBuffer;
			AudioFrameCountType readSize = framesToRead;
			ma_pcm_rb_acquire_read(&pContext->RingBuffer, &readSize, &pReadBuffer);

			std::memcpy(pOutput, pReadBuffer, readSize * pContext->NumChannels * sizeof(float));
			ma_pcm_rb_commit_read(&pContext->RingBuffer, readSize);
		}

		if (framesToRead < frameCount)
		{

			std::cout << "[AudioPolicy] Audio data framesToRead < frameCount: " << (framesToRead < frameCount) << std::endl;
			auto pOutputFloat = static_cast<float*>(pOutput);
			AudioFrameCountType framesMissing = frameCount - framesToRead;
			std::memset(&pOutputFloat[framesToRead * pContext->NumChannels], 0,
			            framesMissing * pContext->NumChannels * sizeof(float));
		}
	}

	bool Initialize(int InSampleRate = 48000, int InNumChannels = 2)
	{
		return InitializeWithDeviceId(nullptr, InSampleRate, InNumChannels);
	}

	bool InitializeWithDeviceId(const AudioDeviceIdType* pDeviceId, int InSampleRate = 48000, int InNumChannels = 2)
	{
		if (bInitialized)
		{
			Close();
		}

		if (pDeviceId)
		{
			DeviceId = pDeviceId;
			bHasDeviceId = true;
		}
		else
		{
			DeviceId = nullptr;
			bHasDeviceId = false;
		}

		SampleRate = InSampleRate;
		NumChannels = InNumChannels;
		auto bufferSizeInFrames = static_cast<AudioFrameCountType>(SampleRate);

		if (ma_pcm_rb_init(ma_format_f32, NumChannels, bufferSizeInFrames, nullptr, nullptr, &RingBuffer) != MA_SUCCESS)
		{
			return false;
		}
		bRingBufferInitialized = true;

		ma_device_config Config = ma_device_config_init(ma_device_type_playback);
		Config.playback.format = ma_format_f32;
		Config.playback.channels = NumChannels;
		Config.playback.pDeviceID = DeviceId;
		Config.sampleRate = SampleRate;
		Config.dataCallback = DataCallback;
		Config.pUserData = this;

		if (ma_device_init(nullptr, &Config, &Device) != MA_SUCCESS)
		{
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
			return false;
		}

		if (ma_device_start(&Device) != MA_SUCCESS)
		{
			ma_device_uninit(&Device);
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
			return false;
		}

		bInitialized = true;
		return true;
	}

	void Close()
	{
		if (bInitialized)
		{
			ma_device_stop(&Device);
			ma_device_uninit(&Device);
			bInitialized = false;
		}
		if (bRingBufferInitialized)
		{
			ma_pcm_rb_uninit(&RingBuffer);
			bRingBufferInitialized = false;
		}
	}

	[[nodiscard]] bool IsValid() const
	{
		return bInitialized && bRingBufferInitialized;
	}

	AudioFrameCountType GetAvailableWriteFrames()
	{
		if (!bRingBufferInitialized)
		{
			return 0;
		}
		return ma_pcm_rb_available_write(&RingBuffer);
	}

	bool WriteHapticData(const std::vector<std::int16_t>& InterleavedData)
	{
		if (!IsValid() || InterleavedData.empty())
		{
			return false;
		}
		auto framesInput = static_cast<AudioFrameCountType>(InterleavedData.size() / 2);

		AudioFrameCountType framesAvailable = ma_pcm_rb_available_write(&RingBuffer);
		AudioFrameCountType framesToWrite = (framesInput > framesAvailable) ? framesAvailable : framesInput;

		if (framesToWrite == 0)
		{
			return true;
		}

		void* pWriteBufferPtr;
		if (ma_pcm_rb_acquire_write(&RingBuffer, &framesToWrite, &pWriteBufferPtr) != MA_SUCCESS)
		{
			return false;
		}

		auto* pOutputBuffer = static_cast<float*>(pWriteBufferPtr);
		constexpr float kNormalization = 1.0f / 32768.0f;
		for (AudioFrameCountType i = 0; i < framesToWrite; i++)
		{
			float LeftFloat = static_cast<float>(InterleavedData[i * 2]) * kNormalization;
			float RightFloat = static_cast<float>(InterleavedData[(i * 2) + 1]) * kNormalization;
			AudioFrameCountType baseIndex = i * NumChannels;

			if (NumChannels >= 4)
			{
				pOutputBuffer[baseIndex + 0] = 0.f;
				pOutputBuffer[baseIndex + 1] = 0.f;
				pOutputBuffer[baseIndex + 2] = LeftFloat;
				pOutputBuffer[baseIndex + 3] = RightFloat;
			}
			else
			{
				pOutputBuffer[baseIndex + 0] = LeftFloat;
				pOutputBuffer[baseIndex + 1] = RightFloat;
			}
		}

		ma_pcm_rb_commit_write(&RingBuffer, framesToWrite);
		return true;
	}
};

class test_audio_device_registry : public IAudioDevice
{
public:
	using DevicePathType = std::string;
	using AudioDeviceIdType = ma_device_id;
	using AudioRingBufferType = ma_pcm_rb;
	using AudioFrameCountType = ma_uint32;
	using AudioDeviceType = ma_device;

	std::unordered_map<DevicePathType, std::shared_ptr<FAudioDeviceContext>> DevicePolicies;

	test_audio_device_registry()
	{
		SetInstance(this);
	}

	static test_audio_device_registry* Get()
	{
		static test_audio_device_registry* Instance;
		if (!Instance)
		{
			Instance = new test_audio_device_registry();
		}
		return Instance;
	}

	void RegisterAudioDevice(const DevicePathType& Path, const AudioDeviceIdType* id)
	{
		auto Policy = std::make_shared<FAudioDeviceContext>();
		Policy->DevicePath = Path;
		if (id)
		{
			Policy->DeviceId = id;
			Policy->InitializeWithDeviceId(Policy->DeviceId, Policy->SampleRate, Policy->NumChannels);
		}
		else
		{
			Policy->Initialize(Policy->SampleRate, Policy->NumChannels);
		}
		DevicePolicies[Path] = std::move(Policy);
	}

	void UnregisterAudioDevice(const DevicePathType Path) override
	{
		DevicePolicies.erase(Path);
	}

	void ProcessAudioHaptic(FDeviceContext* Context, const std::vector<std::int16_t>& AudioData) override
	{
		if (!Context)
		{
			return;
		}

		if (const auto it = DevicePolicies.find(Context->Path); it != DevicePolicies.end())
		{
			it->second->WriteHapticData(AudioData);
		}
	}

	void InitializeAudioContainer(FDeviceContext* Context) override
	{
		if (!Context)
		{
			std::cerr << "[AudioPolicy] Error: Context is null" << std::endl;
			return;
		}

		if (const auto it = DevicePolicies.find(Context->Path); it != DevicePolicies.end())
		{
			return;
		}

		std::cout << "[AudioPolicy] Initializing audio for context: " << Context->Path << std::endl;
		ma_result result;
		ma_context maContext;
		result = ma_context_init(nullptr, 0, nullptr, &maContext);
		if (result != MA_SUCCESS)
		{
			std::cerr << "[AudioPolicy] Error: Failed to initialize miniaudio context: " << result << std::endl;
			return;
		}

		ma_device_info* pPlaybackInfos;
		ma_uint32 playbackCount;
		ma_device_info* pCaptureInfos;
		ma_uint32 captureCount;

		result = ma_context_get_devices(&maContext, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
		if (result != MA_SUCCESS)
		{
			std::cerr << "[AudioPolicy] Error: Failed to get devices: " << result << std::endl;
			ma_context_uninit(&maContext);
			return;
		}

		std::string TargetContainerId = windows_device_info::get_container_id(Context->Path);
		std::cout << "[AudioPolicy] Target Device Container ID: " << TargetContainerId << std::endl;

		const ma_device_id* pFoundDeviceId = nullptr;
		for (ma_uint32 i = 0; i < playbackCount; i++)
		{
			std::string AudioContainerId = windows_device_info::get_audio_container_id(pPlaybackInfos[i].id.wasapi);
			std::cout << "[AudioPolicy] Checking device: " << pPlaybackInfos[i].name
			          << " | Container ID: " << AudioContainerId << std::endl;

			if (!AudioContainerId.empty() && AudioContainerId == TargetContainerId)
			{
				pFoundDeviceId = &pPlaybackInfos[i].id;
				std::cout << "[AudioPolicy] Found match by Container ID: " << pPlaybackInfos[i].name << std::endl;
				break;
			}
		}

		if (!pFoundDeviceId)
		{
			std::cout << "[AudioPolicy] No match by Container ID. Falling back to name search..." << std::endl;
			for (ma_uint32 i = 0; i < playbackCount; i++)
			{
				std::string deviceName(pPlaybackInfos[i].name);
				if (deviceName.find("DualSense") != std::string::npos ||
				    deviceName.find("Wireless Controller") != std::string::npos)
				{
					pFoundDeviceId = &pPlaybackInfos[i].id;
					std::cout << "[AudioPolicy] Found potential match by name: " << pPlaybackInfos[i].name << std::endl;
					break;
				}
			}
		}

		if (pFoundDeviceId)
		{
			RegisterAudioDevice(Context->Path, pFoundDeviceId);
			std::cout << "[AudioPolicy] Successfully registered audio device for " << Context->Path << std::endl;
		}
		else
		{
			std::cerr << "[AudioPolicy] Warning: Could not find any DualSense audio device." << std::endl;
		}

		ma_context_uninit(&maContext);
	}
};

// ============================================================================
// Helper Functions
// ============================================================================
void print_help()
{
	std::cout << "\n=======================================================" << std::endl;
	std::cout << "        AUDIO HAPTICS INTEGRATION TEST                 " << std::endl;
	std::cout << "=======================================================" << std::endl;
	std::cout << " Usage: AudioHapticsTest <wav_file_path>" << std::endl;
	std::cout << "" << std::endl;
	std::cout << " This test plays a WAV file on your speakers" << std::endl;
	std::cout << " and simultaneously sends haptic feedback to" << std::endl;
	std::cout << " your DualSense controller." << std::endl;
	std::cout << "" << std::endl;
	std::cout << " Supports both USB and Bluetooth!" << std::endl;
	std::cout << " - USB: 48kHz haptics via audio device" << std::endl;
	std::cout << " - Bluetooth: 3000Hz haptics via HID" << std::endl;
	std::cout << "=======================================================" << std::endl;
}


int main(int argc, char* argv[])
{
	int error = 0;
	encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
	if (error)
	{
		std::cerr << "Failed to create Opus encoder: " << error << std::endl;
		return -1;
	}

	opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(166000)); //
	opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
	opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

	opus_encoder_ctl(encoder, OPUS_SET_PREDICTION_DISABLED(0));
	opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(80));
	opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
	opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));

	int ret;
	ret = opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
	if (ret != OPUS_OK) return ret;

	opus_int32 rate;
	opus_encoder_ctl(encoder, OPUS_GET_BANDWIDTH(&rate));

	std::cout << "Opus encoder initialized with bandwidth: " << rate << std::endl;
	opus_encoder_ctl(encoder, OPUS_RESET_STATE);

	std::string WavFilePath;
	bool bUseSystemAudio = false;

	if (argc < 2)
	{
#ifdef AUTOMATED_TESTS
		WavFilePath = std::string(GAMEPAD_CORE_PROJECT_ROOT) + "/Integration/Datasets/ES_Touch_SCENE.wav";
		bUseSystemAudio = false;
		std::cout << "[Test] Automated mode: Forcing audio file: " << WavFilePath << std::endl;
#else
		bUseSystemAudio = true;
		std::cout << "[System] No WAV file provided. Using System Audio Loopback." << std::endl;
		print_help();
#endif
	}
	else
	{
		WavFilePath = argv[1];
	}

	std::cout << "[System] Audio Haptics Integration Test" << std::endl;

#if GAMEPAD_CORE_HAS_AUDIO
	ma_decoder decoder;
	ma_uint64 totalFrames = 0;
#endif

	if (!bUseSystemAudio)
	{
		fs::path p(WavFilePath);
		if (!fs::exists(p))
		{
			fs::path alternativePath = fs::path(GAMEPAD_CORE_PROJECT_ROOT) / WavFilePath;
			if (fs::exists(alternativePath))
			{
				WavFilePath = alternativePath.string();
				std::cout << "[System] Resolved path to: " << WavFilePath << std::endl;
			}
		}

		std::cout << "[System] Loading WAV file: " << WavFilePath << std::endl;

#if GAMEPAD_CORE_HAS_AUDIO
		// Initialize decoder (output as float, stereo, 48kHz)
		ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 2, 48000);

		if (ma_decoder_init_file(WavFilePath.c_str(), &decoderConfig, &decoder) != MA_SUCCESS)
		{
			std::cerr << "[Error] Failed to load WAV file: " << WavFilePath << std::endl;
			return 1;
		}

		ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);

		std::cout << "[WavReader] Loaded WAV file successfully:" << std::endl;
		std::cout << "  - Sample Rate: " << decoder.outputSampleRate << " Hz" << std::endl;
		std::cout << "  - Channels: " << decoder.outputChannels << std::endl;
		std::cout << "  - Total Frames: " << totalFrames << std::endl;
		std::cout << "  - Duration: " << (static_cast<float>(totalFrames) / decoder.outputSampleRate) << " seconds" << std::endl;
		ma_decoder_uninit(&decoder);
#else
		std::cout << "[System] Audio support disabled. WAV playback not available." << std::endl;
#endif
	}
	else
	{
		std::cout << "[System] Mode: System Audio Capture (Press Ctrl+C to stop)" << std::endl;
	}

	// Initialize Hardware Layer
	std::cout << "[System] Initializing Hardware Layer..." << std::endl;
#if _WIN32
	using platform_hardware = windows_platform::windows_hardware;
#else
	using platform_hardware = linux_platform::linux_hardware;
#endif
	auto HardwareImpl = std::make_unique<platform_hardware>();
	IPlatformHardware::SetInstance(std::move(HardwareImpl));

	// Initialize Registry
	auto Registry = std::make_unique<test_device_registry>();

	// IMPORTANT: Keep AudioRegistry alive throughout the test
	auto AudioRegistry = std::make_unique<test_audio_device_registry>();
	IAudioDevice::SetInstance(AudioRegistry.get());

	std::cout << "[System] Waiting for controller connection via USB/BT..." << std::endl;
	std::unordered_map<uint32_t, std::unique_ptr<gamepad_audio_worker>> ActiveWorkers;

	bool IsWorked = false;
	while (!IsWorked)
	{
		Registry->Policy.NewGamepads.clear();
		Registry->PlugAndPlay(2.f);

		// Check for new gamepads from policy
		{
			gc_lock::lock_guard<gc_lock::mutex> Lock(Registry->Policy.PolicyMutex);
			for (uint32_t GamepadId : Registry->Policy.NewGamepads)
			{
				IGamepadBase* Gamepad = Registry->GetLibrary(GamepadId);
				if (Gamepad)
				{
					IGamepadSettings* Settings = Gamepad->GetIGamepadSettings();
					Settings->DualSenseSettings(0x0, 0x1, 0x1, 0, 100, 0xFC, 0, 0);

					// Initialize Audio for this gamepad
					if (GamepadId == 1)
					{
						if (auto* Lightbar = Gamepad->GetIGamepadLightbar())
						{
							Lightbar->SetLightbar({200, 255, 0});
							Lightbar->SetPlayerLed(EDSPlayer::Two, 0xff);
						}
						Gamepad->UpdateOutput();
					}
					else if (GamepadId == 0)
					{
						if (auto* Lightbar = Gamepad->GetIGamepadLightbar())
						{
							Lightbar->SetLightbar({0, 255, 255});
							Lightbar->SetPlayerLed(EDSPlayer::One, 0xff);
						}
						Gamepad->UpdateOutput();
					}

					std::cout << "[System] Creating worker for GamepadId: " << GamepadId << std::endl;
					auto Worker = std::make_unique<gamepad_audio_worker>(Gamepad, WavFilePath, bUseSystemAudio);
					Worker->start();
					ActiveWorkers[GamepadId] = std::move(Worker);
					IsWorked = true;
				}
			}
		}
	}
//
	while (true)
	{
		//gc_sync::sleep_for(std::chrono::milliseconds(16));
		// Clean up finished or disconnected workers
		for (auto it = ActiveWorkers.begin(); it != ActiveWorkers.end();)
		{
			bool bIsConnected = false;
			IGamepadBase* Gamepad = Registry->GetLibrary(it->first);
			if (Gamepad && Gamepad->IsConnected())
			{
				bIsConnected = true;
			}

			if (it->second->is_finished() || !bIsConnected)
			{
				std::cout << "[System] Removing worker for GamepadId: " << it->first << std::endl;
				it = ActiveWorkers.erase(it);
			}
			else
			{
				++it;
			}
		}
#ifdef AUTOMATED_TESTS
		static auto StartTime = std::chrono::steady_clock::now();
		auto Now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(Now - StartTime).count() >= 30)
		{
			if (!ActiveWorkers.empty())
			{
				std::cout << "[Test] Automated timeout reached (30s). Finishing..." << std::endl;
			}
			else
			{
				std::cout << "[Test] No controller found in automated mode after 30s. Exiting." << std::endl;
			}
			break;
		}
#endif
	}

	return 0;
}
#endif
