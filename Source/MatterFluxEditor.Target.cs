// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class MatterFluxEditorTarget : TargetRules
{
	public MatterFluxEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.AddRange(new string[]
		{
			"MatterFlux",
			"MatterFluxDeveloper",
			"MatterFluxTests"
		});
	}
}
