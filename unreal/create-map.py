import unreal
# Preserve existing maps and the 2D default. The launcher selects the 3D GameMode.
if not unreal.EditorAssetLibrary.does_asset_exist("/Game/Maps/Repoverse"):
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level("/Game/Maps/Repoverse")
    subsystem.save_current_level()
if not unreal.EditorAssetLibrary.does_asset_exist("/Game/Materials/M_Repoverse"):
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_Repoverse", "/Game/Materials", unreal.Material, unreal.MaterialFactoryNew())
    vertex = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionVertexColor)
    unreal.MaterialEditingLibrary.connect_material_property(vertex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(vertex, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
