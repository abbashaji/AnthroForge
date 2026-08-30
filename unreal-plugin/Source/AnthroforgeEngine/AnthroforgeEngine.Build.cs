using UnrealBuildTool;

public class AnthroforgeEngine : ModuleRules
{
	public AnthroforgeEngine(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",
			"GeometryFramework", // UDynamicMeshComponent / FDynamicMesh3
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects", // IPluginManager, used to locate Binaries/ThirdParty at runtime
			"RenderCore",
			"RHI",
		});

		// anthroforge_core.dll/.so/.dylib is loaded dynamically at runtime via
		// FPlatformProcess::GetDllHandle (see UAnthroforgeCharacterAssembler),
		// not linked at build time, so no .lib/import-library setup or
		// RuntimeDependencies staging entry is required here beyond ensuring
		// copy_lib.sh/.bat has placed the file under Binaries/ThirdParty
		// before packaging. If this plugin is later packaged for
		// distribution, add a RuntimeDependencies.Add(...) entry pointing at
		// the platform-specific Binaries/ThirdParty artifact so the cooker
		// stages it alongside the game.
	}
}
