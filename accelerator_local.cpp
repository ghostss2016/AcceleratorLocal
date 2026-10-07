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

#include "accelerator_local.h"
#include "CMiniDumpComment.hpp"

#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <limits>
#include <new>

#include "common/path_helper.h"
#include "common/scoped_ptr.h"
#include "common/using_std_string.h"
#include "google_breakpad/processor/basic_source_line_resolver.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_state.h"
#include "processor/simple_symbol_supplier.h"
#include "processor/stackwalk_common.h"
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <processor/pathname_stripper.h>

AcceleratorLocal g_AcceleratorLocal;
PLUGIN_EXPOSE(AcceleratorLocal, g_AcceleratorLocal);

accelerator::CrashMetadata g_CrashMetadata;
accelerator::CallbackActivity g_CallbackActivity;
auto& crashMap = g_CrashMetadata.map;
auto& crashGamePath = g_CrashMetadata.gamePath;
auto& crashCommandLine = g_CrashMetadata.commandLine;
auto& dumpStoragePath = g_CrashMetadata.dumpPath;
CMiniDumpComment g_MiniDumpComment(95000);

// Compatibility for the pinned upstream Breakpad API. Retain the original
// four-argument writer call and its full stack/all-thread output semantics.
static void PrintProcessState(const google_breakpad::ProcessState& state,
	bool stackContents, bool requestingThreadOnly,
	google_breakpad::SourceLineResolverInterface* resolver)
{
	accelerator::PrintOriginalProcessState(state, stackContents, requestingThreadOnly,
		resolver, &google_breakpad::PrintProcessState);
}

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	accelerator::CallbackActivity::Scope callback(g_CallbackActivity);
	if (succeeded)
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	else
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded)
		return succeeded;

	my_strlcpy(dumpStoragePath, descriptor.path(), sizeof(dumpStoragePath));
	my_strlcat(dumpStoragePath, ".txt", sizeof(dumpStoragePath));

	int extra = sys_open(dumpStoragePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (extra == -1)
	{
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);
	sys_write(extra, "\n", 1);
	
	LoggingSystem_GetLogCapture(&g_MiniDumpComment, true);
	const char* pszConsoleHistory = g_MiniDumpComment.GetStartPointer();
	
	if (pszConsoleHistory[0])
	{
		sys_write(extra, "-------- CONSOLE HISTORY BEGIN --------\n", 40);
		sys_write(extra, pszConsoleHistory, my_strlen(pszConsoleHistory));
		sys_write(extra, "-------- CONSOLE HISTORY END --------\n", 38);
		sys_write(extra, "\n", 1);
	}
	
	google_breakpad::scoped_ptr<google_breakpad::SimpleSymbolSupplier> symbolSupplier;
	google_breakpad::BasicSourceLineResolver resolver;
	google_breakpad::MinidumpProcessor minidump_processor(symbolSupplier.get(), &resolver);

	// Increase the maximum number of threads and regions.
	google_breakpad::MinidumpThreadList::set_max_threads(std::numeric_limits<uint32_t>::max());
	google_breakpad::MinidumpMemoryList::set_max_regions(std::numeric_limits<uint32_t>::max());
	// Process the minidump.
	google_breakpad::Minidump miniDump(descriptor.path());
	if (!miniDump.Read())
	{
		sys_write(STDOUT_FILENO, "Failed to read minidump\n", 24);
	}
	else
	{
		google_breakpad::ProcessState processState;
		if (minidump_processor.Process(&miniDump, &processState) !=  google_breakpad::PROCESS_OK)
		{
			sys_write(STDOUT_FILENO, "MinidumpProcessor::Process failed\n", 34);
		}
		else
		{
			int requestingThread = processState.requesting_thread();
			if (requestingThread == -1)
				requestingThread = 0;

			const google_breakpad::CallStack* stack = processState.threads()->at(requestingThread);
			size_t frameCount = MIN(stack->frames()->size(), 15);

			auto signal_safe_hex_print = [](uint64_t num)
			{
				char buffer[18];
				char* ptr = buffer + sizeof(buffer);

				if (num == 0)
					*(--ptr) = '0';
				else
				{
					while (num > 0)
					{
						*(--ptr) = "0123456789abcdef"[num % 16];
						num /= 16;
					}
				}

				*(--ptr) = 'x';
				*(--ptr) = '0';

				size_t length = buffer + sizeof(buffer) - ptr;
				sys_write(STDOUT_FILENO, ptr, length);
			};

			sys_write(STDOUT_FILENO, "\n", 1);
			for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
			{
				const google_breakpad::StackFrame* frame = stack->frames()->at(frameIndex);

				uint64_t moduleOffset = frame->ReturnAddress();
				if (frame->module)
				{
					const std::string moduleFile = google_breakpad::PathnameStripper::File(frame->module->code_file());
					moduleOffset -= frame->module->base_address();
					sys_write(STDOUT_FILENO, moduleFile.c_str(), moduleFile.size());
					sys_write(STDOUT_FILENO, " + ", 3);
					signal_safe_hex_print(moduleOffset);
					sys_write(STDOUT_FILENO, "\n", 1);
				}
				else
				{
					sys_write(STDOUT_FILENO, "unknown + ", 10);
					signal_safe_hex_print(moduleOffset);
					sys_write(STDOUT_FILENO, "\n", 1);
				}
			}

			freopen(dumpStoragePath, "a", stdout);
			PrintProcessState(processState, true, false, &resolver);
			fflush(stdout);
		}
	}

	sys_close(extra);

	return succeeded;
}

