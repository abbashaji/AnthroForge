// AnthroforgeCharacterAssembler.cpp
#include "AnthroforgeCharacterAssembler.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Tasks/Task.h"
#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnthroforge, Log, All);

namespace
{
	/// Platform-specific dylib filename anthroforge_core builds to, matching
	/// copy_lib.sh/copy_lib.bat's output name.
	FString GetPlatformDylibFileName()
	{
#if PLATFORM_WINDOWS
		return TEXT("anthroforge_core.dll");
#elif PLATFORM_MAC
		return TEXT("libanthroforge_core.dylib");
#else
		return TEXT("libanthroforge_core.so");
#endif
	}

	/// Resolves the dylib path under this plugin's
	/// Binaries/ThirdParty directory. copy_lib.sh/.bat are responsible for
	/// actually placing the built artifact there.
	FString GetDylibPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AnthroforgeEngine"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: could not find the 'AnthroforgeEngine' plugin via IPluginManager; falling back to a relative path guess."));
			return FPaths::ProjectPluginsDir() / TEXT("AnthroforgeEngine/Source/AnthroforgeEngine/Binaries/ThirdParty") / GetPlatformDylibFileName();
		}
		return Plugin->GetBaseDir() / TEXT("Source/AnthroforgeEngine/Binaries/ThirdParty") / GetPlatformDylibFileName();
	}

	template <typename FnPtrType>
	bool ResolveExport(void* DllHandle, const TCHAR* SymbolName, FnPtrType& OutFn)
	{
		OutFn = reinterpret_cast<FnPtrType>(FPlatformProcess::GetDllExport(DllHandle, SymbolName));
		if (OutFn == nullptr)
		{
			UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: required export '%s' was not found in anthroforge_core. The plugin's C++ headers and the built .dll/.so/.dylib are out of sync — rebuild rust-core and re-run copy_lib."), SymbolName);
			return false;
		}
		return true;
	}
} // namespace

UAnthroforgeCharacterAssembler::UAnthroforgeCharacterAssembler()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UAnthroforgeCharacterAssembler::BeginPlay()
{
	Super::BeginPlay();

	if (!LoadLibraryAndResolveExports())
	{
		bIsLoaded = false;
		return;
	}
	bIsLoaded = true;

	if (AssetDirectory.IsEmpty())
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: AssetDirectory is empty; init_part_registry was not called. Set AssetDirectory before BeginPlay."));
		return;
	}

	const FTCHARToUTF8 Utf8AssetDir(*AssetDirectory);
	bRegistryInitialized = InitPartRegistryFn(Utf8AssetDir.Get());
	if (!bRegistryInitialized)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: init_part_registry('%s') returned false. See the Rust core's stderr output for the specific reason (this failure carries no error code across the FFI boundary today — see AnthroforgeCoreTypes.h, GAPS FLAGGED item 3)."), *AssetDirectory);
	}
}

void UAnthroforgeCharacterAssembler::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (DllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
	}
	InitPartRegistryFn = nullptr;
	GenerateCharacterFn = nullptr;
	FreeMeshBufferFn = nullptr;
	GenerateRuntimeAtlasFn = nullptr;
	FreeAtlasBufferFn = nullptr;
	LastErrorFn = nullptr;
	bIsLoaded = false;
	bRegistryInitialized = false;

	Super::EndPlay(EndPlayReason);
}

bool UAnthroforgeCharacterAssembler::LoadLibraryAndResolveExports()
{
	const FString DylibPath = GetDylibPath();

	if (!FPaths::FileExists(DylibPath))
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: dylib not found at '%s'. Run copy_lib.sh/copy_lib.bat after building rust-core."), *DylibPath);
		return false;
	}

	DllHandle = FPlatformProcess::GetDllHandle(*DylibPath);
	if (DllHandle == nullptr)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: FPlatformProcess::GetDllHandle failed for '%s'."), *DylibPath);
		return false;
	}

	bool bAllResolved = true;
	bAllResolved &= ResolveExport(DllHandle, TEXT("init_part_registry"), InitPartRegistryFn);
	bAllResolved &= ResolveExport(DllHandle, TEXT("generate_character"), GenerateCharacterFn);
	bAllResolved &= ResolveExport(DllHandle, TEXT("free_mesh_buffer"), FreeMeshBufferFn);
	bAllResolved &= ResolveExport(DllHandle, TEXT("generate_runtime_atlas"), GenerateRuntimeAtlasFn);
	bAllResolved &= ResolveExport(DllHandle, TEXT("free_atlas_buffer"), FreeAtlasBufferFn);
	bAllResolved &= ResolveExport(DllHandle, TEXT("anthroforge_last_error"), LastErrorFn);

	if (!bAllResolved)
	{
		FPlatformProcess::FreeDllHandle(DllHandle);
		DllHandle = nullptr;
		return false;
	}

	UE_LOG(LogAnthroforge, Log, TEXT("AnthroforgeCharacterAssembler: loaded '%s' and resolved all %d exports."), *DylibPath, 6);
	return true;
}

