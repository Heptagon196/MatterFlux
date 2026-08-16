"""Create the project-owned MatterFlux voxel palette material.

Run from Unreal Editor:
  UnrealEditor.exe MatterFlux.uproject -ExecutePythonScript=Tools/CreateMatterFluxVoxelMaterials.py

The script is intentionally idempotent. It rebuilds the expression graph when
the asset already exists so the material remains reproducible from source.
"""

import unreal


ASSET_PATH = "/Game/MatterFlux/Materials/M_VoxelPalette"
ASSET_NAME = "M_VoxelPalette"
GAS_ASSET_PATH = "/Game/MatterFlux/Materials/M_VoxelGas"
GAS_ASSET_NAME = "M_VoxelGas"
PACKAGE_PATH = "/Game/MatterFlux/Materials"


def expression(material, expression_class, x, y):
    result = unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, x, y
    )
    if result is None:
        raise RuntimeError(f"Could not create {expression_class}")
    return result


def vector_parameter(material, name, value, x, y):
    node = expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    node.set_editor_property("parameter_name", unreal.Name(name))
    node.set_editor_property("default_value", value)
    return node


def scalar_parameter(material, name, value, x, y):
    node = expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", unreal.Name(name))
    node.set_editor_property("default_value", value)
    return node


def connect(source, output_name, target, input_name):
    if not unreal.MaterialEditingLibrary.connect_material_expressions(
        source, output_name, target, input_name
    ):
        raise RuntimeError(
            f"Could not connect {source.get_name()}:{output_name} "
            f"to {target.get_name()}:{input_name}"
        )


def connect_property(source, output_name, material_property):
    if not unreal.MaterialEditingLibrary.connect_material_property(
        source, output_name, material_property
    ):
        raise RuntimeError(
            f"Could not connect {source.get_name()} to {material_property}"
        )


def custom_input(name):
    result = unreal.CustomInput()
    result.set_editor_property("input_name", unreal.Name(name))
    return result


def create_material(asset_path, asset_name):
    # UE 5.8 can assert in DeleteAllMaterialExpressions when expressions from an
    # asset loaded by Python are still rooted by the material editor subsystem.
    # This asset is fully generated and has no hand-authored state, so replacing
    # the package is both safer and more reproducible than mutating its graph.
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        if not unreal.EditorAssetLibrary.delete_asset(asset_path):
            raise RuntimeError(f"Could not replace generated asset {asset_path}")

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        asset_name,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if not isinstance(material, unreal.Material):
        raise RuntimeError(f"{asset_path} is not a Material asset")
    return material


def build_material():
    material = create_material(ASSET_PATH, ASSET_NAME)

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)
    unreal.MaterialEditingLibrary.set_base_material_usage(
        material,
        unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
        True,
    )

    base_color = vector_parameter(
        material, "Color", unreal.LinearColor(0.18, 0.52, 0.12, 1.0), -900, -220
    )
    normal = expression(material, unreal.MaterialExpressionPixelNormalWS, -900, -80)
    world_position = expression(
        material, unreal.MaterialExpressionWorldPosition, -900, 60
    )
    face_contrast = scalar_parameter(material, "FaceContrast", 0.72, -900, 200)
    color_variation = scalar_parameter(
        material, "ColorVariation", 0.055, -900, 320
    )
    pixel_size = scalar_parameter(material, "PixelSize", 12.0, -900, 440)

    shade = expression(material, unreal.MaterialExpressionCustom, -380, 0)
    shade.set_editor_property(
        "description", "Quantized world-space voxel palette shading"
    )
    shade.set_editor_property(
        "inputs",
        [
            custom_input("BaseColor"),
            custom_input("PixelNormal"),
            custom_input("WorldPos"),
            custom_input("FaceContrast"),
            custom_input("ColorVariation"),
            custom_input("PixelSize"),
        ],
    )
    shade.set_editor_property(
        "output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3
    )
    shade.set_editor_property(
        "code",
        """
float3 N = normalize(PixelNormal);
float top = saturate(N.z);
float bottom = saturate(-N.z);
float2 sideNormal = normalize(N.xy + float2(0.0001, 0.0001));
float sideDirection = 0.5 + 0.5 * dot(
    sideNormal,
    normalize(float2(-0.64, 0.77)));
float sideLight = lerp(0.68, 0.86, sideDirection);
float faceLight = lerp(sideLight, 1.12, top);
faceLight = lerp(faceLight, 0.52, bottom);
faceLight = lerp(1.0, faceLight, saturate(FaceContrast));

float safePixelSize = max(PixelSize, 1.0);
float3 cell = floor(WorldPos / safePixelSize);
float hashValue = frac(
    sin(dot(cell, float3(12.9898, 78.233, 37.719))) * 43758.5453);
float variation = lerp(
    1.0 - ColorVariation,
    1.0 + ColorVariation,
    hashValue);

float3 shaded = saturate(BaseColor.rgb * faceLight * variation);
return floor(shaded * 31.0 + 0.5) / 31.0;
""".strip(),
    )

    connect(base_color, "", shade, "BaseColor")
    connect(normal, "", shade, "PixelNormal")
    connect(world_position, "", shade, "WorldPos")
    connect(face_contrast, "", shade, "FaceContrast")
    connect(color_variation, "", shade, "ColorVariation")
    connect(pixel_size, "", shade, "PixelSize")
    connect_property(shade, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = scalar_parameter(material, "Roughness", 0.88, -360, 330)
    specular = scalar_parameter(material, "Specular", 0.12, -360, 430)
    ambient_occlusion = scalar_parameter(
        material, "AmbientOcclusion", 0.88, -360, 530
    )
    shadow_lift = scalar_parameter(material, "ShadowLift", 0.16, -360, 630)
    emissive = expression(material, unreal.MaterialExpressionMultiply, 40, 500)
    connect(shade, "", emissive, "A")
    connect(shadow_lift, "", emissive, "B")
    connect_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    connect_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)
    connect_property(
        ambient_occlusion, "", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION
    )
    connect_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError("Material compile errors: " + " | ".join(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(
        material, only_if_is_dirty=False
    ):
        raise RuntimeError(f"Could not save {ASSET_PATH}")
    unreal.log(f"MatterFlux voxel material generated: {ASSET_PATH}")


def build_gas_material():
    material = create_material(GAS_ASSET_PATH, GAS_ASSET_NAME)
    material.set_editor_property(
        "blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT
    )
    material.set_editor_property("two_sided", True)
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT
    )
    unreal.MaterialEditingLibrary.set_base_material_usage(
        material,
        unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES,
        True,
    )

    color = vector_parameter(
        material, "Color", unreal.LinearColor(0.70, 0.84, 0.92, 1.0), -500, -80
    )
    opacity = scalar_parameter(material, "Opacity", 0.22, -500, 80)
    connect_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    connect_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError("Gas material compile errors: " + " | ".join(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(
        material, only_if_is_dirty=False
    ):
        raise RuntimeError(f"Could not save {GAS_ASSET_PATH}")
    unreal.log(f"MatterFlux voxel gas material generated: {GAS_ASSET_PATH}")


build_material()
build_gas_material()
