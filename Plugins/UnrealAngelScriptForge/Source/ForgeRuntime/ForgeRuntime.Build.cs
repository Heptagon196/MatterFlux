using UnrealBuildTool;
using System.IO;

public class ForgeRuntime : ModuleRules
{
    public ForgeRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        if (Target.Platform != UnrealTargetPlatform.Win64)
        {
            throw new BuildException("Unreal AngelScript Forge currently supports Win64 only.");
        }

        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        IWYUSupport = IWYUSupport.None;
        bUseUnity = false;
        CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Json"
        });

        PrivateIncludePaths.AddRange(new[]
        {
            Path.Combine(ModuleDirectory, "Private", "ThirdParty", "AngelScript", "include"),
            Path.Combine(ModuleDirectory, "Private", "ThirdParty", "AngelScript", "source"),
            Path.Combine(ModuleDirectory, "Private", "ThirdParty", "MinHook", "include"),
            Path.Combine(ModuleDirectory, "Private", "ThirdParty", "MinHook", "src")
        });

        PrivateDefinitions.AddRange(new[]
        {
            "AS_MAX_PORTABILITY=1",
            "AS_NO_EXCEPTIONS=1",
            "AS_DEBUG=0",
            "AS_USE_COMPUTED_GOTOS=0",
            "_XBOX_VER=0",
            "_CRT_SECURE_NO_WARNINGS=1"
        });
    }
}