void UAnthroforgeCharacterAssembler::AssembleCharacterAsync(FAnthroforgeCharacterDNA DNA, UDynamicMeshComponent* TargetMeshComponent)
{
	if (!bIsLoaded)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler::AssembleCharacterAsync called but the dylib is not loaded."));
		OnAssembleComplete.Broadcast(false);
		return;
	}
	if (!bRegistryInitialized)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler::AssembleCharacterAsync called but init_part_registry never succeeded."));
		OnAssembleComplete.Broadcast(false);
		return;
	}
	if (TargetMeshComponent == nullptr)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler::AssembleCharacterAsync called with a null TargetMeshComponent."));
		OnAssembleComplete.Broadcast(false);
		return;
	}

	// Weak pointers for the background task/continuation to check safely —
	// the component or its owning actor may be destroyed before either
	// stage runs.
	TWeakObjectPtr<UAnthroforgeCharacterAssembler> WeakThis(this);
	TWeakObjectPtr<UDynamicMeshComponent> WeakTarget(TargetMeshComponent);

	// Character generation must never run on the Game Thread. UE::Tasks
	// gives us a plain background task; the continuation below is
	// explicitly re-dispatched onto the Game Thread for the mesh upload,
	// since FDynamicMesh3 editing is not safe to interleave with
	// concurrent rendering/game-thread access.
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, WeakTarget, DNA]()
	{
		UAnthroforgeCharacterAssembler* StrongThis = WeakThis.Get();
		if (StrongThis == nullptr)
		{
			return;
		}

		FAssembledMeshData MeshData = StrongThis->GenerateCharacterMeshData(DNA);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakTarget, MeshData = MoveTemp(MeshData)]()
		{
			UAnthroforgeCharacterAssembler* StrongThisGT = WeakThis.Get();
			UDynamicMeshComponent* StrongTargetGT = WeakTarget.Get();

			if (StrongThisGT == nullptr)
			{
				// Component was destroyed mid-flight; nothing left to
				// broadcast to and nothing left to upload onto.
				return;
			}

			bool bFinalSuccess = MeshData.bSuccess;
			if (bFinalSuccess && StrongTargetGT != nullptr)
			{
				ApplyMeshToComponent(MeshData, StrongTargetGT);
			}
			else if (bFinalSuccess && StrongTargetGT == nullptr)
			{
				UE_LOG(LogAnthroforge, Warning, TEXT("AnthroforgeCharacterAssembler: mesh generated successfully but TargetMeshComponent was destroyed before upload; discarding."));
				bFinalSuccess = false;
			}

			StrongThisGT->OnAssembleComplete.Broadcast(bFinalSuccess);
		});
	});
}

