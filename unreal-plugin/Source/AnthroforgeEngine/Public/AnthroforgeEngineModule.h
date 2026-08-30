// AnthroforgeEngineModule.h
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/// Module entry point. The Rust dylib itself is loaded per-component by
/// UAnthroforgeCharacterAssembler (see its header for why), not globally
/// here — this module only logs load/unload for diagnostics.
class FAnthroforgeEngineModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
