using System.IO;
using UnrealBuildTool;

public class MatterFluxLua : ModuleRules
{
	public MatterFluxLua(ReadOnlyTargetRules Target) : base(Target)
	{
		// Lua's internal TString type intentionally lives in the global
		// namespace and conflicts with UE's TString alias. Keep the language
		// core in its own translation unit without a UE PCH or unity merge.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core"
		});

		PrivateIncludePaths.Add(
			Path.Combine(ModuleDirectory, "../../ThirdParty/Lua/src"));

		PublicDefinitions.Add("MATTERFLUX_LUA_SCHEMA_VERSION=3");
	}
}