UAnthroforgeCharacterAssembler::FAssembledMeshData UAnthroforgeCharacterAssembler::GenerateCharacterMeshData(const FAnthroforgeCharacterDNA& DNA)
{
	FAssembledMeshData Result;

	// Build the tightly-packed FFI struct explicitly rather than
	// reinterpret_casting FAnthroforgeCharacterDNA: the Blueprint-facing
	// struct and the FFI struct are intentionally kept as two separate
	// types (see AnthroforgeCoreTypes.h) so UHT-generated reflection
	// metadata on the former can never silently affect the latter's layout.
	//
	// Zero-initialize first (rather than leaving it default-constructed
	// with 5 of 7 fields set by hand) so any field left unset here — today
	// or after a future field is added and someone forgets to wire it up —
	// defaults to zero/null instead of indeterminate stack memory. This is
	// the fix for a real bug: EquippedClothingIdsPtr/EquippedClothingCount
	// were previously left uninitialized whenever this struct only set the
	// other 5 fields, and the Rust core now actually reads those two
	// fields, so garbage here was live undefined behavior (a bogus nonzero
	// count paired with an invalid pointer).
	FAnthroforgeCharacterDNA_FFI FfiDna = {};
	FfiDna.Seed = static_cast<uint64>(DNA.Seed); // bit-pattern reinterpret, not a numeric conversion
	FfiDna.HeightModifier = DNA.HeightModifier;
	FfiDna.WeightModifier = DNA.WeightModifier;
	FfiDna.HeadId = static_cast<uint32>(DNA.HeadId);
	FfiDna.TorsoId = static_cast<uint32>(DNA.TorsoId);

	// TArray<int32> -> const uint32* is a legitimate, safe reinterpret, not
	// a workaround for a real type mismatch: both are plain 4-byte
	// integers with identical per-element layout, and every real part id
	// is non-negative, so no bit pattern changes meaning. Do NOT "fix"
	// this into an element-by-element copy or a separate TArray<uint32> —
	// that would be unnecessary allocation/copying for a same-size,
	// same-layout reinterpret.
	//
	// GetData() on an empty TArray is not guaranteed to return nullptr in
	// Unreal's implementation, but that's fine here: generate_character's
	// doc comment in rust-core/src/lib.rs (see the CharacterDNA field
	// comments) treats `equipped_clothing_count == 0` alone as the "no
	// clothing equipped" signal, and explicitly tolerates a non-null
	// pointer paired with a zero count — it never dereferences the
	// pointer in that case. So setting the pointer unconditionally from
	// GetData() and the count from Num() is correct with no special-case
	// branch for the empty-array case.
	FfiDna.EquippedClothingIdsPtr = reinterpret_cast<const uint32*>(DNA.EquippedClothingIds.GetData());
	FfiDna.EquippedClothingCount = static_cast<uint32>(DNA.EquippedClothingIds.Num());

	// Pointer-lifetime check: EquippedClothingIdsPtr above points into
	// DNA.EquippedClothingIds's own backing allocation. DNA is a const&
	// parameter of this function, nothing between here and the
	// GenerateCharacterFn call below mutates DNA (or reassigns/appends to
	// EquippedClothingIds), and this function makes only the one,
	// synchronous FFI call — there is no reentrant or async call in
	// between that could reallocate it. So the pointer stays valid for
	// the full duration of the call immediately below.
	FAnthroforgeMeshOutputBuffer* Buffer = GenerateCharacterFn(&FfiDna);
	if (Buffer == nullptr)
	{
		// anthroforge_last_error() (resolved the same way GenerateCharacterFn
		// is, in LoadLibraryAndResolveExports) reports the actual reason for
		// the most recent failure on this thread — a bad head_id is only one
		// of several possible causes (an unresolvable TorsoId, or a failure
		// during DNA mutation, can also return null here), so ask it instead
		// of guessing. Its return value is borrowed, not owned (see its doc
		// comment in AnthroforgeCoreTypes.h): copy it into an FString via
		// UTF8_TO_TCHAR immediately, before any other call on this thread
		// into the library could invalidate it.
		const char* RawLastError = (LastErrorFn != nullptr) ? LastErrorFn() : nullptr;
		const FString LastErrorMessage = (RawLastError != nullptr) ? FString(UTF8_TO_TCHAR(RawLastError)) : TEXT("<no last-error available>");
		UE_LOG(LogAnthroforge, Error, TEXT("AnthroforgeCharacterAssembler: generate_character returned null (head_id=%d, torso_id=%d): %s"), DNA.HeadId, DNA.TorsoId, *LastErrorMessage);
		Result.bSuccess = false;
		return Result;
	}

	// Copy the data out of the Rust-owned buffer into plain TArrays, then
	// immediately free the Rust buffer. This keeps buffer ownership
	// entirely within this function: nothing Rust-owned survives past this
	// point, so the Game Thread continuation only ever touches
	// plugin-owned memory.
	Result.Vertices.Append(Buffer->VerticesPtr, Buffer->VerticesCount);
	Result.Indices.Append(Buffer->IndicesPtr, Buffer->IndicesCount);
	Result.bSuccess = true;

	FreeMeshBufferFn(Buffer);

	return Result;
}

