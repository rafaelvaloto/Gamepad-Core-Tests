// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
// Project: GamepadCore
// Description: Integration test for Audio Haptics using different .wav files for different controllers.

#ifdef BUILD_GAMEPAD_CORE_TESTS
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include "GCore/Utils/SoDefines.h"
#include <queue>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

// miniaudio for audio playback and WAV decoding
#if GAMEPAD_CORE_HAS_AUDIO
#include "miniaudio.h"
#endif
#include "GCore/Interfaces/Segregations/IGamepadBase.h"
#include "GCore/Interfaces/IPlatformHardware.h"
#include "GCore/Interfaces/Segregations/IGamepadHaptics.h"
#include "GCore/Templates/TBasicDeviceRegistry.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "test_utils.h"

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
	std::atomic<bool> bFinished{false};
	std::atomic<uint64_t> framesPlayed{0};
	bool bIsWireless = false;

	// Queues for haptics (like AudioHapticsListener)
	thread_safe_queue<std::vector<uint8_t>> btPacketQueue;
	thread_safe_queue<std::vector<int16_t>> usbSampleQueue;

	// Accumulator for Bluetooth - need 1024 frames to produce 64 resampled frames
	std::vector<float> btAccumulator;
	gc_lock::mutex btAccumulatorMutex;
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

	std::vector<float> tempBuffer(frameCount * 2);
	ma_uint64 framesRead = 0;

	if (pData->bIsSystemAudio)
	{
		if (pInput == nullptr)
		{
			return;
		}

		const float* pInputFloat = static_cast<const float*>(pInput);
		std::memcpy(tempBuffer.data(), pInputFloat, frameCount * 2 * sizeof(float));
		framesRead = frameCount;

		if (pOutput)
		{
			std::memcpy(pOutput, pInput, frameCount * 2 * sizeof(float));
		}
	}
	else
	{
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

		if (pOutput)
		{
			auto* pOutputFloat = static_cast<float*>(pOutput);
			std::memcpy(pOutputFloat, tempBuffer.data(), framesRead * 2 * sizeof(float));

			if (framesRead < frameCount)
			{
				std::memset(&pOutputFloat[framesRead * 2], 0, (frameCount - framesRead) * 2 * sizeof(float));
			}
		}
	}

	if (!pData->bIsWireless)
	{
		for (ma_uint64 i = 0; i < framesRead; ++i)
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
	else
	{
		for (ma_uint64 i = 0; i < framesRead; ++i)
		{
			pData->btAccumulator.push_back(tempBuffer[i * 2]);     // Left
			pData->btAccumulator.push_back(tempBuffer[i * 2 + 1]); // Right
		}

		const size_t requiredSamples = 1024 * 2;

		while (true)
		{
			std::vector<float> framesToProcess;

			{
  		gc_lock::lock_guard<gc_lock::mutex> lock(pData->btAccumulatorMutex);
				if (pData->btAccumulator.size() < requiredSamples)
				{
					break;
				}

				framesToProcess.assign(pData->btAccumulator.begin(), pData->btAccumulator.begin() + requiredSamples);
				pData->btAccumulator.erase(pData->btAccumulator.begin(), pData->btAccumulator.begin() + requiredSamples);
			}

			const float ratio = 3000.0f / 48000.0f;
			const std::int32_t numInputFrames = 1024;

			std::vector<float> resampledData(128, 0.0f);

			for (std::int32_t outFrame = 0; outFrame < 64; ++outFrame)
			{
				float srcPos = static_cast<float>(outFrame) / ratio;
				std::int32_t srcIndex = static_cast<std::int32_t>(srcPos);
				float frac = srcPos - static_cast<float>(srcIndex);

				if (srcIndex >= numInputFrames - 1)
				{
					srcIndex = numInputFrames - 2;
					frac = 1.0f;
				}
				if (srcIndex < 0)
				{
					srcIndex = 0;
				}

				float left0 = framesToProcess[srcIndex * 2];
				float left1 = framesToProcess[(srcIndex + 1) * 2];
				float right0 = framesToProcess[srcIndex * 2 + 1];
				float right1 = framesToProcess[(srcIndex + 1) * 2 + 1];

				resampledData[outFrame * 2] = left0 + frac * (left1 - left0);
				resampledData[outFrame * 2 + 1] = right0 + frac * (right1 - right0);
			}

			for (std::int32_t i = 0; i < 64; ++i)
			{
				const std::int32_t dataIndex = i * 2;

				float inLeft = resampledData[dataIndex];
				float inRight = resampledData[dataIndex + 1];

				pData->LowPassStateLeft = kOneMinusAlphaBt * inLeft + kLowPassAlphaBt * pData->LowPassStateLeft;
				pData->LowPassStateRight = kOneMinusAlphaBt * inRight + kLowPassAlphaBt * pData->LowPassStateRight;

				resampledData[dataIndex] = inLeft - pData->LowPassStateLeft;
				resampledData[dataIndex + 1] = inRight - pData->LowPassStateRight;
			}

			std::vector<std::int8_t> packet1(64, 0);
			for (std::int32_t i = 0; i < 32; ++i)
			{
				const std::int32_t dataIndex = i * 2;
				float leftSample = resampledData[dataIndex];
				float rightSample = resampledData[dataIndex + 1];
				packet1[dataIndex] = static_cast<std::int8_t>(std::clamp(static_cast<int>(std::round(leftSample * 127.0f)), -128, 127));
				packet1[dataIndex + 1] = static_cast<std::int8_t>(std::clamp(static_cast<int>(std::round(rightSample * 127.0f)), -128, 127));
			}

			std::vector<std::int8_t> packet2(64, 0);
			for (std::int32_t i = 0; i < 32; ++i)
			{
				const std::int32_t dataIndex = (i + 32) * 2;
				float leftSample = resampledData[dataIndex];
				float rightSample = resampledData[dataIndex + 1];
				packet2[i * 2] = static_cast<std::int8_t>(std::clamp(static_cast<int>(std::round(leftSample * 127.0f)), -128, 127));
				packet2[i * 2 + 1] = static_cast<std::int8_t>(std::clamp(static_cast<int>(std::round(rightSample * 127.0f)), -128, 127));
			}

			std::vector<std::uint8_t> packet1Unsigned(packet1.begin(), packet1.end());
			std::vector<std::uint8_t> packet2Unsigned(packet2.begin(), packet2.end());

			pData->btPacketQueue.push(packet1Unsigned);
			pData->btPacketQueue.push(packet2Unsigned);
		}
	}

	pData->framesPlayed += framesRead;
}
#endif

