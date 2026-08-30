// AnthroforgeCharacterAssembler.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AnthroforgeCoreTypes.h"
#include "AnthroforgeCharacterAssembler.generated.h"

class UDynamicMeshComponent;
class UTexture2D;

/// Loads anthroforge_core's dynamic library, resolves its exported function
/// pointers, and drives character-mesh assembly off the Game Thread.
///
/// # Threading
/// `AssembleCharacterAsync` dispatches the actual `generate_character` FFI
/// call on a background task (`UE::Tasks::Launch`); character generation
/// must never run on the Game Thread per this plugin's mandate. The
/// `UDynamicMeshComponent` upload is marshalled back to the Game Thread via
/// a continuation, since `FDynamicMesh3` editing is not thread-safe against
/// concurrent rendering/game-thread access.
///
/// # Lifecycle
/// The dylib handle is acquired in `BeginPlay` and released in `EndPlay`.
/// Every function pointer is validated at load time; if any expected export
/// is missing, `bIsLoaded` stays false and every public entry point below
/// fails safely (logs an error, invokes the completion delegate with
/// `bSuccess=false`) rather than dereferencing a null function pointer.
UCLASS(ClassGroup = (Anthroforge), meta = (BlueprintSpawnableComponent))
class ANTHROFORGEENGINE_API UAnthroforgeCharacterAssembler : public UActorComponent
{
	GENERATED_BODY()

public:
	UAnthroforgeCharacterAssembler();

	/// Directory handed to `init_part_registry` (containing
	/// `master_skeleton.json` and the modular `.gltf`/`.glb`/`.obj` part
	/// files). Must be set (e.g. via a project-relative path resolved in
	/// Blueprint/C++) before `BeginPlay`, since registry init happens once,
	/// at `BeginPlay`, immediately after the dylib is loaded.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Anthroforge")
	FString AssetDirectory;

	/// Assemble a character mesh for the given DNA and upload it onto
	/// `TargetMeshComponent`. Safe to call multiple times on the same
	/// component (each call replaces the mesh). Runs the actual FFI call
	/// and mesh construction off the Game Thread; the mesh upload itself is
	/// marshalled back onto the Game Thread automatically.
	///
	/// `OnComplete` fires on the Game Thread exactly once per call,
	/// regardless of success or failure.
	UFUNCTION(BlueprintCallable, Category = "Anthroforge")
	void AssembleCharacterAsync(FAnthroforgeCharacterDNA DNA, UDynamicMeshComponent* TargetMeshComponent);

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAnthroforgeAssembleComplete, bool, bSuccess);

	/// Fired on the Game Thread once assembly finishes (success or failure).
	UPROPERTY(BlueprintAssignable, Category = "Anthroforge")
	FAnthroforgeAssembleComplete OnAssembleComplete;

	/// True once the dylib was loaded and every required export resolved
	/// successfully. All public entry points no-op (and log) if this is
	/// false.
	UPROPERTY(BlueprintReadOnly, Category = "Anthroforge")
	bool bIsLoaded = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/// Handle returned by FPlatformProcess::GetDllHandle. Null if loading
	/// failed or hasn't happened yet.
	void* DllHandle = nullptr;

	FAnthroforge_InitPartRegistry InitPartRegistryFn = nullptr;
	FAnthroforge_GenerateCharacter GenerateCharacterFn = nullptr;
	FAnthroforge_FreeMeshBuffer FreeMeshBufferFn = nullptr;
	FAnthroforge_GenerateRuntimeAtlas GenerateRuntimeAtlasFn = nullptr;
	FAnthroforge_FreeAtlasBuffer FreeAtlasBufferFn = nullptr;

	/// Resolved the same way every other export above is (see
	/// LoadLibraryAndResolveExports). Used by GenerateCharacterMeshData to
	/// surface the specific reason a null GenerateCharacterFn return
	/// happened, instead of only guessing from context (see
	/// AnthroforgeCoreTypes.h, GAPS FLAGGED item 3).
	FAnthroforge_LastError LastErrorFn = nullptr;

	/// True once `InitPartRegistryFn(AssetDirectory)` has returned true.
	/// `generate_character` is documented as UB-adjacent (returns null,
	/// per the Rust safety contract, rather than actual UB) if called
	/// before this succeeds, so this component refuses to call it until
	/// then.
	bool bRegistryInitialized = false;

	/// Resolves `anthroforge_core`'s dylib path for the current platform
	/// under this plugin's Binaries/ThirdParty directory, loads it via
	/// FPlatformProcess::GetDllHandle, and resolves every function pointer
	/// above. Logs a clear error (naming the specific missing export) and
	/// leaves `bIsLoaded=false` on any failure.
	bool LoadLibraryAndResolveExports();

	/// Background-thread work: calls generate_character, converts the raw
	/// FAnthroforgeMeshOutputBuffer into a plain copyable struct (so the
	/// Rust buffer can be freed via FreeMeshBufferFn before the Game
	/// Thread continuation runs), and returns whether it succeeded.
	struct FAssembledMeshData
	{
		TArray<FAnthroforgeSkinnedVertex> Vertices;
		TArray<uint32> Indices;
		bool bSuccess = false;
	};
	FAssembledMeshData GenerateCharacterMeshData(const FAnthroforgeCharacterDNA& DNA);

	/// Game-thread work: uploads FAssembledMeshData onto TargetMeshComponent
	/// via UDynamicMeshComponent's FDynamicMesh3 editing API.
	static void ApplyMeshToComponent(const FAssembledMeshData& MeshData, UDynamicMeshComponent* TargetMeshComponent);
};

/// Utility namespace for texture upload from the Rust-produced RawImage.
namespace AnthroforgeTextureUtils
{
	/// Creates a transient UTexture2D from an RGBA8 FAnthroforgeRawImage,
	/// uploads its pixel data via Lock/Memcpy/Unlock + UpdateResource, and
	/// returns it. Returns nullptr (and logs) if Image.PixelsPtr is null,
	/// dimensions are zero, or TotalBytes doesn't match Width*Height*4.
	///
	/// Does NOT free the source RawImage's backing buffer — callers still
	/// own that responsibility (typically via FreeAtlasBufferFn once the
	/// upload above has copied the pixel data out).
	ANTHROFORGEENGINE_API UTexture2D* CreateTransientRuntimeTexture(const FAnthroforgeRawImage& Image);
}