void UAnthroforgeCharacterAssembler::ApplyMeshToComponent(const FAssembledMeshData& MeshData, UDynamicMeshComponent* TargetMeshComponent)
{
	check(IsInGameThread());

	using namespace UE::Geometry;

	TargetMeshComponent->GetDynamicMesh()->EditMesh([&MeshData](FDynamicMesh3& EditMesh)
	{
		EditMesh.Clear();
		EditMesh.EnableAttributes();
		EditMesh.Attributes()->EnablePrimaryColors();
		EditMesh.Attributes()->SetNumUVLayers(1);
		FDynamicMeshNormalOverlay* NormalOverlay = EditMesh.Attributes()->PrimaryNormals();
		FDynamicMeshUVOverlay* UVOverlay = EditMesh.Attributes()->PrimaryUV();

		// Reserve is the realistic zero-copy-ish optimization available
		// here: FDynamicMesh3 is an indexed/attributed structure (not a
		// flat vertex/index buffer), so a raw FMemory::Memcpy from the
		// Rust arrays isn't possible — every vertex/triangle still has to
		// go through AppendVertex/AppendTriangle. (If a flat-buffer upload
		// is preferred instead, switch TargetMeshComponent to a
		// UProceduralMeshComponent and use CreateMeshSection with
		// TArray::SetNumUninitialized + FMemory::Memcpy from
		// MeshData.Vertices.GetData() there instead — don't mix both
		// component types in the same pipeline.)
		EditMesh.Reserve(MeshData.Vertices.Num(), MeshData.Indices.Num() / 3, MeshData.Vertices.Num());

		TArray<int32> VertexIdMap;
		VertexIdMap.SetNumUninitialized(MeshData.Vertices.Num());
		TArray<int32> NormalIdMap;
		NormalIdMap.SetNumUninitialized(MeshData.Vertices.Num());
		TArray<int32> UVIdMap;
		UVIdMap.SetNumUninitialized(MeshData.Vertices.Num());

		for (int32 VertIdx = 0; VertIdx < MeshData.Vertices.Num(); ++VertIdx)
		{
			const FAnthroforgeSkinnedVertex& SrcVert = MeshData.Vertices[VertIdx];
			const FVector3d Position(SrcVert.Position[0], SrcVert.Position[1], SrcVert.Position[2]);
			const int32 NewVid = EditMesh.AppendVertex(Position);
			VertexIdMap[VertIdx] = NewVid;

			const FVector3f Normal(SrcVert.Normal[0], SrcVert.Normal[1], SrcVert.Normal[2]);
			NormalIdMap[VertIdx] = NormalOverlay->AppendElement(Normal);

			const FVector2f UV(SrcVert.UV[0], SrcVert.UV[1]);
			UVIdMap[VertIdx] = UVOverlay->AppendElement(UV);

			// NOTE: SkinnedVertex's bone_indices/bone_weights are not
			// consumed here. FDynamicMesh3/UDynamicMeshComponent has no
			// built-in per-vertex skin-weight channel; wiring runtime
			// skinning through this component (vs. e.g. a
			// USkeletalMeshComponent-based path) is outside this phase's
			// "must never block the Game Thread" mandate and is flagged
			// here rather than silently dropped without comment.
		}

		for (int32 TriStart = 0; TriStart + 2 < MeshData.Indices.Num(); TriStart += 3)
		{
			const int32 A = VertexIdMap[MeshData.Indices[TriStart + 0]];
			const int32 B = VertexIdMap[MeshData.Indices[TriStart + 1]];
			const int32 C = VertexIdMap[MeshData.Indices[TriStart + 2]];
			const int32 NewTid = EditMesh.AppendTriangle(A, B, C);
			if (NewTid >= 0)
			{
				NormalOverlay->SetTriangle(NewTid, FIndex3i(
					NormalIdMap[MeshData.Indices[TriStart + 0]],
					NormalIdMap[MeshData.Indices[TriStart + 1]],
					NormalIdMap[MeshData.Indices[TriStart + 2]]));
				UVOverlay->SetTriangle(NewTid, FIndex3i(
					UVIdMap[MeshData.Indices[TriStart + 0]],
					UVIdMap[MeshData.Indices[TriStart + 1]],
					UVIdMap[MeshData.Indices[TriStart + 2]]));
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	TargetMeshComponent->NotifyMeshUpdated();
}

UTexture2D* AnthroforgeTextureUtils::CreateTransientRuntimeTexture(const FAnthroforgeRawImage& Image)
{
	if (Image.PixelsPtr == nullptr || Image.Width == 0 || Image.Height == 0)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("CreateTransientRuntimeTexture: invalid RawImage (null pixels or zero dimensions)."));
		return nullptr;
	}

	const uint32 ExpectedBytes = Image.Width * Image.Height * 4;
	if (Image.TotalBytes != ExpectedBytes)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("CreateTransientRuntimeTexture: TotalBytes (%u) does not match Width*Height*4 (%u) for a %ux%u image."), Image.TotalBytes, ExpectedBytes, Image.Width, Image.Height);
		return nullptr;
	}

	UTexture2D* NewTexture = UTexture2D::CreateTransient(Image.Width, Image.Height, PF_R8G8B8A8);
	if (NewTexture == nullptr)
	{
		UE_LOG(LogAnthroforge, Error, TEXT("CreateTransientRuntimeTexture: UTexture2D::CreateTransient failed for a %ux%u image."), Image.Width, Image.Height);
		return nullptr;
	}

#if WITH_EDITORONLY_DATA
	NewTexture->MipGenSettings = TMGS_NoMipmaps;
#endif
	NewTexture->NeverStream = true;
	NewTexture->SRGB = true;

	FTexture2DMipMap& Mip0 = NewTexture->GetPlatformData()->Mips[0];
	void* MipData = Mip0.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, Image.PixelsPtr, Image.TotalBytes);
	Mip0.BulkData.Unlock();

	NewTexture->UpdateResource();
	return NewTexture;
}