void consume_haptics_queue(IGamepadHaptics* AudioHaptics, audio_callback_data& callbackData)
{
	if (callbackData.bIsWireless)
	{
		std::vector<std::uint8_t> packet;
		while (callbackData.btPacketQueue.pop(packet))
		{
			AudioHaptics->AudioHapticUpdate(packet);
		}
	}
	else
	{
		std::vector<std::int16_t> allSamples;
		allSamples.reserve(2048 * 2);

		std::vector<int16_t> stereoSample;
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

	~gamepad_audio_worker() { stop(); }

	void start() { WorkerThread = std::thread(&gamepad_audio_worker::run, this); }

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

		std::cout << "[Worker] Starting audio worker for controller (File: " << (bUseSystemAudio ? "System Audio" : WavFilePath) << ")..." << std::endl;

		bool bIsWireless = Gamepad->GetConnectionType() == EDSDeviceConnection::Bluetooth;
		IGamepadHaptics* AudioHaptics = Gamepad->GetIGamepadHaptics();
		if (!AudioHaptics)
		{
			return;
		}

		FDeviceContext* Context = Gamepad->GetMutableDeviceContext();
		if (!bIsWireless && Context)
		{
			IPlatformHardware::Get().InitializeAudioDevice(Context);
		}

#if GAMEPAD_CORE_HAS_AUDIO
		ma_decoder decoder;
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
				bDecoderInitialized = true;
			}
			else
			{
				std::cerr << "[Worker Error] Failed to load WAV file: " << WavFilePath << std::endl;
				return;
			}
#endif
		}

		audio_callback_data callbackData;