bool AcceleratorLocal::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	if (exceptionHandler_ || !hooks_.Empty())
	{
		ismm->Format(error, maxlen, "AcceleratorLocal is already initialized");
		return false;
	}
	if (!KHook::__exported__khook)
	{
		ismm->Format(error, maxlen, "AcceleratorLocal requires the MetaMod API18 KHook provider");
		return false;
	}
	GET_V_IFACE_CURRENT(GetServerFactory, server_, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, networkService_, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

	g_CrashMetadata.Reset();
	const char* baseDir = ismm->GetBaseDir();
	if (!baseDir || strlen(baseDir) + strlen("/addons/accelerator_local/dumps") >= sizeof(dumpStoragePath))
	{
		ismm->Format(error, maxlen, "AcceleratorLocal dump directory exceeds the metadata path limit");
		return false;
	}
	accelerator::CopyMetadata(crashGamePath, baseDir);
	ismm->Format(dumpStoragePath, sizeof(dumpStoragePath), "%s/addons/accelerator_local/dumps", ismm->GetBaseDir());
	accelerator::CopyMetadata(crashCommandLine, CommandLine() ? CommandLine()->GetCmdLine() : nullptr);
	
	struct stat st = {0};
	if (stat(dumpStoragePath, &st) == -1)
	{
		if(mkdir(dumpStoragePath, 0777) == -1)
		{
			ismm->Format(error, maxlen, "%s didn't exist and we couldn't create it :(", dumpStoragePath);
			return false;
		}
	}
	else if (!S_ISDIR(st.st_mode))
	{
		ismm->Format(error, maxlen, "AcceleratorLocal dump path is not a directory: %s", dumpStoragePath);
		return false;
	}
	else
		chmod(dumpStoragePath, 0777);

	const auto readSignal = [](int signal, struct sigaction& action) { return sigaction(signal, nullptr, &action) == 0; };
	if (!signals_.Prepare(readSignal))
	{
		ismm->Format(error, maxlen, "AcceleratorLocal could not read the existing signal handlers");
		return false;
	}

	// Hook registration precedes Breakpad installation: a rejected registration
	// leaves no signal callback in a library which MetaMod is about to close.
	if (!hooks_.Install(
		std::make_unique<FrameHook>(&IServerGameDLL::GameFrame, this, nullptr, &AcceleratorLocal::Api18GameFrame),
		std::make_unique<StartupHook>(&INetworkServerService::StartupServer, this, nullptr, &AcceleratorLocal::Api18StartupServer),
		server_, networkService_))
	{
		ismm->Format(error, maxlen, "AcceleratorLocal API18 hook registration rejected");
		return false;
	}

	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	exceptionHandler_ = new (std::nothrow) google_breakpad::ExceptionHandler(descriptor, nullptr, dumpCallback, nullptr, true, -1);
	if (!exceptionHandler_ || !signals_.Capture(readSignal))
	{
		hooks_.RollbackInitialization();
		delete exceptionHandler_;
		exceptionHandler_ = nullptr;
		signals_.Reset();
		ismm->Format(error, maxlen, "AcceleratorLocal could not establish its Breakpad signal handlers");
		return false;
	}

	if (late)
	{
		auto* gameServer = networkService_->GetIGameServer();
		StartupServer(gameServer ? gameServer->GetMapName() : nullptr);
	}
	
	return true;
}

bool AcceleratorLocal::Unload(char* error, size_t maxlen)
{
	if (!hooks_.Clear(KHook::__exported__khook != nullptr, g_CallbackActivity))
	{
		g_SMAPI->Format(error, maxlen, "AcceleratorLocal callback is active or KHook unavailable; unload refused");
		return false;
	}
	// Breakpad serializes removal with its signal-dispatch stack mutex. Destroy
	// it only after our engine callbacks have been synchronously removed.
	delete exceptionHandler_;
	exceptionHandler_ = nullptr;
	signals_.Reset();
	server_ = nullptr;
	networkService_ = nullptr;
	g_CrashMetadata.Reset();
	return true;
}

KHook::Return<void> AcceleratorLocal::Api18GameFrame(IServerGameDLL*, bool simulating, bool bFirstTick, bool bLastTick)
{
	accelerator::CallbackActivity::Scope callback(g_CallbackActivity);
	GameFrame(simulating, bFirstTick, bLastTick);
	return {KHook::Action::Ignore};
}

KHook::Return<void> AcceleratorLocal::Api18StartupServer(INetworkServerService*, const GameSessionConfiguration_t&,
	ISource2WorldSession*, const char* mapName)
{
	accelerator::CallbackActivity::Scope callback(g_CallbackActivity);
	StartupServer(mapName);
	return {KHook::Action::Ignore};
}

void AcceleratorLocal::GameFrame(bool, bool, bool)
{
	if (!exceptionHandler_) return;
	signals_.Repair(
		[](int signal, struct sigaction& action) { return sigaction(signal, nullptr, &action) == 0; },
		[](int signal, const struct sigaction& action) { return sigaction(signal, &action, nullptr) == 0; });
}

void AcceleratorLocal::StartupServer(const char* mapName)
{
	g_CrashMetadata.Map(mapName);
}

///////////////////////////////////////
const char* AcceleratorLocal::GetLicense()
{
	return "GPL";
}

const char* AcceleratorLocal::GetVersion()
{
	return "1.0.5-api18";
}

const char* AcceleratorLocal::GetDate()
{
	return __DATE__;
}

const char *AcceleratorLocal::GetLogTag()
{
	return "AcceleratorLocal";
}

const char* AcceleratorLocal::GetAuthor()
{
	return "Phoenix (˙·٠●Феникс●٠·˙), Asher Baker (asherkin)";
}

const char* AcceleratorLocal::GetDescription()
{
	return "Crash Handler";
}

const char* AcceleratorLocal::GetName()
{
	return "Accelerator local";
}

const char* AcceleratorLocal::GetURL()
{
	return "https://github.com/komashchenko/AcceleratorLocal";
}
