// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
#pragma once


#ifdef BUILD_GAMEPAD_CORE_TESTS

#include "GCore/Interfaces/IPlatformHardware.h"
#include "GCore/Templates/TBasicDeviceRegistry.h"
#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

#ifndef GAMEPAD_CORE_HAS_AUDIO
#define GAMEPAD_CORE_HAS_AUDIO 0
#endif

#if GAMEPAD_CORE_HAS_AUDIO
#include "opus.h"
#endif

#if _WIN32
#include "Platform/windows/windows_hardware_policy.h"
using platform_hardware = windows_platform::windows_hardware;
#if GAMEPAD_CORE_HAS_AUDIO
#include "Platform/windows/wasapi_policy.h"
using audio_platform_policy = wasapi_policy;
#endif
#elif __linux__
#include "Platform/linux/linux_hardware_policy.h"
using platform_hardware = linux_platform::linux_hardware;
#endif

namespace test_utils
{

#if GAMEPAD_CORE_HAS_AUDIO
	static OpusEncoder* encoder;
#endif

	// Thread-safe queue for audio packets
	template<typename T>
	class thread_safe_queue
	{
	public:
		void push(T item)
		{
			gc_lock::lock_guard<gc_lock::mutex> lock(mMutex);
			mQueue.push(std::move(item));
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

		void clear()
		{
			gc_lock::lock_guard<gc_lock::mutex> lock(mMutex);
			std::queue<T> emptyQueue;
			mQueue.swap(emptyQueue);
		}

	private:
		std::queue<T> mQueue;
		gc_lock::mutex mMutex;
	};

	inline void print_help()
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

	inline void automated_tests()
	{
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

	/**
	 * @brief Registry policy for tests that just prints when a new gamepad is dispatched.
	 */
	struct test_registry_policy
	{
		using EngineIdType = uint32_t;
		struct Hasher
		{
			size_t operator()(const EngineIdType& id) const { return std::hash<EngineIdType>{}(id); }
		};

		static EngineIdType AllocEngineDevice()
		{
			static EngineIdType nextId = 0;
			return nextId++;
		}
		static void DisconnectDevice(EngineIdType id) {}
		static void DispatchNewGamepad(EngineIdType id)
		{
			std::cout << "[TestRegistry] Dispatched Gamepad ID: " << id << std::endl;
		}
	};

	class test_device_registry : public GamepadCore::TBasicDeviceRegistry<test_registry_policy>
	{
	public:
		using TBasicDeviceRegistry<test_registry_policy>::TBasicDeviceRegistry;
		using TBasicDeviceRegistry<test_registry_policy>::GetLibrary;
	};

	/**
	 * @brief Initializes the hardware and registry for tests.
	 * @param OutHardware Pointer to the hardware info interface.
	 * @param OutRegistry Pointer to the device registry.
	 */
	inline void initialize_test_environment(
	    std::unique_ptr<IPlatformHardware>& OutHardware,
	    std::unique_ptr<test_device_registry>& OutRegistry)
	{
		if (!OutHardware)
		{
			OutHardware = std::make_unique<platform_hardware>();
		}
		IPlatformHardware::SetInstance(std::move(OutHardware));
		OutRegistry = std::make_unique<test_device_registry>();

		std::cout << "[test_utils] Environment initialized." << std::endl;
	}

#if GAMEPAD_CORE_HAS_AUDIO
	// Audio Haptics Constants (Based on AudioHapticsListener)
	constexpr float kLowPassAlpha = 1.0f;
	constexpr float kOneMinusAlpha = 1.0f - kLowPassAlpha;

	constexpr float kLowPassAlphaBt = 1.0f;
	constexpr float kOneMinusAlphaBt = 1.0f - kLowPassAlphaBt;
	struct BTPacket
	{
		std::vector<float> opus;      // float pcm
		std::vector<uint8_t> haptics; // 3000Hz data
		std::vector<uint8_t> signal;  // 200 data opus signal
	};

	// Global state for audio callback
	struct audio_callback_data
	{
		ma_decoder* pDecoder = nullptr;

		int framesPlayed = 0;
		float LowPassStateLeft = 0.0f;
		float LowPassStateRight = 0.0f;

		bool bFinished = false;
		bool bIsWireless = false;
		bool bIsSystemAudio = false;

		// Queues for haptics (like AudioHapticsListener)
		test_utils::thread_safe_queue<BTPacket> btPacketQueue;
		// Accumulator for Bluetooth - need 480 frames for 10ms Opus frame @ 48kHz
		test_utils::thread_safe_queue<std::vector<std::vector<float>>> btAccumulator;
		// Queues for usb
		test_utils::thread_safe_queue<std::vector<int16_t>> usbSampleQueue;
	};

	// Audio callback - plays audio on speakers and queues haptics data
	inline void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
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

			if (pData->bIsWireless)
			{
				static int fk = 0;
				static std::vector<std::vector<float>> dualChannel = {};
				if (fk % 2 == 0 && dualChannel.size() == 2)
				{
					// miniaudio already provides the captured audio in pInput
					pData->btAccumulator.push(dualChannel);
					dualChannel.clear();
				}

				dualChannel.push_back(std::vector<float>(static_cast<const float*>(pInput), static_cast<const float*>(pInput) + frameCount * 2));
			}
			else
			{
				const auto* pInputFloat = static_cast<const float*>(pInput);
				std::memcpy(tempBuffer.data(), pInputFloat, frameCount * 2 * sizeof(float));
				framesRead = frameCount;

				if (pOutput)
				{
					std::memcpy(pOutput, pInput, frameCount * 2 * sizeof(float));
				}
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

				const float outLeft = std::clamp(inLeft - pData->LowPassStateLeft, -1.0f, 1.0f);
				const float outRight = std::clamp(inRight - pData->LowPassStateRight, -1.0f, 1.0f);

				const std::vector<int16_t> stereoSample = {
				    static_cast<int16_t>(outLeft * 32767.0f),
				    static_cast<int16_t>(outRight * 32767.0f)};
				pData->usbSampleQueue.push(stereoSample);
			}
		}