#if GAMEPAD_CORE_HAS_AUDIO
		callbackData.pDecoder = bDecoderInitialized ? &decoder : nullptr;
#else
		callbackData.pDecoder = nullptr;
#endif
		callbackData.bIsSystemAudio = bUseSystemAudio;
		callbackData.bIsWireless = bIsWireless;

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
			if (bDecoderInitialized)
			{
				ma_decoder_uninit(&decoder);
			}
			return;
		}

		if (ma_device_start(&device) != MA_SUCCESS)
		{
			ma_device_uninit(&device);
			if (bDecoderInitialized)
			{
				ma_decoder_uninit(&decoder);
			}
			return;
		}
#endif

		while (!callbackData.bFinished && !bFinished.load() && Gamepad->IsConnected())
		{
			consume_haptics_queue(AudioHaptics, callbackData);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

#if GAMEPAD_CORE_HAS_AUDIO
		ma_device_uninit(&device);
		if (bDecoderInitialized)
		{
			ma_decoder_uninit(&decoder);
		}
#endif
		bFinished.store(true);
	}

	IGamepadBase* Gamepad;
	std::string WavFilePath;
	bool bUseSystemAudio;
	std::atomic<bool> bFinished;
	std::thread WorkerThread;
};

/**
 * @brief Audio device context using miniaudio for cross-platform audio playback.
 */
struct FAudioDeviceContext
{
	using DevicePathType = std::string;
	using AudioDeviceType = ma_device;
	using AudioDeviceIdType = ma_device_id;
	using AudioRingBufferType = ma_pcm_rb;
	using AudioFrameCountType = ma_uint32;

	int NumChannels = 4;
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
			auto pOutputFloat = static_cast<float*>(pOutput);
			AudioFrameCountType framesMissing = frameCount - framesToRead;

			std::memset(&pOutputFloat[framesToRead * pContext->NumChannels], 0,
			            framesMissing * pContext->NumChannels * sizeof(float));
		}
	}

	bool Initialize(int InSampleRate = 48000, int InNumChannels = 4)
	{
		return InitializeWithDeviceId(nullptr, InSampleRate, InNumChannels);
	}

	bool InitializeWithDeviceId(const AudioDeviceIdType* pDeviceId, int InSampleRate = 48000, int InNumChannels = 4)
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

	void RegisterAudioDevice(DevicePathType Path, const AudioDeviceIdType* id)
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

	void UnregisterAudioDevice(DevicePathType Path) override
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

using audio_test_device_registry = GamepadCore::TBasicDeviceRegistry<audio_test_registry_policy>;

void print_help()
{
	std::cout << "\n=======================================================" << std::endl;
	std::cout << "        CHANNELS HAPTICS INTEGRATION TEST              " << std::endl;
	std::cout << "=======================================================" << std::endl;
	std::cout << " Usage: test-channels-haptics <wav1> <wav2> ... <wavN>" << std::endl;
	std::cout << "" << std::endl;
	std::cout << " Each argument is assigned to a controller based on its order" << std::endl;
	std::cout << " Example: test-channels-haptics drum.wav bass.wav" << std::endl;
	std::cout << "   - Controller 0: drum.wav" << std::endl;
	std::cout << "   - Controller 1: bass.wav" << std::endl;
	std::cout << "=======================================================" << std::endl;
}

