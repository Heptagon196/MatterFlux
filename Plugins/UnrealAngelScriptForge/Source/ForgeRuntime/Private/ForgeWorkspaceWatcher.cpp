#include "ForgeWorkspaceWatcher.h"

#include "ForgePatchManager.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogForgeWorkspace, Log, All);

namespace
{
bool LoadJsonObject(const FString& File, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *File))
    {
        OutError = FString::Printf(TEXT("Unable to read %s"), *File);
        return false;
    }

    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
    if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
    {
        OutError = FString::Printf(TEXT("Invalid JSON in %s"), *File);
        return false;
    }
    return true;
}

bool RequiredString(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Field,
    FString& OutValue,
    FString& OutError)
{
    if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Required string field '%s' is missing"), Field);
        return false;
    }
    return true;
}
}

FForgeWorkspaceWatcher::FForgeWorkspaceWatcher(FForgePatchManager& InPatchManager)
    : PatchManager(InPatchManager)
{
}

FForgeWorkspaceWatcher::~FForgeWorkspaceWatcher()
{
    Stop();
}

void FForgeWorkspaceWatcher::Start()
{
    if (!FParse::Value(FCommandLine::Get(), TEXT("ForgeWorkspace="), WorkspaceRoot))
    {
        WorkspaceRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("FORGE_WORKSPACE"));
    }
    if (WorkspaceRoot.IsEmpty())
    {
        UE_LOG(
            LogForgeWorkspace,
            Display,
            TEXT("Forge Workspace watcher is disabled; pass -ForgeWorkspace=<path> or set FORGE_WORKSPACE"));
        return;
    }

    WorkspaceRoot = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
    FPaths::NormalizeDirectoryName(WorkspaceRoot);
    ActivePatchFile = FPaths::Combine(WorkspaceRoot, TEXT("state"), TEXT("active-patch.json"));
    RuntimeStatusFile = FPaths::Combine(WorkspaceRoot, TEXT("state"), TEXT("runtime-status.json"));
    WriteRuntimeStatus(TEXT("disabled"), FString(), FString(), FString());
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FForgeWorkspaceWatcher::Tick),
        0.25f);
    UE_LOG(LogForgeWorkspace, Display, TEXT("Watching Forge Workspace: %s"), *WorkspaceRoot);
}

void FForgeWorkspaceWatcher::Stop()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    DisableActivePatch();
}

bool FForgeWorkspaceWatcher::Tick(float DeltaSeconds)
{
    const bool bExists = IFileManager::Get().FileExists(*ActivePatchFile);
    if (!bExists)
    {
        if (bObservedFile)
        {
            bObservedFile = false;
            LastObservedContent.Reset();
            DisableActivePatch();
        }
        return true;
    }

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *ActivePatchFile))
    {
        Content = TEXT("<unreadable-active-patch>");
    }
    if (bObservedFile && Content == LastObservedContent)
    {
        return true;
    }
    bObservedFile = true;
    LastObservedContent = MoveTemp(Content);
    LoadActivePatch();
    return true;
}

