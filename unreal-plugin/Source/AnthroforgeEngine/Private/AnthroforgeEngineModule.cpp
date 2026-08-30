// AnthroforgeEngineModule.cpp
#include "AnthroforgeEngineModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnthroforgeModule, Log, All);

void FAnthroforgeEngineModule::StartupModule()
{
	UE_LOG(LogAnthroforgeModule, Log, TEXT("AnthroforgeEngine module started. anthroforge_core.dll/.so/.dylib is loaded lazily per-UAnthroforgeCharacterAssembler at BeginPlay, not here."));
}

void FAnthroforgeEngineModule::ShutdownModule()
{
	UE_LOG(LogAnthroforgeModule, Log, TEXT("AnthroforgeEngine module shutting down."));
}

IMPLEMENT_MODULE(FAnthroforgeEngineModule, AnthroforgeEngine)