		// Process for haptics
		if (pData->bIsWireless)
		{
			std::vector<std::vector<BTPacket>> packs;

			// samplesSend samples;
			std::vector<std::vector<float>> item;
			while (pData->btAccumulator.pop(item))
			{
				for (int bt = 0; bt < item.size(); bt++)
				{
					std::vector<float> processed;
					for (int i = 0; i < frameCount; i++)
					{
						float inLeft = item[bt][i * 2];
						float inRight = item[bt][i * 2 + 1];

						pData->LowPassStateLeft = kOneMinusAlpha * inLeft + kLowPassAlpha * pData->LowPassStateLeft;
						pData->LowPassStateRight = kOneMinusAlpha * inRight + kLowPassAlpha * pData->LowPassStateRight;

						if (std::isnan(inLeft) || std::isnan(inRight))
						{
							continue;
						}

						float outLeft = std::clamp(inLeft - pData->LowPassStateLeft, -1.0f, 1.0f);
						float outRight = std::clamp(inRight - pData->LowPassStateRight, -1.0f, 1.0f);
						processed.push_back(outLeft);
						processed.push_back(outRight);
					}

					auto opData = std::vector<uint8_t>(200);
					if (const int encodedBytes = opus_encode_float(encoder, processed.data(), processed.size() / 2, opData.data(), 200); encodedBytes <= 0)
					{
						std::cout << "Opus encoding failed: " << encodedBytes << std::endl;
						continue;
					}

					constexpr int ratio = 48000 / 3000;
					std::vector<uint8_t> hapticsData(64, 0);
					for (int outFrame = 0; outFrame < 32; ++outFrame)
					{
						const int inIdx = outFrame * ratio;

						if (inIdx * 2 + 1 >= processed.size())
						{
							break;
						}

						const float leftSample = processed[inIdx * 2];
						const float rightSample = processed[inIdx * 2 + 1];

						const float outLeft = std::clamp((leftSample * 127.0f), -128.0f, 127.0f);
						const float outRight = std::clamp((rightSample * 127.0f), -128.0f, 127.0f);
						hapticsData[(outFrame * 2)] = static_cast<uint8_t>(outLeft);
						hapticsData[(outFrame * 2) + 1] = static_cast<uint8_t>(outRight);
					}

					BTPacket packet;
					packet.opus = processed;
					packet.signal = opData;
					packet.haptics = hapticsData;
					pData->btPacketQueue.push(packet);
				}
			}
		}
		pData->framesPlayed += frameCount;
	}