bool FForgeWorkspaceWatcher::LoadActivePatch()
{
    FString Error;
    auto Reject = [this](const FString& RequestedRevision, const FString& Message)
    {
        UE_LOG(LogForgeWorkspace, Error, TEXT("Forge Patch request was rejected: %s"), *Message);
        WriteRuntimeStatus(
            TEXT("rejected"),
            RequestedRevision,
            ActiveRevision,
            ActiveSymbolId,
            Message);
        return false;
    };
    TSharedPtr<FJsonObject> Active;
    if (!LoadJsonObject(ActivePatchFile, Active, Error))
    {
        return Reject(FString(), Error);
    }

    FString Revision;
    FString ManifestReference;
    if (!RequiredString(Active, TEXT("revision"), Revision, Error) ||
        !RequiredString(Active, TEXT("manifest"), ManifestReference, Error))
    {
        return Reject(Revision, FString::Printf(TEXT("Invalid active Patch state: %s"), *Error));
    }
    if (Revision == ActiveRevision)
    {
        WriteRuntimeStatus(
            TEXT("activated"),
            Revision,
            ActiveRevision,
            ActiveSymbolId);
        return true;
    }

    FString ManifestFile = FPaths::Combine(FPaths::GetPath(ActivePatchFile), ManifestReference);
    ManifestFile = FPaths::ConvertRelativePathToFull(ManifestFile);
    FPaths::CollapseRelativeDirectories(ManifestFile);
    TSharedPtr<FJsonObject> Manifest;
    if (!LoadJsonObject(ManifestFile, Manifest, Error))
    {
        return Reject(Revision, Error);
    }

    FString SymbolId;
    FString EntryFunction;
    FString BuildFingerprint;
    FString ScriptReference;
    FString ModeString;
    if (!RequiredString(Manifest, TEXT("targetSymbolId"), SymbolId, Error) ||
        !RequiredString(Manifest, TEXT("entryFunction"), EntryFunction, Error) ||
        !RequiredString(Manifest, TEXT("mode"), ModeString, Error) ||
        !RequiredString(Manifest, TEXT("buildFingerprint"), BuildFingerprint, Error) ||
        !RequiredString(Manifest, TEXT("script"), ScriptReference, Error))
    {
        return Reject(Revision, FString::Printf(TEXT("Invalid Patch manifest: %s"), *Error));
    }
    EForgePatchMode Mode;
    if (ModeString == TEXT("Replace"))
    {
        Mode = EForgePatchMode::Replace;
    }
    else if (ModeString == TEXT("Wrap"))
    {
        Mode = EForgePatchMode::Wrap;
    }
    else
    {
        return Reject(Revision, FString::Printf(TEXT("Invalid Patch mode: %s"), *ModeString));
    }

    const FString ScriptFile = FPaths::Combine(FPaths::GetPath(ManifestFile), ScriptReference);
    FString ScriptSource;
    if (!FFileHelper::LoadFileToString(ScriptSource, *ScriptFile))
    {
        return Reject(Revision, FString::Printf(TEXT("Unable to read Patch script: %s"), *ScriptFile));
    }

    const FForgePatchResult Result = PatchManager.ApplyPatch(
        SymbolId,
        ScriptSource,
        EntryFunction,
        Mode,
        BuildFingerprint);
    if (!Result.bSuccess)
    {
        return Reject(Revision, Result.Error);
    }

    if (!ActiveSymbolId.IsEmpty() && ActiveSymbolId != SymbolId)
    {
        PatchManager.DisablePatch(ActiveSymbolId);
    }
    ActiveRevision = Revision;
    ActiveSymbolId = SymbolId;
    WriteRuntimeStatus(
        TEXT("activated"),
        Revision,
        ActiveRevision,
        ActiveSymbolId);
    UE_LOG(LogForgeWorkspace, Display, TEXT("Activated Forge Patch %s for %s"), *Revision, *SymbolId);
    return true;
}

void FForgeWorkspaceWatcher::DisableActivePatch()
{
    const FString PreviousRevision = ActiveRevision;
    const FString PreviousSymbolId = ActiveSymbolId;
    if (!ActiveSymbolId.IsEmpty())
    {
        PatchManager.DisablePatch(ActiveSymbolId);
        UE_LOG(LogForgeWorkspace, Display, TEXT("Disabled Forge Patch for %s"), *ActiveSymbolId);
    }
    ActiveRevision.Reset();
    ActiveSymbolId.Reset();
    if (!RuntimeStatusFile.IsEmpty())
    {
        WriteRuntimeStatus(
            TEXT("disabled"),
            PreviousRevision,
            FString(),
            PreviousSymbolId);
    }
}

void FForgeWorkspaceWatcher::WriteRuntimeStatus(
    const TCHAR* Status,
    const FString& RequestedRevision,
    const FString& ActiveRevisionValue,
    const FString& SymbolId,
    const FString& Error)
{
    if (RuntimeStatusFile.IsEmpty())
    {
        return;
    }
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("schemaVersion"), 1);
    Object->SetStringField(TEXT("status"), Status);
    Object->SetStringField(TEXT("requestedRevision"), RequestedRevision);
    Object->SetStringField(TEXT("activeRevision"), ActiveRevisionValue);
    Object->SetStringField(TEXT("targetSymbolId"), SymbolId);
    Object->SetStringField(TEXT("error"), Error);
    Object->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());

    FString Content;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Content);
    if (!FJsonSerializer::Serialize(Object, Writer))
    {
        UE_LOG(LogForgeWorkspace, Error, TEXT("Unable to serialize Forge Runtime status"));
        return;
    }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(RuntimeStatusFile), true);
    const FString Temporary = RuntimeStatusFile + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(
            Content,
            *Temporary,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !IFileManager::Get().Move(*RuntimeStatusFile, *Temporary, true, true))
    {
        IFileManager::Get().Delete(*Temporary, false, true);
        UE_LOG(
            LogForgeWorkspace,
            Error,
            TEXT("Unable to write Forge Runtime status: %s"),
            *RuntimeStatusFile);
    }
}
