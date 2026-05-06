// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
// Project: GamepadCore
// Description: Integration test for Audio Haptics using a .wav file as input.
// Reference: Based on AudioHapticsListener implementation for USB/BT audio processing.

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

#include "GCore/Interfaces/IPlatformHardware.h"
#include "GCore/Interfaces/Segregations/IGamepadBase.h"
#include "GCore/Templates/TBasicDeviceRegistry.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
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
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_BITRATE(160000)); //
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_VBR(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_COMPLEXITY(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_PREDICTION_DISABLED(1));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_PACKET_LOSS_PERC(0));
	opus_encoder_ctl(test_utils::encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	bool bUseSystemAudio = false;
	std::vector<std::string> WavFiles;

	if (argc < 2)
	{
		bUseSystemAudio = true;
		std::cout << "[System] No WAV file provided. Using System Audio Loopback." << std::endl;
		test_utils::print_help();
	}
	else
	{

		for (int i = 1; i < argc; ++i)
		{
			WavFiles.push_back(argv[i]);
		}
	}

	std::cout << "[System] Audio Haptics Integration Test" << std::endl;

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
	std::unordered_map<uint32_t, std::string> WorkerDevicePaths;

	Registry->Policy.NewGamepads.clear();
	Registry->PlugAndPlay(2.0f);
	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Registry->PlugAndPlay(0.001f); // when sum 0.0166 == 2 seconds

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
						std::string SelectedWav;
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

						auto Worker = std::make_unique<test_utils::gamepad_audio_worker>(Gamepad, SelectedWav, bUseSystemAudio);
						ActiveWorkers[GamepadId] = std::move(Worker);
						ActiveWorkers[GamepadId]->start();
						if (ctx)
						{
							WorkerDevicePaths[GamepadId] = ctx->Path;
						}

						std::cout << "[System] Creating worker for GamepadId: " << GamepadId << std::endl;
					}
				}
			}
			Registry->Policy.NewGamepads.clear();
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
				it->second->stop();
				if (const auto pathIt = WorkerDevicePaths.find(it->first); pathIt != WorkerDevicePaths.end())
				{
					test_audio_device_registry::Get()->UnregisterAudioDevice(pathIt->second);
					WorkerDevicePaths.erase(pathIt);
				}
				std::cout << "[System] Removing worker for GamepadId: " << it->first << std::endl;
				it = ActiveWorkers.erase(it);
			}
			else
			{
				++it;
			}
		}

		// helper automated tests
		if (test_utils::automated_tests())
		{
			break;
		}
	}

	return 0;
}
