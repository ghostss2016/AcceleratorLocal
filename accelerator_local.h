/**
 * ======================================================
 * Accelerator local
 * Written by Phoenix (˙·٠●Феникс●٠·˙) 2023-2025, Asher Baker (asherkin) 2011.
 * ======================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from 
 * the use of this software.
 */

#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <eiface.h>
#include <iserver.h>
#include "metamod_virtual_hook.h"
#include "accelerator_runtime.h"

#if METAMOD_PLAPI_VERSION < 18
#error "AcceleratorLocal requires the real MetaMod API18/KHook headers"
#endif

namespace google_breakpad { class ExceptionHandler; }

class AcceleratorLocal final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
	bool Unload(char* error, size_t maxlen) override;
	
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();

private: // Hooks
	using FrameHook = SvarogHooks::Virtual<IServerGameDLL, void, bool, bool, bool>;
	using StartupHook = SvarogHooks::Virtual<INetworkServerService, void,
		const GameSessionConfiguration_t&, ISource2WorldSession*, const char*>;
	accelerator::OwnedHooks<FrameHook, StartupHook> hooks_;
	accelerator::SignalMonitor signals_;
	IServerGameDLL* server_ = nullptr;
	INetworkServerService* networkService_ = nullptr;
	google_breakpad::ExceptionHandler* exceptionHandler_ = nullptr;
	KHook::Return<void> Api18GameFrame(IServerGameDLL*, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<void> Api18StartupServer(INetworkServerService*, const GameSessionConfiguration_t&,
		ISource2WorldSession*, const char*);
	void GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void StartupServer(const char* mapName);
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
