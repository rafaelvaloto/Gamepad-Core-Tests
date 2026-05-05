// Copyright (c) 2025 Rafael Valoto. All Rights Reserved.
#include "linux_device_info.h"
#ifdef BUILD_GAMEPAD_CORE_TESTS
#ifdef __unix__
#include "GCore/Types/ECoreGamepad.h"
#include "GCore/Types/Structs/Config/GamepadCalibration.h"
#include "GCore/Types/Structs/Context/DeviceContext.h"
#include "GImplementations/Utils/GamepadSensors.h"
#include "SDL_hidapi.h"
#include <cstring>
#include <string>
#include <unordered_set>

static const std::uint16_t SONY_VENDOR_ID = 0x054C;
static const std::uint16_t DUALSHOCK4_PID_V1 = 0x05C4;
static const std::uint16_t DUALSHOCK4_PID_V2 = 0x09CC;
static const std::uint16_t DUALSENSE_PID = 0x0CE6;
static const std::uint16_t DUALSENSE_EDGE_PID = 0x0DF2;

void linux_device_info::read(FDeviceContext* Context)
{
	if (!Context || !Context->Handle)
	{
		return;
	}
	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Context->Handle);
	if (!DeviceHandle)
	{
		return;
	}

	if (Context->ConnectionType == EDSDeviceConnection::Bluetooth && Context->DeviceType == EDSDeviceType::DualShock4)
	{
		const size_t InputReportLength = 547;
		std::int32_t BytesRead = 0;
		if (poll_tick(Context->Handle, Context->BufferDS4, (std::int32_t)InputReportLength, BytesRead) == EPollResult::Disconnected)
		{
			invalidate_handle(Context);
		}
		return;
	}

	const size_t InputReportLength = (Context->ConnectionType == EDSDeviceConnection::Bluetooth) ? 78 : 64;
	if (sizeof(Context->Buffer) < InputReportLength)
	{
		invalidate_handle(Context);
		return;
	}

	std::int32_t BytesRead = 0;
	if (poll_tick(Context->Handle, Context->Buffer, (std::int32_t)InputReportLength, BytesRead) == EPollResult::Disconnected)
	{
		invalidate_handle(Context);
	}
}

void linux_device_info::process_audio_haptic(FDeviceContext* Context)
{
	if (!Context || !Context->Handle)
	{
		return;
	}

	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Context->Handle);

	if (Context->ConnectionType == EDSDeviceConnection::Bluetooth)
	{
		
		int BytesWritten = SDL_hid_write(DeviceHandle, Context->BufferHapitcs, 398);
	}
}

bool linux_device_info::configure_features(FDeviceContext* Context)
{
	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Context->Handle);

	unsigned char FeatureBuffer[41] = {0};
	std::memset(FeatureBuffer, 0, sizeof(FeatureBuffer));

	FeatureBuffer[0] = 0x05;
	if (!SDL_hid_get_feature_report(DeviceHandle, FeatureBuffer, 41))
	{
		return false;
	}

	using namespace FGamepadSensors;
	FGamepadCalibration Calibration;
	DualSenseCalibrationSensors(FeatureBuffer, Calibration);

	Context->Calibration = Calibration;
	return true;
}

void linux_device_info::write(FDeviceContext* Context)
{
	if (!Context || !Context->Handle)
	{
		return;
	}

	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Context->Handle);

	const size_t InReportLength = (Context->DeviceType == EDSDeviceType::DualShock4) ? 32 : 74;
	const size_t OutputReportLength = (Context->ConnectionType == EDSDeviceConnection::Bluetooth) ? 78 : InReportLength;

	int BytesWritten = SDL_hid_write(DeviceHandle, Context->GetRawOutputBuffer(), OutputReportLength);
	if (BytesWritten < 0)
	{
		invalidate_handle(Context);
	}
}

void linux_device_info::detect(std::vector<FDeviceContext>& Devices)
{
	Devices.clear();

	const std::unordered_set<uint16_t> SupportedPIDs = {
	    DUALSHOCK4_PID_V1,
	    DUALSHOCK4_PID_V2,
	    DUALSENSE_PID,
	    DUALSENSE_EDGE_PID};

	SDL_hid_device_info* Devs = SDL_hid_enumerate(SONY_VENDOR_ID, 0);
	if (!Devs)
	{
		return;
	}

	for (SDL_hid_device_info* CurrentDevice = Devs; CurrentDevice != nullptr; CurrentDevice = CurrentDevice->next)
	{
		if (SupportedPIDs.contains(CurrentDevice->product_id))
		{
			FDeviceContext NewDeviceContext;
			NewDeviceContext.Path = std::string(CurrentDevice->path);

			switch (CurrentDevice->product_id)
			{
				case DUALSHOCK4_PID_V1:
				case DUALSHOCK4_PID_V2:
					NewDeviceContext.DeviceType = EDSDeviceType::DualShock4;
					break;
				case DUALSENSE_EDGE_PID:
					NewDeviceContext.DeviceType = EDSDeviceType::DualSenseEdge;
					break;
				case DUALSENSE_PID:
				default:
					NewDeviceContext.DeviceType = EDSDeviceType::DualSense;
					break;
			}

			NewDeviceContext.IsConnected = true;
			if (CurrentDevice->interface_number == -1)
			{
				NewDeviceContext.ConnectionType = EDSDeviceConnection::Bluetooth;
			}
			else
			{
				NewDeviceContext.ConnectionType = EDSDeviceConnection::Usb;
			}
			NewDeviceContext.Handle = nullptr;
			Devices.push_back(NewDeviceContext);
		}
	}
	SDL_hid_free_enumeration(Devs);
}

bool linux_device_info::create_handle(FDeviceContext* Context)
{
	if (!Context)
	{
		return false;
	}

	const char* Path = Context->Path.data();
	const FPlatformDeviceHandle Handle = SDL_hid_open_path(Path, true);
	if (Handle == INVALID_PLATFORM_HANDLE)
	{
		return false;
	}

	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Handle);
	SDL_hid_set_nonblocking(DeviceHandle, 1);
	Context->Handle = Handle;

	configure_features(Context);
	return true;
}

void linux_device_info::invalidate_handle(FDeviceContext* Context)
{
	if (Context)
	{
		SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Context->Handle);
		if (DeviceHandle != nullptr)
		{
			SDL_hid_close(DeviceHandle);
		}

		Context->Handle = INVALID_PLATFORM_HANDLE;
		Context->IsConnected = false;

		Context->Path.clear();
		std::memset(Context->Buffer, 0, sizeof(Context->Buffer));
		std::memset(Context->BufferDS4, 0, sizeof(Context->BufferDS4));
		std::memset(Context->BufferHapitcs, 0, sizeof(Context->BufferHapitcs));

		unsigned char* RawOutput = Context->GetRawOutputBuffer();
		std::memset(RawOutput, 0, 78);
	}
}

EPollResult linux_device_info::poll_tick(FPlatformDeviceHandle Handle, unsigned char* Buffer, std::int32_t Length, std::int32_t& OutBytesRead)
{
	if (Handle == INVALID_PLATFORM_HANDLE)
	{
		return EPollResult::Disconnected;
	}

	SDL_hid_device* DeviceHandle = static_cast<SDL_hid_device*>(Handle);
	int Result = SDL_hid_read(DeviceHandle, Buffer, Length);

	if (Result < 0)
	{
		return EPollResult::Disconnected;
	}

	if (Result == 0)
	{
		OutBytesRead = 0;
		return EPollResult::NoIoThisTick;
	}

	OutBytesRead = Result;
	return EPollResult::ReadOk;
}
#endif
