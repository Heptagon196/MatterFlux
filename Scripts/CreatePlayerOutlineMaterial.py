import unreal


ASSET_PATH = "/Game/MatterFlux/Materials/M_PlayerOutline"
PACKAGE_PATH = "/Game/MatterFlux/Materials"
ASSET_NAME = "M_PlayerOutline"


def create_or_replace_material():
    existing = unreal.EditorAssetLibrary.load_asset(ASSET_PATH)
    if existing:
        unreal.EditorAssetLibrary.delete_asset(ASSET_PATH)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = asset_tools.create_asset(
        ASSET_NAME,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if not material:
        raise RuntimeError("Could not create M_PlayerOutline")

    material.set_editor_property(
        "material_domain",
        unreal.MaterialDomain.MD_POST_PROCESS,
    )
    # Run after tone mapping and after ordinary world post-process materials.
    # This keeps the occluded silhouette crisp and visually above opaque
    # occluders without drawing over Slate/UMG.
    material.set_editor_property(
        "blendable_location",
        unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING,
    )
    material.set_editor_property("blendable_priority", 1000)

    scene_color = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionSceneTexture,
        -420,
        -80,
    )
    scene_color.set_editor_property(
        "scene_texture_id",
        unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0,
    )

    scene_depth = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionSceneTexture,
        -420,
        30,
    )
    scene_depth.set_editor_property(
        "scene_texture_id",
        unreal.SceneTextureId.PPI_SCENE_DEPTH,
    )

    custom_depth = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionSceneTexture,
        -420,
        140,
    )
    custom_depth.set_editor_property(
        "scene_texture_id",
        unreal.SceneTextureId.PPI_CUSTOM_DEPTH,
    )

    custom_stencil = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionSceneTexture,
        -420,
        250,
    )
    custom_stencil.set_editor_property(
        "scene_texture_id",
        unreal.SceneTextureId.PPI_CUSTOM_STENCIL,
    )

    custom = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionCustom,
        -120,
        -80,
    )
    custom.set_editor_property(
        "description",
        "MatterFlux occluded-player inner outline",
    )
    custom.set_editor_property(
        "output_type",
        unreal.CustomMaterialOutputType.CMOT_FLOAT3,
    )
    inputs = []
    for name in (
        "SceneColor",
        "SceneDepthHint",
        "CustomDepthHint",
        "PlayerStencilHint",
    ):
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)
    custom.set_editor_property(
        "code",
        """
const float PlayerStencilValue = 1.0;
const float DepthBiasCm = 1.5;
float2 UV = GetDefaultSceneTextureUV(Parameters, PPI_PostProcessInput0);
float2 Pixel = View.BufferSizeAndInvSize.zw * 1.5;

float SceneDepth = SceneDepthHint.r;
float CustomDepth = CustomDepthHint.r;
float PlayerMask = 1.0 - step(0.5, abs(PlayerStencilHint.r - PlayerStencilValue));
float HiddenCenter = PlayerMask
    * (CustomDepth > SceneDepth + DepthBiasCm ? 1.0 : 0.0);

// Almost every screen pixel takes this branch, avoiding the 8-neighbor work
// unless the current pixel actually belongs to a hidden player surface.
[branch]
if (HiddenCenter < 0.5)
{
    return SceneColor.rgb;
}

// Erode the hidden-player mask and subtract it from the original mask. The
// resulting line lies inside the occluded part, so a visible body pixel can
// never be overwritten by the X-ray outline.
float HiddenInterior = 1.0;
[unroll]
for (int Y = -1; Y <= 1; ++Y)
{
    [unroll]
    for (int X = -1; X <= 1; ++X)
    {
        if (X == 0 && Y == 0)
        {
            continue;
        }
        float2 SampleUV = UV + float2(X, Y) * Pixel;
        float SampleSceneDepth = SceneTextureLookup(
            SampleUV, PPI_SceneDepth, false).r;
        float SampleCustomDepth = SceneTextureLookup(
            SampleUV, PPI_CustomDepth, false).r;
        float SampleStencil = SceneTextureLookup(
            SampleUV, PPI_CustomStencil, false).r;
        float SamplePlayer = 1.0 - step(
            0.5, abs(SampleStencil - PlayerStencilValue));
        float SampleHidden = SamplePlayer
            * (SampleCustomDepth > SampleSceneDepth + DepthBiasCm ? 1.0 : 0.0);
        HiddenInterior = min(HiddenInterior, SampleHidden);
    }
}

float Edge = saturate(HiddenCenter * (1.0 - HiddenInterior));
float3 OutlineColor = float3(0.018, 0.006, 0.002);
return lerp(SceneColor.rgb, OutlineColor, Edge);
""",
    )

    unreal.MaterialEditingLibrary.connect_material_expressions(
        scene_color,
        "Color",
        custom,
        "SceneColor",
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(
        scene_depth,
        "Color",
        custom,
        "SceneDepthHint",
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(
        custom_depth,
        "Color",
        custom,
        "CustomDepthHint",
    )
    unreal.MaterialEditingLibrary.connect_material_expressions(
        custom_stencil,
        "Color",
        custom,
        "PlayerStencilHint",
    )
    unreal.MaterialEditingLibrary.connect_material_property(
        custom,
        "",
        unreal.MaterialProperty.MP_EMISSIVE_COLOR,
    )
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("Created {}".format(ASSET_PATH))


create_or_replace_material()