int main(int argc, char* argv[])
{
	std::vector<std::string> WavFiles;
	bool bUseSystemAudio = false;

	if (argc < 2)
	{
#ifdef AUTOMATED_TESTS
		WavFiles.push_back(std::string(GAMEPAD_CORE_PROJECT_ROOT) + "/Tests/Integration/Datasets/ES_Replay_Lawd_Ito.wav");
		std::cout << "[Test] Automated mode: Using default file." << std::endl;
#else
		print_help();
		bUseSystemAudio = true;
		std::cout << "[System] No WAV files provided. Using System Audio Loopback for all." << std::endl;
#endif
	}
	else
	{
		for (int i = 1; i < argc; ++i)
		{
			WavFiles.push_back(argv[i]);
		}
	}

	std::cout << "[System] Initializing Hardware..." << std::endl;
#if _WIN32
	using platform_hardware = windows_platform::windows_hardware;
#else
	using platform_hardware = linux_platform::linux_hardware;
#endif
	IPlatformHardware::SetInstance(std::make_unique<platform_hardware>());
	auto Registry = std::make_unique<audio_test_device_registry>();

	// IMPORTANT: Keep AudioRegistry alive throughout the test
	auto AudioRegistry = std::make_unique<test_audio_device_registry>();
	IAudioDevice::SetInstance(AudioRegistry.get());

	std::unordered_map<uint32_t, std::unique_ptr<gamepad_audio_worker>> ActiveWorkers;

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
		Registry->PlugAndPlay(0.016f);

		{
			gc_lock::lock_guard<gc_lock::mutex> Lock(Registry->Policy.PolicyMutex);
			for (uint32_t GamepadId : Registry->Policy.NewGamepads)
			{
				IGamepadBase* Gamepad = Registry->GetLibrary(GamepadId);
				if (Gamepad)
				{
					// Initialize Audio for this gamepad
					IAudioDevice::Get().InitializeAudioContainer(Gamepad->GetMutableDeviceContext());

					if (GamepadId == 1)
					{
						if (auto* Lightbar = Gamepad->GetIGamepadLightbar())
						{
							Lightbar->SetLightbar({200, 255, 0});
							Lightbar->SetPlayerLed(EDSPlayer::Two, 0xff);
						}
						Gamepad->UpdateOutput();
						std::this_thread::sleep_for(std::chrono::seconds(1));
					}
					else if (GamepadId == 0)
					{
						if (auto* Lightbar = Gamepad->GetIGamepadLightbar())
						{
							Lightbar->SetLightbar({0, 255, 255});
							Lightbar->SetPlayerLed(EDSPlayer::One, 0xff);
						}
						Gamepad->UpdateOutput();
						std::this_thread::sleep_for(std::chrono::seconds(1));
					}

					std::string SelectedWav;
					bool bLocalUseSystem = bUseSystemAudio;

					if (!bUseSystemAudio)
					{
						if (GamepadId < WavFiles.size())
						{
							SelectedWav = WavFiles[GamepadId];
						}
						else
						{
							SelectedWav = WavFiles.back();
							std::cout << "[Warning] No specific WAV for GamepadId " << GamepadId << ". Using last: " << SelectedWav << std::endl;
						}
					}

					auto Worker = std::make_unique<gamepad_audio_worker>(Gamepad, SelectedWav, bLocalUseSystem);
					Worker->start();
					ActiveWorkers[GamepadId] = std::move(Worker);
				}
			}
			Registry->Policy.NewGamepads.clear();
		}

		for (auto it = ActiveWorkers.begin(); it != ActiveWorkers.end();)
		{
			IGamepadBase* Gamepad = Registry->GetLibrary(it->first);
			if (it->second->is_finished() || !Gamepad || !Gamepad->IsConnected())
			{
				it = ActiveWorkers.erase(it);
			}
			else
			{
				++it;
			}
		}

#ifdef AUTOMATED_TESTS
		static auto StartTime = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - StartTime).count() >= 30)
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