	// Consume haptics queue and send to controller
	inline void consume_haptics_queue(IGamepadHaptics* AudioHaptics, audio_callback_data& callbackData)
	{
		if (!AudioHaptics || callbackData.bFinished)
		{
			return;
		}

		if (callbackData.bIsWireless)
		{
			test_utils::BTPacket chunk;
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

	// GamepadBase Audio Worker - Manages audio/haptics for a single controller
	class gamepad_audio_worker
	{
	public:
		gamepad_audio_worker(IGamepadBase* InGamepad, std::string InWavPath, const bool InUseSystemAudio)
		    : Gamepad(InGamepad)
		    , WavFilePath(std::move(InWavPath))
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
		const std::string& get_device_path() const { return DevicePath; }

	private:
		void run()
		{
			if (!Gamepad)
			{
				bFinished.store(true);
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
			if (Context)
			{
				DevicePath = Context->Path;
			}
			if (!bIsWireless && Context)
			{
				IAudioDevice::Get().InitializeAudioContainer(Context);
			}

			ma_decoder decoder;
			ma_uint64 totalFrames = 0;
			bool bDecoderInitialized = false;

			if (!bUseSystemAudio)
			{
				if (std::filesystem::path path(WavFilePath); !path.is_absolute() && !std::filesystem::exists(path))
				{
					#ifdef GAMEPAD_CORE_PROJECT_ROOT
					if (std::filesystem::path alternativePath = std::filesystem::path(GAMEPAD_CORE_PROJECT_ROOT) / WavFilePath; std::filesystem::exists(alternativePath))
					{
						WavFilePath = alternativePath.string();
						std::cout << "[Worker] Resolved path to: " << WavFilePath << std::endl;
					}
					#endif
				}

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
			}

			// Callback data
			audio_callback_data callbackData;
			callbackData.bIsWireless = bIsWireless;
			callbackData.bIsSystemAudio = bUseSystemAudio;
			callbackData.pDecoder = bDecoderInitialized ? &decoder : nullptr;

			// Initialize playback device
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
			deviceConfig.pUserData = &callbackData;
			deviceConfig.dataCallback = audio_data_callback;
			deviceConfig.periodSizeInMilliseconds = 10; // 20ms buffer for lower latency

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

			// Main loop for this controller
			while (!callbackData.bFinished && !bFinished.load())
			{
				if (!Gamepad->IsConnected())
				{
					break;
				}
				consume_haptics_queue(AudioHaptics, callbackData);
			}

			callbackData.bFinished = true;

			// Cleanup
			ma_device_uninit(&device);
			callbackData.btPacketQueue.clear();
			callbackData.btAccumulator.clear();
			callbackData.usbSampleQueue.clear();
			if (bDecoderInitialized)
			{
				ma_decoder_uninit(&decoder);
			}

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

		bool bUseSystemAudio;
		std::atomic<bool> bFinished;

		IGamepadBase* Gamepad;
		std::string WavFilePath;
		std::string DevicePath;
		std::thread WorkerThread;
	};
#endif

} // namespace test_utils

#endif
