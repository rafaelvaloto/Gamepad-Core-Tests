// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
// Project: GamepadCore
// Description: Integration test for Audio Haptics using a .wav file as input.
// Reference: Based on AudioHapticsListener implementation for USB/BT audio processing.

#ifdef BUILD_GAMEPAD_CORE_TESTS
#include "GCore/Templates/TAudioDeviceRegistry.h"
#include "GCore/Utils/SoDefines.h"
#include "Platform/windows/wasapi_policy.h"
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
#include <vector>
namespace fs = std::filesystem;

// miniaudio for audio playback and WAV decoding
#if GAMEPAD_CORE_HAS_AUDIO
#include "miniaudio.h"
#endif

#include "GCore/Interfaces/IPlatformHardware.h"
#include "GCore/Interfaces/Segregations/IGamepadBase.h"
#include "GCore/Templates/TBasicDeviceRegistry.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "opus.h"
#include "test_utils.h"

struct test_device_registry_policy : public test_utils::test_registry_policy
{
	std::vector<uint32_t> NewGamepads;
	gc_lock::mutex PolicyMutex;

	void DispatchNewGamepad(const uint32_t GamepadId)
	{
		gc_lock::lock_guard<gc_lock::mutex> Lock(PolicyMutex);
		NewGamepads.push_back(GamepadId);

		std::cout << "[Policy] New Gamepad Registered: " << GamepadId << std::endl;
	}
};

class test_audio_device_registry : public IAudioDevice
{
public:
	using DevicePathType = std::string;
	std::unordered_map<DevicePathType, std::shared_ptr<TAudioDeviceRegistry<audio_platform_policy>>> DevicePolicies;

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
			// already initialized
			return;
		}

		auto Policy = std::make_shared<TAudioDeviceRegistry<audio_platform_policy>>();
		Policy->Policy.DevicePath = Context->Path;
		if (Policy->Policy.InitializeAudioContainer(Context))
		{
			if (Context->ConnectionType == EDSDeviceConnection::Usb)
			{
				Policy->Policy.NumChannels = 2; // USB can support more channels for haptics
				Policy->Policy.SampleRate = 48000;
			}

			DevicePolicies[Context->Path] = std::move(Policy);
			std::cout << "[AudioPolicy] Registered audio device for: " << Context->Path << std::endl;
		}
		else
		{
			std::cerr << "[AudioPolicy] Warning: Could not initialize audio device for: " << Context->Path << std::endl;
		}
	}
};

using test_device_registry = TBasicDeviceRegistry<test_device_registry_policy>;
int main(int argc, char* argv[])
{
	int error = 0;
	test_utils::encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
	if (error)
	{
		std::cerr << "Failed to create Opus encoder: " << error << std::endl;
		return -1;
	}

	opus_encoder_ctl(test_utils::encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_BITRATE(OPUS_BITRATE_MAX)); //
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_VBR(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_COMPLEXITY(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_PREDICTION_DISABLED(1));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_PACKET_LOSS_PERC(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	std::string WavFilePath;
	bool bUseSystemAudio = false;

	if (argc < 2)
	{
		bUseSystemAudio = true;
		std::cout << "[System] No WAV file provided. Using System Audio Loopback." << std::endl;
		test_utils::print_help();
	}
	else
	{
		WavFilePath = argv[1];
	}

	std::cout << "[System] Audio Haptics Integration Test" << std::endl;

	ma_decoder decoder;
	ma_uint64 totalFrames = 0;
	if (!bUseSystemAudio)
	{
		if (fs::path path(WavFilePath); !fs::exists(path))
		{
			if (fs::path alternativePath = fs::path(GAMEPAD_CORE_PROJECT_ROOT) / WavFilePath; fs::exists(alternativePath))
			{
				WavFilePath = alternativePath.string();
				std::cout << "[System] Resolved path to: " << WavFilePath << std::endl;
			}
		}

		std::cout << "[System] Loading WAV file: " << WavFilePath << std::endl;

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
	}
	else
	{
		std::cout << "[System] Mode: System Audio Capture (Press Ctrl+C to stop)" << std::endl;
	}

	// Initialize Hardware Layer
	std::cout << "[System] Initializing Hardware Layer..." << std::endl;
	auto HardwareImpl = std::make_unique<platform_hardware>();
	IPlatformHardware::SetInstance(std::move(HardwareImpl));

	// Initialize Registry
	auto Registry = std::make_unique<test_device_registry>();

	// IMPORTANT: Keep AudioRegistry alive throughout the test
	auto AudioRegistry = std::make_unique<test_audio_device_registry>();
	IAudioDevice::SetInstance(AudioRegistry.get());

	std::cout << "[System] Waiting for controller connection via USB/BT..." << std::endl;
	std::unordered_map<uint32_t, std::unique_ptr<test_utils::gamepad_audio_worker>> ActiveWorkers;

	Registry->Policy.NewGamepads.clear();
	Registry->PlugAndPlay(2.0f);
	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
		Registry->PlugAndPlay(0.0166f); // when sum 0.0166 == 2 seconds

		// Check for new gamepads from policy
		{
			gc_lock::lock_guard<gc_lock::mutex> Lock(Registry->Policy.PolicyMutex);
			for (uint32_t GamepadId : Registry->Policy.NewGamepads)
			{
				if (IGamepadBase* Gamepad = Registry->GetLibrary(GamepadId))
				{
					auto ctx = Gamepad->GetMutableDeviceContext();
					if (ctx && ctx->ConnectionType == EDSDeviceConnection::Usb)
					{
						test_audio_device_registry::Get()->InitializeAudioContainer(ctx);
					}

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

					if (!ActiveWorkers.contains(GamepadId))
					{
						auto Worker = std::make_unique<test_utils::gamepad_audio_worker>(Gamepad, WavFilePath, bUseSystemAudio);
						ActiveWorkers[GamepadId] = std::move(Worker);
						ActiveWorkers[GamepadId]->start();

						std::cout << "[System] Creating worker for GamepadId: " << GamepadId << std::endl;
					}
				}
			}
		}

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

		// helper automated tests
		test_utils::automated_tests();
	}

	return 0;
}
#endif
