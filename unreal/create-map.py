import unreal
# Run after compiling the plugin. Never replace an existing level.
if not unreal.EditorAssetLibrary.does_asset_exist("/Game/Maps/Repoverse"):
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    subsystem.new_level("/Game/Maps/Repoverse")
    subsystem.save_current_level()
