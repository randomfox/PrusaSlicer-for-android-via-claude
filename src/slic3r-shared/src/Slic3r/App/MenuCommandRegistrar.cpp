#include "Slic3r/App/MenuCommandRegistrar.hpp"

#include "Slic3r/Directories.hpp"
#include "Slic3r/Platform.hpp"
#include "Slic3r/Version.hpp"
#include "Slic3r/App/Render/Context.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <thread>
#include "Slic3r/App/MenuManager.hpp"
#include "Slic3r/App/UIItemCommand.hpp"
#include "Slic3r/App/Platform/CommandName.hpp"
#include "Slic3r/App/Platform/AbstractRenderModule.hpp"
#include "Slic3r/App/ResultExport/ExportActions.hpp"
#include "Slic3r/App/Scene/GeometryDataFactory.hpp"
#include "Slic3r/App/Scene/ISceneProvider.hpp"
#include "Slic3r/App/Scene/GizmoManager.hpp"
#include "Slic3r/App/Scene/EmbossCreate.hpp"
#include "Slic3r/App/Navigator.hpp"
#include "Slic3r/App/ThumbnailStore.hpp"
#include "Slic3r/App/AppServices.hpp"
#include "Slic3r/App/AppConfigInteractor.hpp"
#include "Slic3r/App/IDialogManager.hpp"
#include "Slic3r/App/Wildcards.hpp"
#include "Slic3r/App/AppConfig.hpp"
#include "Slic3r/App/Localization.hpp"
#include "Slic3r/App/OpenBrowser.hpp"
#include "Slic3r/App/Lua/ProjectApi.hpp"
#include "Slic3r/App/Platform/IFileExplorerHandler.hpp"
#include "Slic3r/App/ProjectSaver.hpp"
#include "Slic3r/App/Browser/BrowserLogicPluginMarketplace.hpp"

#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Biz/Scene/SceneInteractor.hpp"
#include "Slic3r/Biz/Scene/Selection.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Biz/Format/3mf.hpp"
#include "Slic3r/Biz/Algorithms/Point.hpp"
#include "Slic3r/Biz/FileLoadingLogic.hpp"
#include "Slic3r/Biz/OrientationOptimizer.hpp"
#include "Slic3r/Biz/ConfigSnapshots.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <ctime>
#if defined(__linux__) && !defined(__ANDROID__)
#include <boost/dll/runtime_symbol_info.hpp>
#include <cstdlib>
#endif
#include "Slic3r/Biz/MeshExportLogic.hpp"
#include "Slic3r/Biz/VolumeReloadLogic.hpp"
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"

#include "Slic3r/App/Plater/PlaterRenderModule.hpp"
#include "Slic3r/App/Preview/PreviewRenderModule.hpp"
#include "Slic3r/App/Plater/TextGizmo.hpp"

#include <optional>

namespace Slic3r::App {

// #define SHOW_NOT_IMPLEMENTED_ITEMS

using namespace Slic3r::Biz;
using CommandName = Platform::CommandName;

namespace {

/**
 * File types offered when importing geometry into the scene.
 */
constexpr Wildcards::TypeFlag import_file_types = Wildcards::TypeFlag::Project3mf
    | Wildcards::TypeFlag::Stl
    | Wildcards::TypeFlag::Obj
    | Wildcards::TypeFlag::Svg
    | Wildcards::TypeFlag::Step;

/**
 * File types offered when replacing or reloading a volume mesh.
 */
constexpr Wildcards::TypeFlag model_file_types = Wildcards::TypeFlag::Project3mf
    | Wildcards::TypeFlag::Stl
    | Wildcards::TypeFlag::Obj
    | Wildcards::TypeFlag::Step;

class CommandBuilder
{
public:
    CommandBuilder(MenuManager& menu_manager, Platform::AbstractRenderModule& render_module) :
        m_menu_manager(menu_manager),
        m_render_module(render_module)
    {}

    ~CommandBuilder()
    {
        register_commands();
    }

    CommandBuilder& push_path_level(MenuItemName name)
    {
        m_path.push_back(name);
        return *this;
    }

    CommandBuilder& pop_path_level()
    {
        ASSERT(!m_path.empty());
        m_path.pop_back();
        return *this;
    }

    CommandBuilder& append_item(
        MenuItemName key,
        const char* command_name,
        std::function<void()> action,
        UIItemCommandExtraOpts extra_opts = {}
    )
    {
        ASSERT(!m_path.empty());
        auto path_and_key = m_path;
        path_and_key.push_back(key);
        m_commands.emplace_back(
            CommandElement{path_and_key, command_name, std::move(action), extra_opts}
        );
        return *this;
    }

    CommandBuilder& append_item_from_command(MenuItemName key, const char* command_name)
    {
        ASSERT(!m_path.empty());
        auto path_and_key = m_path;
        path_and_key.push_back(key);
        m_commands.emplace_back(CommandElement{path_and_key, command_name, nullptr, {}});
        return *this;
    }

    CommandBuilder& append_separator()
    {
        ASSERT(!m_path.empty());
        m_commands.emplace_back(CommandElement{m_path, nullptr, []() {}, {}});
        return *this;
    }

    void register_commands_and_clear_cache()
    {
        register_commands();
        m_commands.clear();
    }

private:
    void register_commands() const
    {
        for (const auto& command : m_commands) {
            if (command.name) {
                if (command.action) {
                    m_menu_manager.register_menu_item(
                        command.path,
                        std::make_unique<UIItemCommand>(
                            command.name,
                            command.action,
                            command.extra_opts
                        )
                    );
                } else {
                    m_menu_manager.register_menu_item_from_command(
                        command.path,
                        m_render_module.command(command.name)

                    );
                }
            } else {
                m_menu_manager.register_menu_separator_item(command.path);
            }
        }
    }

private:
    struct CommandElement
    {
        std::vector<UniversalMenuItemName> path;
        const char* name; // nullptr means separator
        std::function<void()> action{nullptr}; // nullptr means use already registered command
        UIItemCommandExtraOpts extra_opts;
    };

    std::vector<UniversalMenuItemName> m_path;
    std::vector<CommandElement> m_commands;

    MenuManager& m_menu_manager;
    Platform::AbstractRenderModule& m_render_module;
};

} // namespace

MenuCommandRegistrar::MenuCommandRegistrar(
    Platform::AbstractRenderModule& render_module,
    Biz::ProjectInteractor& project_interactor,
    Navigator& navigator,
    ProjectSaver& project_saver
) :
    m_render_module(render_module),
    m_menu_manager(m_render_module.menu_manager()),
    m_project_interactor(project_interactor),
    m_navigator(navigator),
    m_project_saver(project_saver),
    m_clipboard_interactor(m_project_interactor.clipboard_interactor())
{}

void MenuCommandRegistrar::register_top_bar_menus(Lua::PluginSystem* plugin_system)
{
    register_undo_redo_commands();
    register_main_menu_commands(plugin_system);
    register_file_menu_commands();
}

void MenuCommandRegistrar::register_context_menus(
    Scene::GeometryDataFactory& data_factory,
    Scene::ISceneProvider* scene_provider
)
{
    m_data_factory   = &data_factory;
    m_scene_provider = scene_provider;

    register_bed_menu_commands();
    register_object_menu_commands();
    register_svg_or_text_volume_menu_commands();
    register_volume_menu_commands();
    register_multi_object_menu_commands();
}

void MenuCommandRegistrar::register_bed_menu_commands()
{
    register_bed_menu_add_shape_commands();

    auto any_selected_bed_has_object = [this]() -> bool
    {
        const Biz::Scene::BedSelection& selection{
            m_project_interactor.scene_interactor().bed_selection()
        };
        if (selection.empty()) {
            return false;
        }
        const Biz::Scene::BedInstances beds{
            m_project_interactor.scene_interactor().selected_bed_instances()
        };

        for (const auto& bed : beds) {
            if (!bed.get().model_instances.empty())
                return true;
        }
        return false;
    };

    CommandBuilder builder(m_menu_manager, m_render_module);
    builder.push_path_level(MenuItemName::BedContextMenu)
        .append_separator()
        .append_item(
            MenuItemName::ArrangeBed,
            CommandName::ArrangeBed,
            [this]
            {
                const auto it{m_render_module.gizmo_commands().find("arrange-gizmo-arrange-local")};
                if (it != m_render_module.gizmo_commands().end()) {
                    it->second->execute();
                }
            },
            UIItemCommandExtraOpts{
                .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                    Platform::KeyModifiers{},
                    Platform::KeyCode::D
                }},
                .enabled = [any_selected_bed_has_object]() { return any_selected_bed_has_object(); },
            }
        )
        .append_item(
            MenuItemName::ArrangeSelectionBed,
            CommandName::ArrangeSelectionBed,
            [this]
            {
                const auto it{m_render_module.gizmo_commands().find("arrange-gizmo-arrange-local-selection")};
                if (it != m_render_module.gizmo_commands().end()) {
                    it->second->execute();
                }
            },
            UIItemCommandExtraOpts{
                .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                    Platform::KeyModifiers(Platform::KeyModifier::Shift),
                    Platform::KeyCode::D
                }},
                .enabled = [this]() { return !m_project_interactor.scene_interactor().object_selection().empty(); },
            }
        )
        .append_item(
            MenuItemName::SelectAllOnBed,
            CommandName::SelectAllOnBed,
            [this]
            {
                const Biz::Scene::BedInstances beds{
                    m_project_interactor.scene_interactor().selected_bed_instances()};

                Biz::Scene::ObjectSelection new_selection;
                for (const auto& bed : beds) {
                    for (const auto& instance : bed.get().model_instances) {
                        new_selection.elements.push_back(
                            {instance->get_object()->id().id, instance->id().id}
                        );
                    }
                }
                m_project_interactor.scene_interactor().set_object_selection(new_selection);
            },
            UIItemCommandExtraOpts{
                .enabled = [any_selected_bed_has_object]() { return any_selected_bed_has_object(); }
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::DeleteBed,
            CommandName::DeleteBed,
            [this]
            {
                if (m_project_interactor.selected_config_container().bed_instances().size() > 1) {
                    m_project_interactor.scene_interactor().remove_bed_instance(
                        m_project_interactor.scene_interactor().bed_selection().last_selected_bed()
                    );
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::DeleteBed);
                } else {
                    m_project_interactor.remove_config_container(
                        m_project_interactor.selected_config_container_id()
                    );
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::DeleteConfigContainer);
                }
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                {
                    if (m_project_interactor.selected_config_container_id() == Domain::INVALID_ID) {
                        return false;
                    }
                    return
                        m_project_interactor.selected_project().config_containers().size() > 1
                     || m_project_interactor.selected_config_container().bed_instances().size() > 1;
                }
            }
        );
}

static std::string geometry_name(Scene::GeometryDataId geometry_id)
{
    switch (geometry_id) {
    case Scene::GeometryDataId::Cube:
        return _u8L("Cube");
    case Scene::GeometryDataId::Cylinder:
        return _u8L("Cylinder");
    case Scene::GeometryDataId::Sphere:
        return _u8L("Sphere");
    case Scene::GeometryDataId::Segment:
    case Scene::GeometryDataId::Cone:
    case Scene::GeometryDataId::Circle:
    case Scene::GeometryDataId::GradedCircle:
    case Scene::GeometryDataId::SmoothSphere:
    case Scene::GeometryDataId::ToolMarker:
    case Scene::GeometryDataId::CandyButton:
        break;
    }
    PANIC("Unsupported geometry");
    return "Undefined";
}

void MenuCommandRegistrar::register_bed_menu_add_shape_commands()
{
    auto add_object_shape =
        [this](Scene::GeometryDataId geometry_id, UndoSnapshotType snapshot_type)
    {
        const Domain::ElementRefs new_instances =
            m_project_interactor.scene_interactor().add_object_to_active_bed(
                m_data_factory->triangle_mesh(geometry_id)->triangles(),
                geometry_name(geometry_id)
            );

        const Domain::BedRef target_bed =
            m_project_interactor.scene_interactor().bed_selection().last_selected_bed();
        m_project_interactor.arrange_interactor().arrange_added_instances(
            m_project_interactor.selected_project_id(),
            new_instances,
            target_bed,
            snapshot_type
        );
    };

    CommandBuilder builder(m_menu_manager, m_render_module);
    builder.push_path_level(MenuItemName::BedContextMenu)
        .push_path_level(MenuItemName::AddObjectShape)
        .append_item_from_command(MenuItemName::ObjectShapeLoad, CommandName::ImportGeometry)
        .append_separator()
        .append_item(
            MenuItemName::ObjectShapeCube,
            "add-object-shape-cube",
            [this, add_object_shape]()
            {
                add_object_shape(Scene::GeometryDataId::Cube, UndoSnapshotType::AddCube);
            }
        )
        .append_item(
            MenuItemName::ObjectShapeCylinder,
            "add-object-shape-cylinder",
            [this, add_object_shape]()
            {
                add_object_shape(Scene::GeometryDataId::Cylinder, UndoSnapshotType::AddCylinder);
            }
        )
        .append_item(
            MenuItemName::ObjectShapeSphere,
            "add-object-shape-sphere",
            [this, add_object_shape]()
            {
                add_object_shape(Scene::GeometryDataId::Sphere, UndoSnapshotType::AddSphere);
            }
        )
        .append_separator()
        .append_item_from_command(MenuItemName::ObjectShapeText, CommandName::CreateObjectAsText)
        .append_item(
            MenuItemName::ObjectShapeSvg,
            "add-object-shape-svg",
            [this]() { load_object(Wildcards::TypeFlag::Svg); },
            UIItemCommandExtraOpts{
                .keyboard_shortcuts =
                    Platform::KeyboardShortcuts{
                        Platform::KeyboardShortcut{0, Platform::KeyCode::G}
                    },
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().object_selection().empty(); }
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::ObjectShapeFromGallery,
            "add-object-shape-gallery",
            [this]()
            {
                // The undo snapshot is taken inside load_models_to_project().
                load_shape_from_gallery();
            }
        );
}

void MenuCommandRegistrar::register_object_menu_commands()
{
    CommandBuilder builder(m_menu_manager, m_render_module);

    builder.push_path_level(MenuItemName::ObjectContextMenu)
        .append_item(
            MenuItemName::CopyObject,
            CommandName::CopyModelItems,
            [this]() { m_clipboard_interactor.copy(m_project_interactor.selected_project_id()); },
            UIItemCommandExtraOpts{
                .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                    Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                    Platform::KeyCode::C
                }},
                .enabled            = [this]() { return m_clipboard_interactor.can_copy(); }
            }
        )
        .append_item(
            MenuItemName::PasteObject,
            CommandName::PasteModelItems,
            [this]() { m_clipboard_interactor.paste(m_project_interactor.selected_project_id()); },
            UIItemCommandExtraOpts{
                .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                    Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                    Platform::KeyCode::V
                }},
                .enabled            = [this]() { return m_clipboard_interactor.can_paste(); }
            }
        )
        .append_item_from_command(MenuItemName::DeleteSelectedObject, CommandName::DeleteSelected)
        .append_item_from_command(MenuItemName::RenameObject, CommandName::RenameSelected)
        .append_separator()
        .append_item(
            MenuItemName::SetNumberOfInstances,
            CommandName::SetNumberOfInstances,
            [this]()
            {
                size_t init_cnt{1};
                const auto& selection = m_project_interactor.scene_interactor().object_selection();
                if (selection.only_single_object()) {
                    Domain::ModelObject* object =
                        m_project_interactor.selected_project().find_object_by_id(
                            selection.elements.front().object_id
                        );
                    init_cnt = object->instances.size();
                }

                m_navigator.input_number(
                    Biz::_u8L("Enter the number of copies"),
                    std::string{},
                    Biz::_u8L("Copies of the selected object"),
                    init_cnt,
                    1,
                    1000,
                    [this](int number)
                    {
                        ASSERT(number > 0);
                        const Domain::ElementRefs new_instances =
                            m_project_interactor.scene_interactor()
                                .set_selected_objects_instance_count(number);
                        const Domain::BedRef target_bed = m_project_interactor.scene_interactor()
                                                              .bed_selection()
                                                              .last_selected_bed();

                        m_project_interactor.arrange_interactor().arrange_added_instances(
                            m_project_interactor.selected_project_id(),
                            new_instances,
                            target_bed,
                            UndoSnapshotType::SetNumberOfInstances
                        );
                    },
                    [] {}
                );
            },
            UIItemCommandExtraOpts{
                .enabled =
                    [this]()
                {
                    const auto& selection =
                        m_project_interactor.scene_interactor().object_selection();
                    return !selection.empty()
                        && selection.mode == Biz::Scene::SelectionMode::Instance;
                }
            }
        )
        .append_item(
            MenuItemName::FillBedWithInstances,
            "fill-bed-with-instances",
            [this]()
            {
                const auto& selection = m_project_interactor.scene_interactor().object_selection();
                if (!selection.only_single_object()) {
                    return;
                }
                m_project_interactor.arrange_interactor().fill_bed(
                    m_project_interactor.selected_project_id(),
                    selection.elements.front().object_id,
                    m_project_interactor.scene_interactor().bed_selection().last_selected_bed()
                );
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                {
                    const auto& selection = m_project_interactor.scene_interactor().object_selection();
                    return selection.only_single_object() && selection.mode == Biz::Scene::SelectionMode::Instance;
                }
            }
        )
        .append_separator()
        // we need to force a registration of added menu items before call extra function
        .register_commands_and_clear_cache();

    register_object_menu_add_volume_commands();

    builder.append_separator()
        .append_item(
            MenuItemName::SetAsSeparateObject,
            "set-as-separate-object",
            [this]()
            {
                m_project_interactor.scene_interactor().extract_selected_instances();
                m_project_interactor.undo_provider().take_snapshot(
                    Biz::UndoSnapshotType::SetAsSeparateObject
                );
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_extract_selected_instances(); }
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::ExportObject,
            CommandName::ExportAsStl,
            [this]() { this->export_selection_as_stl_obj(); },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_export_selection_as_mesh(); }
            }
        )
        .append_item(
            MenuItemName::ReplaceObject,
            CommandName::ReplaceWithStl,
            [this]() { replace_selected_volume_with_stl(); },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_replace_selected_volume(); }
            }
        )
        .append_item(
            MenuItemName::ReloadObject,
            CommandName::ReloadFromDisk,
            [this]() { reload_selection_from_disk(); },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_reload_selection_from_disk(); }
            }
        )
        .append_separator()
        .push_path_level(MenuItemName::SplitObject)
        .append_item(
            MenuItemName::SplitObjectToObjects,
            "split-object-to-objects",
            [this]()
            {
                m_project_interactor.scene_interactor().split_selection_to_objects();
                m_project_interactor.undo_provider().take_snapshot(
                    Biz::UndoSnapshotType::SplitToObjects
                );
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_split_selection_to_objects(); }
            }
        )
        .append_item(
            MenuItemName::SplitObjectToVolumes,
            CommandName::SplitToVolumes,
            [this]()
            {
                m_project_interactor.scene_interactor().split_selection_to_volumes();
                m_project_interactor.undo_provider().take_snapshot(
                    Biz::UndoSnapshotType::SplitToVolumes
                );
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_split_selection_to_volumes(); }
            }
        )
        .pop_path_level()
        .append_item(
            MenuItemName::ScaleToPrintVolume,
            "scale-to-print-volume",
            [this]() { scale_selection_to_print_volume(); },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return !m_project_interactor.scene_interactor().object_selection().empty(); }
            }
        )
        .append_item(
            MenuItemName::OptimizeOrientation,
            "optimize-orientation",
            [this]() { optimize_orientation(); },
            UIItemCommandExtraOpts{.enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }}
        )
        .append_item(
            MenuItemName::ObjectInfo,
            "object-info",
            [this]() { show_object_info(); },
            UIItemCommandExtraOpts{.enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }}
        )
        .push_path_level(MenuItemName::MirrorObject)
        .append_item(
            MenuItemName::MirrorObjectX,
            "mirror-x",
            [this]() { mirror_selection(0); },
            UIItemCommandExtraOpts{.enabled = [this]() { return !m_project_interactor.scene_interactor().object_selection().empty(); }}
        )
        .append_item(
            MenuItemName::MirrorObjectY,
            "mirror-y",
            [this]() { mirror_selection(1); },
            UIItemCommandExtraOpts{.enabled = [this]() { return !m_project_interactor.scene_interactor().object_selection().empty(); }}
        )
        .append_item(
            MenuItemName::MirrorObjectZ,
            "mirror-z",
            [this]() { mirror_selection(2); },
            UIItemCommandExtraOpts{.enabled = [this]() { return !m_project_interactor.scene_interactor().object_selection().empty(); }}
        )
        .pop_path_level()
#if SLIC3R_ENABLE_WIN10_MESH_REPAIR
        .append_item(
            MenuItemName::FixObjectWithRepairAlgorithm,
            CommandName::FixWithRepairAlgorithm,
            [this]() { m_project_interactor.scene_interactor().repair_selected_object(); },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_repair_selected_object(); }
            }
        )
#endif
        .append_separator()
        .append_item(
            MenuItemName::InvalidateCutInfo,
            "invalidate-cut-info",
            [this]()
            {
                m_project_interactor.scene_interactor().invalidate_cut_info();
                m_project_interactor.undo_provider().take_snapshot(
                    Biz::UndoSnapshotType::InvalidateCutInfo
                );
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().can_invalidate_cut_info(); }
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::PrintableObject,
            CommandName::SetAsPrintable,
            [this]()
            {
                if (const UIItemCommand* ui_command = dynamic_cast<const UIItemCommand*>(
                        &m_render_module.command(CommandName::SetAsPrintable)
                    ))
                {
                    m_project_interactor.scene_interactor().set_selected_instances_printable(
                        !ui_command->checked()
                    );
                    m_project_interactor.undo_provider().take_snapshot(
                        Biz::UndoSnapshotType::SetAsPrintable
                    );
                }
            },
            UIItemCommandExtraOpts{
                .checked = [this]()
                { return m_project_interactor.scene_interactor().selected_instances_printable(); },
            }
        );
}

void MenuCommandRegistrar::register_object_menu_add_volume_commands()
{
    using VolumeType = Domain::ModelVolumeType;
    using GeometryId = Scene::GeometryDataId;

    auto add_volume_shape = [this](GeometryId geometry_id, VolumeType volume_type)
    {
        m_project_interactor.scene_interactor().add_volume_to_active_object(
            m_data_factory->triangle_mesh(geometry_id)->triangles(),
            volume_type,
            geometry_name(geometry_id)
        );
    };

    auto add_volume_text = [this](VolumeType volume_type)
    {
        Plater::TextGizmo* text_gizmo = dynamic_cast<Plater::TextGizmo*>(
            m_render_module.tool_gizmo(Scene::ToolType::TextGizmo, Domain::PrinterTechnology::FFF)
        );
        ASSERT(text_gizmo);
        text_gizmo->set_next_volume_type(volume_type);
        m_render_module.command(CommandName::TextGizmo).execute();
    };

    CommandBuilder builder(m_menu_manager, m_render_module);

    builder.push_path_level(MenuItemName::ObjectContextMenu)
        .push_path_level(MenuItemName::AddVolume)
        .push_path_level(MenuItemName::SolidPartVolume)
        .append_item(
            MenuItemName::SolidPartVolumeShapeLoad,
            "add-volume-shape-load-solid",
            [this]()
            {
                load_volume(VolumeType::MODEL_PART);
                m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddVolume);
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SolidPartVolumeShapeCube,
            "add-volume-shape-cube-solid",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cube, VolumeType::MODEL_PART);
                m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddVolumeCube);
            }
        )
        .append_item(
            MenuItemName::SolidPartVolumeShapeCylinder,
            "add-volume-shape-cylinder-solid",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cylinder, VolumeType::MODEL_PART);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddVolumeCylinder
                );
            }
        )
        .append_item(
            MenuItemName::SolidPartVolumeShapeSphere,
            "add-volume-shape-sphere-solid",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Sphere, VolumeType::MODEL_PART);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddVolumeSphere
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SolidPartVolumeShapeText,
            "add-volume-shape-text-solid",
            [this, add_volume_text]()
            {
                add_volume_text(VolumeType::MODEL_PART);
                m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddVolumeText);
            }
        )
        .append_item(
            MenuItemName::SolidPartVolumeShapeSVG,
            "add-volume-shape-svg-solid",
            [this]()
            {
                load_volume(VolumeType::MODEL_PART, Wildcards::TypeFlag::Svg);
                m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddVolumeSvg);
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SolidPartVolumeShapeFromGallery,
            "add-volume-shape-from-gallery-solid",
            [this]()
            {
                load_shape_from_gallery(VolumeType::MODEL_PART, [this]() {
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddVolumeGallery);
                });
            },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }
            }
        )
        .pop_path_level()
        .push_path_level(MenuItemName::NegativeVolume)
        .append_item(
            MenuItemName::NegativeVolumeShapeLoad,
            "add-volume-shape-load-negative",
            [this]()
            {
                load_volume(VolumeType::NEGATIVE_VOLUME);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolume
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::NegativeVolumeShapeCube,
            "add-volume-shape-cube-negative",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cube, VolumeType::NEGATIVE_VOLUME);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolumeCube
                );
            }
        )
        .append_item(
            MenuItemName::NegativeVolumeShapeCylinder,
            "add-volume-shape-cylinder-negative",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cylinder, VolumeType::NEGATIVE_VOLUME);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolumeCylinder
                );
            }
        )
        .append_item(
            MenuItemName::NegativeVolumeShapeSphere,
            "add-volume-shape-sphere-negative",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Sphere, VolumeType::NEGATIVE_VOLUME);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolumeSphere
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::NegativeVolumeShapeText,
            "add-volume-shape-text-negative",
            [this, add_volume_text]()
            {
                add_volume_text(VolumeType::NEGATIVE_VOLUME);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolumeText
                );
            }
        )
        .append_item(
            MenuItemName::NegativeVolumeShapeSVG,
            "add-volume-shape-svg-negative",
            [this]()
            {
                load_volume(VolumeType::NEGATIVE_VOLUME, Wildcards::TypeFlag::Svg);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddNegativeVolumeSvg
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::NegativeVolumeShapeFromGallery,
            "add-volume-shape-from-gallery-negative",
            [this]()
            {
                load_shape_from_gallery(VolumeType::NEGATIVE_VOLUME, [this]() {
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddNegativeVolumeGallery);
                });
            },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }
            }
        )
        .pop_path_level()
        .push_path_level(MenuItemName::ModifierVolume)
        .append_item(
            MenuItemName::ModifierVolumeShapeLoad,
            "add-volume-shape-load-modifier",
            [this]()
            {
                load_volume(VolumeType::PARAMETER_MODIFIER);

                m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddModifier);
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::ModifierVolumeShapeCube,
            "add-volume-shape-cube-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cube, VolumeType::PARAMETER_MODIFIER);

                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddModifierCube
                );
            }
        )
        .append_item(
            MenuItemName::ModifierVolumeShapeCylinder,
            "add-volume-shape-cylinder-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cylinder, VolumeType::PARAMETER_MODIFIER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddModifierCylinder
                );
            }
        )
        .append_item(
            MenuItemName::ModifierVolumeShapeSphere,
            "add-volume-shape-sphere-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Sphere, VolumeType::PARAMETER_MODIFIER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddModifierSphere
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::ModifierVolumeShapeText,
            "add-volume-shape-text-modifier",
            [this, add_volume_text]()
            {
                add_volume_text(VolumeType::PARAMETER_MODIFIER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddModifierText
                );
            }
        )
        .append_item(
            MenuItemName::ModifierVolumeShapeSVG,
            "add-volume-shape-svg-modifier",
            [this]()
            {
                load_volume(VolumeType::PARAMETER_MODIFIER, Wildcards::TypeFlag::Svg);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddModifierSvg
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::ModifierVolumeShapeFromGallery,
            "add-volume-shape-from-gallery-modifier",
            [this]()
            {
                load_shape_from_gallery(VolumeType::PARAMETER_MODIFIER, [this]() {
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddModifierGallery);
                });
            },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }
            }
        )
        .pop_path_level()
        .push_path_level(MenuItemName::SupportBlocker)
        .append_item(
            MenuItemName::SupportBlockerShapeLoad,
            "add-volume-shape-load-support-blocker",
            [this]()
            {
                load_volume(VolumeType::SUPPORT_BLOCKER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportBlocker
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SupportBlockerShapeCube,
            "add-volume-shape-cube-support-blocker",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cube, VolumeType::SUPPORT_BLOCKER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportBlockerCube
                );
            }
        )
        .append_item(
            MenuItemName::SupportBlockerShapeCylinder,
            "add-volume-shape-cylinder-support-blocker",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cylinder, VolumeType::SUPPORT_BLOCKER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportBlockerCube
                );
            }
        )
        .append_item(
            MenuItemName::SupportBlockerShapeSphere,
            "add-volume-shape-sphere-support-blocker",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Sphere, VolumeType::SUPPORT_BLOCKER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportBlockerSphere
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SupportBlockerShapeFromGallery,
            "add-volume-shape-from-gallery-support-blocker",
            [this]()
            {
                load_shape_from_gallery(VolumeType::SUPPORT_BLOCKER, [this]() {
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddSupportBlockerGallery);
                });
            },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }
            }
        )
        .pop_path_level()
        .push_path_level(MenuItemName::SupportModifier)
        .append_item(
            MenuItemName::SupportModifierShapeLoad,
            "add-volume-shape-load-support-modifier",
            [this]()
            {
                load_volume(VolumeType::SUPPORT_ENFORCER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportModifier
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SupportModifierShapeCube,
            "add-volume-shape-cube-support-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cube, VolumeType::SUPPORT_ENFORCER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportModifierCube
                );
            }
        )
        .append_item(
            MenuItemName::SupportModifierShapeCylinder,
            "add-volume-shape-cylinder-support-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Cylinder, VolumeType::SUPPORT_ENFORCER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportModifierCylinder
                );
            }
        )
        .append_item(
            MenuItemName::SupportModifierShapeSphere,
            "add-volume-shape-sphere-support-modifier",
            [this, add_volume_shape]()
            {
                add_volume_shape(GeometryId::Sphere, VolumeType::SUPPORT_ENFORCER);
                m_project_interactor.undo_provider().take_snapshot(
                    UndoSnapshotType::AddSupportModifierSphere
                );
            }
        )
        .append_separator()
        .append_item(
            MenuItemName::SupportModifierShapeFromGallery,
            "add-volume-shape-from-gallery-support-modifier",
            [this]()
            {
                load_shape_from_gallery(VolumeType::SUPPORT_ENFORCER, [this]() {
                    m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::AddSupportModifierGallery);
                });
            },
            UIItemCommandExtraOpts{
                .enabled = [this]() { return m_project_interactor.scene_interactor().object_selection().only_single_object(); }
            }
        );
}

void MenuCommandRegistrar::register_svg_or_text_volume_menu_commands()
{
    CommandBuilder builder(m_menu_manager, m_render_module);
    builder.push_path_level(MenuItemName::SvgOrTextContextMenu)
        .append_item(
            MenuItemName::EditSvgOrText,
            "edit-svg-or-text",
            [this]()
            {
                // Opens the tool that created the volume (it edits the selected one).
                const auto& selection = m_project_interactor.scene_interactor().object_selection();
                if (selection.elements.size() != 1) {
                    return;
                }
                const Domain::ElementRef& ref = selection.elements.front();
                const Domain::ModelVolume* volume =
                    m_project_interactor.selected_project().find_volume_by_id(ref.object_id, ref.volume_id);
                if (volume == nullptr) {
                    return;
                }
                m_render_module
                    .command(volume->is_svg() ? Platform::CommandName::SvgGizmo : Platform::CommandName::TextGizmo)
                    .execute();
            },
            UIItemCommandExtraOpts{
                .enabled = [this]()
                { return m_project_interactor.scene_interactor().object_selection().elements.size() == 1; }
            }
        )
        .append_item_from_command(
            MenuItemName::DeleteSelectedSvgOrText,
            CommandName::DeleteSelected
        );
}

void MenuCommandRegistrar::register_volume_menu_commands()
{
    CommandBuilder builder(m_menu_manager, m_render_module);

    builder.push_path_level(MenuItemName::VolumeContextMenu)
        .append_item_from_command(MenuItemName::DeleteSelectedVolume, CommandName::DeleteSelected)
        .append_item_from_command(MenuItemName::RenameVolume, CommandName::RenameSelected)
        .append_item_from_command(MenuItemName::CopyVolume, CommandName::CopyModelItems)
        .append_separator()
        .append_item_from_command(MenuItemName::ExportVolume, CommandName::ExportAsStl)
        .append_item_from_command(MenuItemName::ReplaceVolume, CommandName::ReplaceWithStl)
        .append_item_from_command(MenuItemName::ReloadVolume, CommandName::ReloadFromDisk)
        .append_separator()
        .append_item_from_command(MenuItemName::SplitVolume, CommandName::SplitToVolumes)
#if SLIC3R_ENABLE_WIN10_MESH_REPAIR
        .append_item_from_command(
            MenuItemName::FixVolumeWithRepairAlgorithm,
            CommandName::FixWithRepairAlgorithm
        )
#endif
    ;
}

void MenuCommandRegistrar::register_multi_object_menu_commands()
{
    CommandBuilder builder(m_menu_manager, m_render_module);
    builder.push_path_level(MenuItemName::MultiObjectsContextMenu)
        .append_item_from_command(MenuItemName::CopyMultiObjects, CommandName::CopyModelItems)
        .append_item_from_command(
            MenuItemName::DeleteSelectedMultiObjects,
            CommandName::DeleteSelected
        )
        .append_separator()
        .append_item_from_command(
            MenuItemName::SetMultiObjectsNumberOfInstances,
            CommandName::SetNumberOfInstances
        )
        .append_separator()
#if SLIC3R_ENABLE_WIN10_MESH_REPAIR
        .append_item_from_command(
            MenuItemName::FixMultiObjectWithRepairAlgorithm,
            CommandName::FixWithRepairAlgorithm
        )
#endif
        .append_item(
            MenuItemName::MergeMultiObjects,
            "merge-multi-objects",
            [this]()
            {
                m_project_interactor.scene_interactor().merge_selection_into_object();
                m_project_interactor.undo_provider().take_snapshot(
                    Biz::UndoSnapshotType::MergeToOneObject
                );
            },
            UIItemCommandExtraOpts{
                .enabled =
                    [this]()
                {
                    return m_project_interactor.scene_interactor()
                        .can_merge_selection_into_object();
                }
            }
        )
        .append_separator()
        .append_item_from_command(MenuItemName::PrintableMultiObjects, CommandName::SetAsPrintable);
}

boost::filesystem::path MenuCommandRegistrar::default_dialog_folder() const
{
    return m_project_interactor.project_dir(
        m_project_interactor.selected_project_id(),
        AppServices::instance().app_config().get<std::string>("last_used_directory")
    );
}

void MenuCommandRegistrar::load_project()
{
    IDialogManager::FileCallback callback =
        [this](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (success) {
            m_project_interactor.file_opening_interactor().open({file_paths.front()});
        }
    };

    auto& dlg_manager = AppServices::instance().dialog_manager();
    dlg_manager.show_file_dialog(
        FileDialogType::Open,
        _u8L("Open project"),
        this->default_dialog_folder(),
        "",
        Wildcards::generate_wildcards(Wildcards::TypeFlag::Project3mf),
        callback
    );
}

void MenuCommandRegistrar::install_plugin(Lua::PluginSystem& plugin_system)
{
    IDialogManager::FileCallback callback =
        [&plugin_system](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (success) {
            // as on Win/Linux this callback is triggered within render loop showing menu items,
            // we are going to recreate, we need to dispatch this action outside render loop
            // to prevent rendering disposed menu items
            Biz::Platform::PlatformServices::instance()
                .main_thread_dispatcher()
                .dispatch_on_main_thread([&plugin_system, file_path = file_paths.front().string()]
                                         { plugin_system.install_file(file_path); });
        }
    };

    auto& dlg_manager = AppServices::instance().dialog_manager();
    dlg_manager.show_file_dialog(
        FileDialogType::Open,
        _u8L("Install plugin"),
        m_project_interactor.project_dir(
            m_project_interactor.selected_project_id(),
            AppServices::instance().app_config().get<std::string>("last_used_directory")
        ),
        "",
        Wildcards::generate_wildcards(Wildcards::TypeFlag::Zip),
        callback
    );

}

void MenuCommandRegistrar::load_object(Wildcards::TypeFlag specific_type)
{
    IDialogManager::FileCallback callback =
        [this](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (success) {
            m_project_interactor.file_opening_interactor().open(file_paths, {.as_geometry = true});
            if (m_render_module.has_command(CommandName::SvgGizmo)
                && m_render_module.command(CommandName::SvgGizmo).enabled())
            {
                // open SvgGizmo, when svg volume is selected(was imported)
                m_render_module.command(CommandName::SvgGizmo).execute();
            }
            m_navigator.navigate_to_module_type(App::Render::ModuleType::Plater);
        }
    };

    auto& dlg_manager = App::AppServices::instance().dialog_manager();
    dlg_manager.show_file_dialog(
        FileDialogType::OpenMultiple,
        _u8L("Import file"),
        this->default_dialog_folder(),
        "",
        specific_type == Wildcards::TypeFlag::None ? Wildcards::generate_wildcards(
                                                         import_file_types,
                                                         Wildcards::TypeFlag::AllImportFiles
                                                     ) :
                                                     Wildcards::generate_wildcards(specific_type),
        callback
    );
}

void MenuCommandRegistrar::load_archive()
{
    IDialogManager::FileCallback callback =
        [this](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (success) {
            m_project_interactor.file_opening_interactor().open(file_paths);
            m_navigator.navigate_to_module_type(App::Render::ModuleType::Plater);
        }
    };

    auto& dlg_manager = App::AppServices::instance().dialog_manager();
    dlg_manager.show_file_dialog(
        FileDialogType::OpenMultiple,
        _u8L("Import ZIP archive"),
        this->default_dialog_folder(),
        "",
        Wildcards::generate_wildcards(Wildcards::TypeFlag::Zip),
        callback
    );
}

namespace {
// open SvgGizmo, when svg volume is just added into scene
// TODO: solve multiple svgs at once
std::optional<Scene::TrafoGuess> get_svg_guess_tr(
    const std::vector<boost::filesystem::path>& file_paths,
    const Biz::ProjectInteractor& project_interactor,
    const Scene::ISceneProvider& scene_provider
)
{
    const Biz::Scene::ObjectSelection& selection =
        project_interactor.scene_interactor().object_selection();
    const Domain::Project& project = project_interactor.selected_project();
    const Scene::Scene& scene      = scene_provider.scene();
    return Scene::guess_volume_transformation(selection.elements, project, scene);
}

static void open_svg_gizmo(
    const std::optional<Scene::TrafoGuess>& volume_tr,
    Biz::ProjectInteractor& project_interactor,
    Platform::AbstractRenderModule& render_module
)
{
    const Biz::Scene::ObjectSelection& selection =
        project_interactor.scene_interactor().object_selection();
    if (selection.elements.size() != 1)
        return;

    const Domain::ElementRef& element = selection.elements.front();
    const Domain::Project& project    = project_interactor.selected_project();
    const Domain::ModelVolume* volume = nullptr;
    if (element.has_volume()) {
        volume = project.find_volume_by_id(element.object_id, element.volume_id);
    } else {
        const Domain::ModelObject* object = project.find_object_by_id(element.object_id);
        if (object != nullptr && object->volumes.size() == 1)
            volume = object->volumes.front();
    }
    if (volume == nullptr)
        return;

    const Domain::ModelInstance* instance =
        project.find_instance_by_id(element.object_id, element.instance_id);
    if (instance == nullptr || instance != volume_tr->instance)
        return;
    const Domain::Transform3d& instance_tr = instance->get_matrix();
    Domain::Transform3d world_relative     = instance_tr
        * volume->get_matrix().inverse()
        * volume_tr->transformation
        * instance_tr.inverse();
    project_interactor.scene_interactor().transform_selection(world_relative.matrix());

    if (render_module.command(CommandName::SvgGizmo).enabled()) {
        render_module.command(CommandName::SvgGizmo).execute();
    }
}
} // namespace

void
MenuCommandRegistrar::load_volume(Domain::ModelVolumeType type, Wildcards::TypeFlag specific_type)
{
    IDialogManager::FileCallback callback =
        [this, type](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (!success || file_paths.empty())
            return;

        bool svg_loading = file_paths.size() == 1 && file_paths.back().extension() == ".svg";

        // NOTE: Need to ray cast into scene before import
        auto svg_transform = svg_loading ?
            get_svg_guess_tr(file_paths, m_project_interactor, *m_scene_provider) :
            std::nullopt;
        Biz::FileLoadingLogic::import_volumes_into_selected_object(
            file_paths,
            type,
            m_project_interactor.scene_interactor(),
            &AppServices::instance().dialog_manager()
        );

        // open SvgGizmo, when svg volume is selected(was imported)
        if (svg_loading) {
            open_svg_gizmo(svg_transform, m_project_interactor, m_render_module);
        }
    };

    auto& dlg_manager = AppServices::instance().dialog_manager();
    dlg_manager.show_file_dialog(
        FileDialogType::OpenMultiple,
        _u8L("Import file"),
        this->default_dialog_folder(),
        "",
        specific_type == Wildcards::TypeFlag::None ? Wildcards::generate_wildcards(
                                                         import_file_types,
                                                         Wildcards::TypeFlag::AllImportFiles
                                                     ) :
                                                     Wildcards::generate_wildcards(specific_type),
        callback
    );
}

namespace {
boost::filesystem::path user_shapes_dir() { return boost::filesystem::path(Slic3r::data_dir()) / "shapes"; }

bool is_gallery_shape(const boost::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".stl" || ext == ".obj" || ext == ".3mf" || ext == ".step" || ext == ".stp";
}

std::vector<boost::filesystem::path> gallery_shapes()
{
    std::vector<boost::filesystem::path> shapes;
    for (const boost::filesystem::path& dir :
         {boost::filesystem::path(Slic3r::resources_dir()) / "shapes", user_shapes_dir()}) {
        boost::system::error_code ec;
        std::vector<boost::filesystem::path> in_dir;
        for (boost::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (boost::filesystem::is_regular_file(it->path(), ec) && is_gallery_shape(it->path())) {
                in_dir.push_back(it->path());
            }
        }
        std::sort(in_dir.begin(), in_dir.end());
        shapes.insert(shapes.end(), in_dir.begin(), in_dir.end());
    }
    return shapes;
}
} // namespace

void MenuCommandRegistrar::add_gallery_shape(
    const boost::filesystem::path& path, std::optional<Domain::ModelVolumeType> type
)
{
    if (type) {
        Biz::FileLoadingLogic::import_volumes_into_selected_object(
            {path}, *type, m_project_interactor.scene_interactor(), &AppServices::instance().dialog_manager()
        );
    } else {
        m_project_interactor.load_models_to_project({path}, false);
    }
}

void MenuCommandRegistrar::load_shape_from_gallery(std::optional<Domain::ModelVolumeType> type, std::function<void()> on_loaded)
{
    IDialogManager& dialogs = AppServices::instance().dialog_manager();
    const std::vector<boost::filesystem::path> shapes = gallery_shapes();
    const std::string add_new = _u8L("Add shape from file") + "...";
    std::vector<std::string> names{add_new};
    for (const boost::filesystem::path& shape : shapes) {
        names.push_back(shape.stem().string());
    }
    const std::string chosen = dialogs.show_combo_dialog(
        _u8L("Shape Gallery"),
        shapes.empty() ? _u8L("The gallery is empty. Add a shape (STL, OBJ, 3MF or STEP) to it:") :
                         _u8L("Choose a shape:"),
        names
    );
    if (chosen.empty()) {
        return;
    }
    if (chosen != add_new) {
        for (const boost::filesystem::path& shape : shapes) {
            if (shape.stem().string() == chosen) {
                add_gallery_shape(shape, type);
                if (on_loaded) {
                    on_loaded();
                }
                return;
            }
        }
        return;
    }
    // Copy the chosen files into the user's gallery and add the first one.
    IDialogManager::FileCallback callback =
        [this, type, on_loaded](bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (!success || file_paths.empty()) {
            return;
        }
        boost::system::error_code ec;
        boost::filesystem::create_directories(user_shapes_dir(), ec);
        std::optional<boost::filesystem::path> first;
        for (const boost::filesystem::path& source : file_paths) {
            if (!is_gallery_shape(source)) {
                continue;
            }
            const boost::filesystem::path target = user_shapes_dir() / source.filename();
            boost::filesystem::copy_file(source, target, boost::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                AppServices::instance().dialog_manager().show_error_dialog(
                    fmt::format("{}\n{}", _u8L("Failed to add the shape to the gallery."), ec.message()),
                    _u8L("Shape Gallery")
                );
                return;
            }
            if (!first) {
                first = target;
            }
        }
        if (first) {
            add_gallery_shape(*first, type);
            if (on_loaded) {
                on_loaded();
            }
        }
    };
    dialogs.show_file_dialog(
        FileDialogType::OpenMultiple,
        _u8L("Add shape to the gallery"),
        this->default_dialog_folder(),
        "",
        Wildcards::generate_wildcards(
            Wildcards::TypeFlag::Stl | Wildcards::TypeFlag::Obj | Wildcards::TypeFlag::Project3mf | Wildcards::TypeFlag::Step,
            Wildcards::TypeFlag::Stl
        ),
        callback
    );
}

void MenuCommandRegistrar::show_about()
{
    const std::string text = fmt::format(
        "{} {}\n\n{}\n\n{}\n\n{}",
        SLIC3R_APP_NAME, SLIC3R_VERSION,
        _u8L("PrusaSlicer is based on Slic3r by Alessandro Ranellucci and the RepRap community. "
             "Developed by Prusa Research."),
        _u8L("Licensed under GNU AGPLv3. Distributed in the hope that it will be useful, but "
             "WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS "
             "FOR A PARTICULAR PURPOSE."),
        "https://www.prusa3d.com/prusaslicer/"
    );
    AppServices::instance().dialog_manager().show_info_dialog(text, fmt::format(fmt::runtime(_u8L("About {}")), SLIC3R_APP_NAME));
}

void MenuCommandRegistrar::show_system_info()
{
    const Render::Context& gl = Render::Context::instance();
    const std::string text = fmt::format(
        "{} {}\n{}: {}\n{}: {}\n\nOpenGL\n{}: {}\n{}: {}\n{}: {}\nGLSL: {}\n\n{}: {}\n{}: {}",
        SLIC3R_APP_NAME, SLIC3R_VERSION,
        _u8L("Platform"), platform_to_string(platform()),
        _u8L("CPU threads"), std::thread::hardware_concurrency(),
        _u8L("Vendor"), gl.gl_vendor_string(),
        _u8L("Renderer"), gl.gl_renderer_string(),
        _u8L("Version"), gl.gl_version_string(),
        gl.glsl_version_string(),
        _u8L("Data directory"), Slic3r::data_dir(),
        _u8L("Resources directory"), Slic3r::resources_dir()
    );
    AppServices::instance().dialog_manager().show_info_dialog(text, _u8L("System Information"));
}

void MenuCommandRegistrar::optimize_orientation()
{
    auto& scene           = m_project_interactor.scene_interactor();
    const auto& selection = scene.object_selection();
    if (!selection.only_single_object()) {
        return;
    }
    const Domain::ElementRef& element = selection.elements.front();
    const Domain::Project& project    = m_project_interactor.selected_project();
    const Domain::ModelObject* object = project.find_object_by_id(element.object_id);
    const Domain::ModelInstance* instance = project.find_instance_by_id(element.object_id, element.instance_id);
    if (object == nullptr) {
        return;
    }
    if (instance == nullptr && !object->instances.empty()) {
        instance = object->instances.front();
    }
    if (instance == nullptr) {
        return;
    }
    std::vector<const indexed_triangle_set*> meshes;
    std::vector<Domain::Transform3d> transformations;
    for (const Domain::ModelVolume* volume : object->volumes) {
        if (volume->is_model_part()) {
            meshes.push_back(&volume->mesh().its);
            transformations.push_back(instance->get_matrix() * volume->get_matrix());
        }
    }
    const std::optional<Biz::OrientationResult> result = Biz::optimize_orientation(meshes, transformations);
    if (!result) {
        AppServices::instance().dialog_manager().show_info_dialog(
            _u8L("The current orientation is already the one needing the least support."), _u8L("Optimize orientation")
        );
        return;
    }
    const std::optional<Biz::Scene::SelectionExtents> extents = scene.selection_bounding_box();
    if (!extents) {
        return;
    }
    // Rotate the found direction to point down, around the object's center, then onto the bed.
    const Domain::Vec3d center = extents->oriented_bounding_box().center;
    const Eigen::Quaterniond rotation = Eigen::Quaterniond::FromTwoVectors(result->down, Domain::Vec3d(0., 0., -1.));
    Domain::Transform3d transform{Domain::Transform3d::Identity()};
    transform.translate(center);
    transform.rotate(rotation);
    transform.translate(-center);
    scene.transform_selection(transform.matrix(), true);
    m_project_interactor.undo_provider().take_snapshot(Biz::UndoSnapshotType::OptimizeOrientation);
}

void MenuCommandRegistrar::take_config_snapshot()
{
    IDialogManager& dialogs = AppServices::instance().dialog_manager();
    const std::string title = _u8L("Take configuration snapshot");
    const std::string comment = dialogs.show_input_dialog(title, _u8L("Snapshot name:"), _u8L("Snapshot"));
    if (comment.empty()) {
        return;
    }
    const auto snapshot = Biz::ConfigSnapshots::take_snapshot(comment);
    if (!snapshot) {
        dialogs.show_error_dialog(snapshot.error(), title);
    } else {
        dialogs.show_info_dialog(fmt::format("{}: {} ({})", _u8L("Snapshot taken"), snapshot->comment, snapshot->id), title);
    }
}

void MenuCommandRegistrar::show_config_snapshots()
{
    IDialogManager& dialogs = AppServices::instance().dialog_manager();
    const std::string title = _u8L("Configuration snapshots");
    const std::vector<Biz::ConfigSnapshots::Snapshot> snapshots = Biz::ConfigSnapshots::list_snapshots();
    if (snapshots.empty()) {
        dialogs.show_info_dialog(_u8L("There are no configuration snapshots yet. Take one with \"Take configuration snapshot\"."), title);
        return;
    }
    std::vector<std::string> values;
    for (const auto& snapshot : snapshots) {
        values.push_back(fmt::format("{} - {} ({})", snapshot.id, snapshot.comment, snapshot.app_version));
    }
    const std::string chosen = dialogs.show_combo_dialog(
        title,
        _u8L("Restore the presets and physical printers of a snapshot (the current ones are saved in a new snapshot first):"),
        values
    );
    const auto it = std::find(values.begin(), values.end(), chosen);
    if (chosen.empty() || it == values.end()) {
        return;
    }
    const auto& snapshot = snapshots[size_t(it - values.begin())];
    if (const auto restored = Biz::ConfigSnapshots::restore_snapshot(snapshot); !restored) {
        dialogs.show_error_dialog(restored.error(), title);
    } else {
        dialogs.show_info_dialog(
            fmt::format("{} {}\n\n{}", _u8L("Restored the snapshot of"), snapshot.id,
                        _u8L("Restart PrusaSlicer to load the restored configuration.")),
            title
        );
    }
}

void MenuCommandRegistrar::register_url_scheme()
{
#if defined(__linux__) && !defined(__ANDROID__)
    IDialogManager& dialogs = AppServices::instance().dialog_manager();
    const std::string title = _u8L("Open prusaslicer:// links in PrusaSlicer");
    // The AppImage, when running from one, else this executable.
    std::string executable;
    if (const char* appimage = std::getenv("APPIMAGE"); appimage != nullptr && *appimage != 0) {
        executable = appimage;
    } else {
        boost::system::error_code ec;
        executable = boost::dll::program_location(ec).string();
    }
    const char* home = std::getenv("HOME");
    if (executable.empty() || home == nullptr) {
        dialogs.show_error_dialog(_u8L("The location of PrusaSlicer could not be found."), title);
        return;
    }
    const char* data_home_env = std::getenv("XDG_DATA_HOME");
    const boost::filesystem::path applications =
        boost::filesystem::path(data_home_env != nullptr && *data_home_env != 0 ? data_home_env : std::string(home) + "/.local/share") / "applications";
    boost::system::error_code ec;
    boost::filesystem::create_directories(applications, ec);
    const std::string desktop_name = "PrusaSlicer3URLProtocol.desktop";
    boost::nowide::ofstream file((applications / desktop_name).string(), std::ios::binary);
    // Exec quoting per the Desktop Entry spec: double quotes, with " ` $ \\ escaped.
    std::string quoted = "\"";
    for (char c : executable) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += "\"";
    file << "[Desktop Entry]\n"
            "Name=PrusaSlicer URL Protocol\n"
            "Exec=" << quoted << " %u\n"
            "Terminal=false\n"
            "Type=Application\n"
            "MimeType=x-scheme-handler/prusaslicer;\n"
            "Icon=PrusaSlicer\n"
            "NoDisplay=true\n";
    file.close();
    if (!file) {
        dialogs.show_error_dialog(_u8L("The desktop file could not be written."), title);
        return;
    }
    const int result = std::system(("xdg-mime default " + desktop_name + " x-scheme-handler/prusaslicer").c_str());
    std::system(("update-desktop-database \"" + applications.string() + "\" >/dev/null 2>&1").c_str());
    if (result != 0) {
        dialogs.show_error_dialog(
            fmt::format("{}\n{}", _u8L("Registering the link handler with xdg-mime failed."), (applications / desktop_name).string()),
            title
        );
        return;
    }
    dialogs.show_info_dialog(
        fmt::format("{}\n{}", _u8L("prusaslicer:// links (e.g. from Printables) now open in this PrusaSlicer:"), executable), title
    );
#endif
}

void MenuCommandRegistrar::show_tip_of_the_day()
{
    static const std::vector<std::string> tips{
        _u8L("Optimize orientation (object menu) turns an object so that it needs the least support."),
        _u8L("Fill bed with instances (object menu) adds as many copies as fit on the bed."),
        _u8L("Set auto color changes (layers slider menu) adds color changes where an object gets smaller, e.g. the text of a sign."),
        _u8L("The \"{ }\" button next to a custom G-code field inserts placeholders such as {layer_z}."),
        _u8L("Import config (File > Import) opens the settings of a PrusaSlicer 2.x .ini file or G-code with a copy of your objects."),
        _u8L("Click a setting's name to see where its value comes from."),
        _u8L("Object info (object menu) shows the size, volume and mesh errors of an object."),
        _u8L("The shape gallery keeps the models you use often; add them with \"Add shape from file\"."),
        _u8L("Export toolpaths as OBJ (File > Export, in the preview) saves the G-code as a 3D model."),
        _u8L("Take a configuration snapshot (Configuration menu) before experimenting with your presets."),
        _u8L("Post-processing scripts get the G-code path as their argument and all settings as SLIC3R_* environment variables."),
        _u8L("Open a .gcode or .bgcode file to preview it, with the settings stored in the file."),
        _u8L("Mirror (object menu) flips an object along X, Y or Z."),
        _u8L("Scale to print volume (object menu) makes an object as large as the printer allows."),
    };
    static size_t next = size_t(std::time(nullptr)) % tips.size();
    const std::string tip = tips[next++ % tips.size()];
    AppServices::instance().dialog_manager().show_info_dialog(tip, _u8L("Tip of the day"));
}

void MenuCommandRegistrar::export_wifi_config()
{
    // prusa_printer_settings.ini, read by Prusa printers (MK4, XL, MINI, CORE One...) from a USB drive.
    IDialogManager& dialogs = AppServices::instance().dialog_manager();
    const std::string title = _u8L("Wi-Fi configuration file");
    const std::string ssid  = dialogs.show_input_dialog(
        title,
        _u8L("Generate a file to be loaded by a Prusa printer to configure its Wi-Fi connection.") + "\n\n" + _u8L("Network name (SSID):"),
        ""
    );
    if (ssid.empty()) {
        return;
    }
    const std::string password = dialogs.show_input_dialog(title, _u8L("Password:"), "");
    if (ssid.find_first_of("\r\n") != std::string::npos || password.find_first_of("\r\n") != std::string::npos) {
        dialogs.show_error_dialog(_u8L("The network name and the password cannot contain line breaks."), title);
        return;
    }
    IDialogManager::FileCallback callback = [ssid, password, title](bool success, const std::vector<boost::filesystem::path>& paths)
    {
        if (!success || paths.empty()) {
            return;
        }
        boost::nowide::ofstream file(paths.front().string(), std::ios::binary);
        file << "[wifi]\nssid = " << ssid << "\npsk = " << password << "\n";
        file.close();
        IDialogManager& dialogs = AppServices::instance().dialog_manager();
        if (!file) {
            dialogs.show_error_dialog(_u8L("The file could not be written."), title);
        } else {
            dialogs.show_info_dialog(
                _u8L("Copy the file to the root of a USB drive and insert it into the printer: it loads the settings on start-up."),
                title
            );
        }
    };
    dialogs.show_file_dialog(
        FileDialogType::Save, title, this->default_dialog_folder(), "prusa_printer_settings.ini",
        Wildcards::generate_wildcards(Wildcards::TypeFlag::Ini, Wildcards::TypeFlag::Ini), callback
    );
}

void MenuCommandRegistrar::show_print_host_queue()
{
    using Biz::PrintHost::UploadRecord;
    auto& manager = m_project_interactor.result_export_interactor().print_host_job_manager();
    const std::vector<UploadRecord> uploads = manager.uploads();

    const auto state_text = [](const UploadRecord& r) -> std::string {
        switch (r.state) {
        case UploadRecord::State::Queued: return _u8L("Queued");
        case UploadRecord::State::Uploading: return fmt::format("{} {:.0f} %", _u8L("Uploading"), r.progress * 100.);
        case UploadRecord::State::Finished: return _u8L("Completed");
        case UploadRecord::State::Failed: return _u8L("Error") + (r.error.empty() ? "" : ": " + r.error);
        case UploadRecord::State::Cancelled: return _u8L("Canceled");
        }
        return {};
    };
    std::string text;
    for (const UploadRecord& r : uploads) {
        text += fmt::format("{} → {} ({}): {}\n", r.filename, r.printer, r.host, state_text(r));
    }
    if (text.empty()) {
        text = _u8L("No uploads in this session.");
    }

    const std::string close   = _u8L("Close");
    const std::string refresh = _u8L("Refresh");
    const std::string clear   = _u8L("Clear finished uploads");
    std::vector<std::string> actions{close, refresh};
    std::vector<size_t> cancel_ids;
    for (const UploadRecord& r : uploads) {
        if (r.state == UploadRecord::State::Queued || r.state == UploadRecord::State::Uploading) {
            actions.push_back(fmt::format("{} {} (#{})", _u8L("Cancel"), r.filename, r.id));
            cancel_ids.push_back(r.id);
        }
    }
    if (uploads.size() > cancel_ids.size()) {
        actions.push_back(clear);
    }

    const std::string chosen =
        AppServices::instance().dialog_manager().show_combo_dialog(_u8L("Print host upload queue"), text, actions);
    if (chosen.empty() || chosen == close) {
        return;
    }
    if (chosen == clear) {
        manager.clear_finished_uploads();
    } else {
        for (size_t i = 0; i < cancel_ids.size(); ++i) {
            if (chosen == actions[2 + i]) {
                manager.cancel_job(cancel_ids[i]);
            }
        }
    }
    show_print_host_queue(); // show the updated list
}

void MenuCommandRegistrar::show_object_info()
{
    // 2.x "Info" panel of the object list: size, volume, facets and mesh errors.
    const auto& selection = m_project_interactor.scene_interactor().object_selection();
    if (!selection.only_single_object()) {
        return;
    }
    const Domain::ModelObject* object =
        m_project_interactor.selected_project().find_object_by_id(selection.elements.front().object_id);
    if (object == nullptr || object->instances.empty()) {
        return;
    }
    const Domain::Transform3d instance_matrix = object->instances.front()->get_matrix();

    double volume = 0.;
    size_t facets = 0;
    int parts = 0, open_edges = 0;
    Domain::RepairedMeshErrors repaired;
    for (const Domain::ModelVolume* model_volume : object->volumes) {
        if (!model_volume->is_model_part()) {
            continue;
        }
        const Domain::TriangleMesh& mesh  = model_volume->mesh();
        const Domain::TriangleMeshStats& stats = mesh.stats();
        const double scale = std::abs((instance_matrix * model_volume->get_matrix()).linear().determinant());
        volume += double(stats.volume >= 0.f ? stats.volume : Domain::its_volume(mesh.its)) * scale;
        facets += mesh.facets_count();
        parts += stats.number_of_parts;
        open_edges += stats.open_edges;
        repaired.merge(stats.repaired_errors);
    }

    std::string text = object->name + "\n\n";
    if (const std::optional<Biz::Scene::SelectionExtents> extents =
            m_project_interactor.scene_interactor().selection_bounding_box()) {
        const Domain::Vec3d size = extents->oriented_bounding_box().dimensions;
        text += fmt::format("{}: {:.2f} x {:.2f} x {:.2f} mm\n", _u8L("Size"), size.x(), size.y(), size.z());
    }
    text += fmt::format("{}: {:.2f} mm³\n", _u8L("Volume"), volume);
    text += fmt::format("{}: {}\n", _u8L("Facets"), facets);
    text += fmt::format("{}: {}\n", _u8L("Parts"), parts);
    text += fmt::format("{}: {}\n", _u8L("Instances"), object->instances.size());
    if (open_edges > 0) {
        text += fmt::format("{}: {}\n", _u8L("Open edges"), open_edges);
    }
    if (repaired.repaired()) {
        text += "\n" + _u8L("Auto-repaired errors") + ":\n";
        const std::pair<int, std::string> errors[] = {
            {repaired.edges_fixed, _u8L("edges fixed")},
            {repaired.degenerate_facets, _u8L("degenerate facets")},
            {repaired.facets_removed, _u8L("facets removed")},
            {repaired.facets_reversed, _u8L("facets reversed")},
            {repaired.backwards_edges, _u8L("backwards edges")},
        };
        for (const auto& [count, label] : errors) {
            if (count > 0) {
                text += fmt::format("  {} {}\n", count, label);
            }
        }
    } else if (open_edges == 0) {
        text += "\n" + _u8L("No errors detected");
    }
    AppServices::instance().dialog_manager().show_info_dialog(text, _u8L("Object info"));
}

void MenuCommandRegistrar::show_keyboard_shortcuts()
{
    // All commands with a keyboard shortcut (2.x showed them grouped in a dialog).
    auto translator = [](const std::string& s) { return Biz::_u8(s); };
    std::vector<std::pair<std::string, std::string>> rows;
    auto collect = [&](const Platform::CommandRegistry::CommandsMap& commands) {
        for (const auto& [name, command] : commands) {
            if (!command || !command->keyboard_shortcuts()) {
                continue;
            }
            const std::string shortcut = command->keyboard_shortcut_string(translator);
            if (shortcut.empty()) {
                continue;
            }
            std::string label = name;
            std::replace(label.begin(), label.end(), '-', ' ');
            if (!label.empty()) {
                label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
            }
            rows.emplace_back(shortcut, label);
        }
    };
    collect(m_render_module.commands());
    collect(m_render_module.gizmo_commands());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::string text;
    for (const auto& [shortcut, label] : rows) {
        text += fmt::format("{}\t{}\n", shortcut, label);
    }
    text += "\n" + _u8L("Mouse: left drag = rotate, right drag = pan, wheel = zoom.");
    AppServices::instance().dialog_manager().show_info_dialog(text, _u8L("Keyboard Shortcuts"));
}

void MenuCommandRegistrar::scale_selection_to_print_volume()
{
    auto& scene                                     = m_project_interactor.scene_interactor();
    const std::optional<Biz::Scene::SelectionExtents> extents = scene.selection_bounding_box();
    if (!extents) {
        return;
    }
    const Domain::BedRef bed_ref = scene.bed_selection().last_selected_bed();
    const Domain::BedInstance* bed_instance =
        m_project_interactor.selected_project().find_bed_instance_by_id(bed_ref.instance_id);
    if (bed_instance == nullptr) {
        return;
    }
    const Domain::Bed& bed = bed_instance->bed.get();
    if (bed.contour().empty()) {
        return;
    }
    Domain::Vec2d bed_min = bed.contour().front(), bed_max = bed.contour().front();
    for (const Domain::Vec2d& point : bed.contour()) {
        bed_min = bed_min.cwiseMin(point);
        bed_max = bed_max.cwiseMax(point);
    }
    const Domain::Vec2d bed_size   = bed_max - bed_min;
    const Domain::Vec3d dimensions = extents->oriented_bounding_box().dimensions;
    if (dimensions.minCoeff() <= 0.) {
        return;
    }
    // As in 2.x: the largest uniform scale that fits, with a small margin.
    double factor = std::min(bed_size.x() / dimensions.x(), bed_size.y() / dimensions.y());
    if (bed.max_print_height() > 0.f) {
        factor = std::min(factor, double(bed.max_print_height()) / dimensions.z());
    }
    factor *= 0.99;
    if (std::abs(factor - 1.) < 1e-4) {
        return;
    }
    // Scale around the selection center, then move it to the bed center.
    const Domain::Vec3d center = extents->oriented_bounding_box().center;
    Domain::Transform3d scale{Domain::Transform3d::Identity()};
    scale.translate(center);
    scale.scale(factor);
    scale.translate(-center);
    scene.transform_selection(scale.matrix(), true);

    if (const std::optional<Biz::Scene::SelectionExtents> scaled = scene.selection_bounding_box()) {
        const Domain::Vec3d bed_offset{bed_instance->transformation.get_offset()};
        const Domain::Vec2d bed_center = 0.5 * (bed_min + bed_max) + Domain::Vec2d{bed_offset.x(), bed_offset.y()};
        const Domain::Vec3d now        = scaled->oriented_bounding_box().center;
        Domain::Transform3d move{Domain::Transform3d::Identity()};
        move.translate(Domain::Vec3d{bed_center.x() - now.x(), bed_center.y() - now.y(), 0.});
        scene.transform_selection(move.matrix(), true);
    }
    m_project_interactor.undo_provider().take_snapshot(Biz::UndoSnapshotType::ScaleToPrintVolume);
}

void MenuCommandRegistrar::mirror_selection(int axis)
{
    auto& scene                                     = m_project_interactor.scene_interactor();
    const std::optional<Biz::Scene::SelectionExtents> extents = scene.selection_bounding_box();
    if (!extents || axis < 0 || axis > 2) {
        return;
    }
    const Domain::Vec3d center = extents->oriented_bounding_box().center;
    Domain::Vec3d factors{1., 1., 1.};
    factors[axis] = -1.;
    Domain::Transform3d transform{Domain::Transform3d::Identity()};
    transform.translate(center);
    transform.scale(factors);
    transform.translate(-center);
    scene.transform_selection(transform.matrix(), true);
    m_project_interactor.undo_provider().take_snapshot(Biz::UndoSnapshotType::Mirror);
}

void MenuCommandRegistrar::export_selection_as_stl_obj()
{
    IDialogManager::FileCallback callback =
        [this](const bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (!success || file_paths.empty()) {
            return;
        }

        MeshExportLogic::export_selection(
            file_paths.front(),
            m_project_interactor.scene_interactor(),
            &AppServices::instance().dialog_manager()
        );
    };

    AppServices::instance().dialog_manager().show_file_dialog(
        FileDialogType::Save,
        _u8L("Export as STL/OBJ"),
        this->default_dialog_folder(),
        MeshExportLogic::proposed_export_file_name(m_project_interactor.scene_interactor()),
        Wildcards::generate_wildcards(
            Wildcards::TypeFlag::Stl | Wildcards::TypeFlag::Obj,
            Wildcards::TypeFlag::Stl
        ),
        callback
    );
}

void MenuCommandRegistrar::replace_selected_volume_with_stl()
{
    IDialogManager::FileCallback callback =
        [this](const bool success, const std::vector<boost::filesystem::path>& file_paths)
    {
        if (!success || file_paths.empty()) {
            return;
        }

        const tl::expected<void, std::string> replace_result =
            VolumeReloadLogic::replace_selected_volume(
                file_paths.front(),
                m_project_interactor.scene_interactor(),
                &AppServices::instance().dialog_manager()
            );

        if (!replace_result) {
            AppServices::instance().dialog_manager().show_warning_dialog(
                replace_result.error(),
                _u8L("Error during replace")
            );

            return;
        }

        m_project_interactor.undo_provider().take_snapshot(UndoSnapshotType::ReplaceWithStl);
    };

    AppServices::instance().dialog_manager().show_file_dialog(
        FileDialogType::Open,
        _u8L("Select the new file:"),
        this->default_dialog_folder(),
        VolumeReloadLogic::proposed_replace_file_name(m_project_interactor.scene_interactor()),
        Wildcards::generate_wildcards(model_file_types, Wildcards::TypeFlag::AllImportFiles),
        callback
    );
}

void MenuCommandRegistrar::reload_selection_from_disk()
{
    IDialogManager& dialog_manager = AppServices::instance().dialog_manager();

    VolumeReloadLogic::MissingFileResolver missing_file_resolver =
        [this, &dialog_manager](
            const boost::filesystem::path& missing_file
        ) -> std::optional<boost::filesystem::path>
    {
        std::optional<boost::filesystem::path> selected_path;
        dialog_manager.show_file_dialog(
            FileDialogType::Open,
            _u8L("Please select the file to reload")
                + " ("
                + missing_file.filename().string()
                + "):",
            default_dialog_folder(),
            missing_file.filename().string(),
            Wildcards::generate_wildcards(model_file_types, Wildcards::TypeFlag::AllImportFiles),
            [&selected_path](bool success, const std::vector<boost::filesystem::path>& file_paths)
            {
                if (success && !file_paths.empty()) {
                    selected_path = file_paths.front();
                }
            }
        );
        return selected_path;
    };

    const VolumeReloadLogic::ReloadFromDiskResult reload_result =
        VolumeReloadLogic::reload_selection(
            missing_file_resolver,
            m_project_interactor.scene_interactor(),
            &dialog_manager
        );

    if (!reload_result.failed_volume_names.empty()) {
        std::string message = _u8L("Unable to reload:") + "\n";
        for (const std::string& failed_volume_name : reload_result.failed_volume_names) {
            message += failed_volume_name + "\n";
        }

        dialog_manager.show_warning_dialog(message, _u8L("Error during reload"));
    }

    if (reload_result.changed_volume_count > 0) {
        m_project_interactor.undo_provider().take_snapshot(Biz::UndoSnapshotType::ReloadFromDisk);
    }
}

void MenuCommandRegistrar::register_main_menu_edit_commands()
{
    m_menu_manager
        // Menu -> Edit -> Select All
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::SelectAll},
            std::make_unique<UIItemCommand>(
                CommandName::SelectAll,
                [this]()
                {
                    Biz::Scene::ObjectSelection selection{Biz::Scene::SelectionMode::Instance, {}};

                    const Domain::Model& model = m_project_interactor.selected_project().model();
                    for (const auto object : model.objects) {
                        for (const auto instance : object->instances) {
                            selection.elements.push_back({object->id().id, instance->id().id});
                        }
                    }
                    m_project_interactor.scene_interactor().set_object_selection(selection);
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::A

                    }}
                }
            )
        )
        // Menu -> Edit -> Deselect All
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::DeselectAll},
            std::make_unique<UIItemCommand>(
                CommandName::ClearSelection,
                [this]()
                {
                    auto& scene_interactor = m_project_interactor.scene_interactor();
                    if (!scene_interactor.object_selection().empty())
                        scene_interactor.clear_object_selection();
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts =
                        Platform::KeyboardShortcuts{
                            Platform::KeyboardShortcut{0, Platform::KeyCode::Escape}
                        },
                    .enabled =
                        [this]()
                    {
                        const bool is_selection_empty{
                            m_project_interactor.scene_interactor().object_selection().empty()
                        };

                        auto plater_render_module{
                            dynamic_cast<Plater::PlaterRenderModule*>(&m_render_module)
                        };
                        if (is_selection_empty || !plater_render_module) {
                            return false;
                        }
                        return plater_render_module->gizmo_controller().current_tool_type()
                            == Scene::ToolType::None;
                    }
                }
            )
        )
        // Menu -> Edit -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Edit})
        // Menu -> Edit -> Delete Selected
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::DeleteSelected},
            std::make_unique<UIItemCommand>(
                CommandName::DeleteSelected,
                [this]()
                {
                    if (m_project_interactor.scene_interactor().object_selection().empty())
                        return;

                    std::optional<std::string> last_solid_part_name =
                        m_project_interactor.scene_interactor().delete_selected_elements();

                    if (last_solid_part_name) {
                        // Show warning dialog
                        auto& dlg_manager = App::AppServices::instance().dialog_manager();
                        dlg_manager.show_warning_dialog(
                            fmt::vformat(
                                _u8L(
                                    // TRN {} is the name of the part
                                    "Part {} could not be deleted from the object,\n"
                                    "as removing the last solid part is not permitted."
                                ),
                                fmt::make_format_args(last_solid_part_name.value())
                            ) + "\n",
                            _u8L("Delete selection")
                        );
                    }
                    m_project_interactor.undo_provider().take_snapshot(
                        UndoSnapshotType::DeleteSelection
                    );
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts =
                        Platform::KeyboardShortcuts{
                            Platform::KeyboardShortcut{0, Platform::KeyCode::Backspace},
                            Platform::KeyboardShortcut{0, Platform::KeyCode::Delete}
                        },
                    .enabled =
                        [this]()
                    {
                        const Biz::Scene::ObjectSelection& object_selection =
                            m_project_interactor.scene_interactor().object_selection();
                        return !object_selection.empty() && !object_selection.contains_wipe_tower();
                    }
                }
            )
        )
#ifdef SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> Edit -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Edit})
        // Menu -> Edit -> Undo
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Undo},
            std::make_unique<UIItemCommand>(
                CommandName::Undo,
                [this]()
                {
                    // TODO: Implement undo functionality
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::Z
                    }}
                }
            )
        )
        // Menu -> Edit -> Redo
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Redo},
            std::make_unique<UIItemCommand>(
                CommandName::Redo,
                [this]()
                {
                    // TODO: Implement redo functionality
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::Y
                    }}
                }
            )
        )
#endif // SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> Edit -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Edit})
        // Menu -> Edit -> Copy
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Copy},
            std::make_unique<UIItemCommand>(
                CommandName::Copy,
                [this]()
                { m_clipboard_interactor.copy(m_project_interactor.selected_project_id()); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::C
                    }},
                    .enabled            = [this]() { return m_clipboard_interactor.can_copy(); }
                }
            )
        )
        // Menu -> Edit -> Paste
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Paste},
            std::make_unique<UIItemCommand>(
                CommandName::Paste,
                [this]()
                { m_clipboard_interactor.paste(m_project_interactor.selected_project_id()); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::V
                    }},
                    .enabled            = [this]() { return m_clipboard_interactor.can_paste(); }
                }
            )
        )
#ifdef SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> Edit -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Edit})
        // Menu -> Edit -> Reload From Disk
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::ReloadFromDisk},
            std::make_unique<UIItemCommand>(
                CommandName::ReloadFromDisk,
                [this]()
                {
                    // TODO: Implement reload from disk functionality
                }
            )
        )
#endif // SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> Edit -> Search
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Search},
            std::make_unique<UIItemCommand>(
                CommandName::Search,
                [this]() { m_navigator.request_search(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::F
                    }}
                }
            )
        )
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::Arrange},
            std::make_unique<UIItemCommand>(
                CommandName::Arrange,
                [this]() {
                    const auto it{m_render_module.gizmo_commands().find("arrange-gizmo-arrange")};
                    if (it != m_render_module.gizmo_commands().end()) {
                        it->second->execute();
                    }
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers{},
                        Platform::KeyCode::A
                    }}
                }
            )
        )
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Edit, MenuItemName::ArrangeSelection},
            std::make_unique<UIItemCommand>(
                CommandName::ArrangeSelection,
                [this]() {
                    const auto it{m_render_module.gizmo_commands().find("arrange-gizmo-arrange-selection")};
                    if (it != m_render_module.gizmo_commands().end()) {
                        it->second->execute();
                    }
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Shift),
                        Platform::KeyCode::A
                    }}
                }
            )
        );
}

void MenuCommandRegistrar::register_main_menu_view_commands()
{
    m_menu_manager
#ifdef SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> View -> Show Label
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::ShowLabel},
            std::make_unique<UIItemCommand>(
                CommandName::ShowLabel,
                [this]()
                {
                    // TODO: Implement show label functionality
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts =
                        Platform::KeyboardShortcuts{
                            Platform::KeyboardShortcut{0, Platform::KeyCode::E}
                        }
                }
            )
        )
#endif
        // Menu -> View -> ZoomIn
        .register_menu_item_from_command(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::ZoomIn},
            m_render_module.command(CommandName::ZoomIn)
        )
        // Menu -> View -> ZoomOut
        .register_menu_item_from_command(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::ZoomOut},
            m_render_module.command(CommandName::ZoomOut)
        )
        // Menu -> View -> Change Camera Type
        .register_menu_item_from_command(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::CameraProjectionSwitch},
            m_render_module.command(CommandName::CameraProjectionSwitch)
        )
        // Menu -> View -> LookAtActiveBed
        .register_menu_item_from_command(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::LookAtActiveBed},
            m_render_module.command(CommandName::LookAtActiveBed)
        )
        // Menu -> View -> CameraDefaultView
        .register_menu_item_from_command(
            {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::CameraDefaultView},
            m_render_module.command(CommandName::CameraDefaultView)
        );
#ifndef __APPLE__
    // OSX adds its own menu item to toggle fullscreen.
    if (m_navigator.has_fullscreen()) {
        // Menu -> View -> Separator
        m_menu_manager
            .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::View})
            // Menu -> View -> Full Screen
            .register_menu_item(
                {MenuItemName::MainMenu, MenuItemName::View, MenuItemName::FullScreen},
                std::make_unique<UIItemCommand>(
                    CommandName::FullScreen,
                    [this]() { m_navigator.set_fullscreen(!m_navigator.is_fullscreen()); },
                    UIItemCommandExtraOpts{
                        .keyboard_shortcuts =
                            Platform::KeyboardShortcuts{
                                Platform::KeyboardShortcut{0, Platform::KeyCode::F11}
                            },
                        .checked = [this]() { return m_navigator.is_fullscreen(); }
                    }
                )
            );
    }
#endif // __APPLE__
}

void MenuCommandRegistrar::register_main_menu_config_commands()
{
#ifdef SHOW_NOT_IMPLEMENTED_ITEMS
    m_menu_manager
        // Menu -> Configuration -> Configuration Wizard
        .register_menu_item(
            {MenuItemName::MainMenu,
             MenuItemName::Configuration,
             MenuItemName::ConfigurationWizard},
            std::make_unique<UIItemCommand>(
                CommandName::ConfigurationWizard,
                [this]()
                {
                    // TODO: Implement configuration wizard functionality
                }
            )
        )
        // Menu -> Configuration -> Check Config Updates
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::CheckConfigUpdates},
            std::make_unique<UIItemCommand>(
                CommandName::CheckConfigUpdates,
                [this]()
                {
                    // TODO: Implement check config updates functionality
                }
            )
        )
        // Menu -> Configuration -> Check App Updates
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::CheckAppUpdates},
            std::make_unique<UIItemCommand>(
                CommandName::CheckAppUpdates,
                [this]()
                {
                    // TODO: Implement check app updates functionality
                }
            )
        )
        // Menu -> Configuration -> Flash Printer Firmware
        .register_menu_item(
            {MenuItemName::MainMenu,
             MenuItemName::Configuration,
             MenuItemName::FlashPrinterFirmware},
            std::make_unique<UIItemCommand>(
                CommandName::FlashPrinterFirmware,
                [this]()
                {
                    // TODO: Implement flash printer firmware functionality
                }
            )
        );
#endif
    m_menu_manager.register_menu_item(
        {MenuItemName::MainMenu,
         MenuItemName::Configuration,
         MenuItemName::PresetReposManagement},
        std::make_unique<UIItemCommand>(
            CommandName::PresetReposManagement,
            [this]() { m_navigator.set_modal_dialog(ModalDialog::PresetUpdater); }
        )
    );
    // Menu -> Configuration -> configuration snapshots (2.x)
    m_menu_manager.register_menu_item(
        {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::TakeConfigSnapshot},
        std::make_unique<UIItemCommand>("take-config-snapshot", [this]() { take_config_snapshot(); })
    );
    m_menu_manager.register_menu_item(
        {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::ConfigSnapshots},
        std::make_unique<UIItemCommand>("config-snapshots", [this]() { show_config_snapshots(); })
    );
#if defined(__linux__) && !defined(__ANDROID__)
    // Menu -> Configuration -> register prusaslicer:// links (2.x desktop integration)
    m_menu_manager.register_menu_item(
        {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::RegisterUrlScheme},
        std::make_unique<UIItemCommand>("register-url-scheme", [this]() { register_url_scheme(); })
    );
#endif
    // Menu -> Configuration -> Wi-Fi configuration file (2.x)
    m_menu_manager.register_menu_item(
        {MenuItemName::MainMenu, MenuItemName::Configuration, MenuItemName::WifiConfigFile},
        std::make_unique<UIItemCommand>("wifi-config-file", [this]() { export_wifi_config(); })
    );
}

void MenuCommandRegistrar::register_main_menu_help_commands()
{
    m_menu_manager
        // Menu -> Help -> Prusa Website
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::PSWebsite},
            std::make_unique<UIItemCommand>(
                CommandName::PSWebsite,
                [this]()
                {
                    open_browser(
                        OpenBrowserParams{
                            .url              = "https://www.prusa3d.com/slicerweb",
                            .is_localized_url = true
                        }
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = hyperlinks_allowed
                }
            )
        )
        // Menu -> Help -> Samples
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::Samples},
            std::make_unique<UIItemCommand>(
                CommandName::Samples,
                [this]()
                {
                    open_browser(
                        OpenBrowserParams{
                            .url = "https://help.prusa3d.com/article/sample-g-codes_529630"
                        }
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = hyperlinks_allowed
                }
            )
        )
        // Menu -> Help -> Releases
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::Releases},
            std::make_unique<UIItemCommand>(
                CommandName::Releases,
                [this]()
                {
                    open_browser(
                        OpenBrowserParams{
                            .url = "https://github.com/prusa3d/PrusaSlicer/releases"
                        }
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = hyperlinks_allowed
                }
            )
        )
        // Menu -> Help -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Help})
        // Menu -> Help -> System Info
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::SystemInfo},
            std::make_unique<UIItemCommand>(CommandName::SystemInfo, [this]() { show_system_info(); })
        )
        // Menu -> Help -> Config Folder
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::ConfigFolder},
            std::make_unique<UIItemCommand>(
                CommandName::ConfigFolder,
                []()
                {
                    AppServices::instance().file_explorer_handler().open_datadir_folder();
                }
            )
        )
        // Menu -> Help -> Report An Issue
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::ReportAnIssue},
            std::make_unique<UIItemCommand>(
                CommandName::ReportAnIssue,
                [this]()
                {
                    open_browser(
                        OpenBrowserParams{
                            .url = "https://github.com/prusa3d/slic3r/issues/new/choose"
                        }
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = hyperlinks_allowed
                }
            )
        )
        // Menu -> Help -> About
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::About},
            std::make_unique<UIItemCommand>(CommandName::About, [this]() { show_about(); })
        )
        // Menu -> Help -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu, MenuItemName::Help})
        // Menu -> Help -> Keyboard Shortcuts
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::KeyboardShortcutsDialog},
            std::make_unique<UIItemCommand>(CommandName::KeyboardShortcutsDialog, [this]() { show_keyboard_shortcuts(); })
        )
        // Menu -> Help -> Tip Of The Day
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Help, MenuItemName::TipOfTheDay},
            std::make_unique<UIItemCommand>(CommandName::TipOfTheDay, [this]() { show_tip_of_the_day(); })
        )
        ;
}

void MenuCommandRegistrar::register_undo_redo_commands()
{
    m_menu_manager
        .register_command(
            std::make_unique<Platform::FuncCommand>(
                CommandName::Undo,
                [&]()
                {
                    m_project_interactor.undo_provider().select_snapshot(
                        Biz::UndoSnapshotSelection::Prev{}
                    );
                },
                Platform::FuncCommandExtraOpts{
                    .keyboard_shortcuts =
                        Platform::KeyboardShortcuts{
                            Platform::KeyboardShortcut{
                                Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                                Platform::KeyCode::Z
                            },
                        },

                    .enabled =
                        [&]()
                    {
                        if (dynamic_cast<Plater::PlaterRenderModule*>(&m_render_module))
                        {
                            return m_project_interactor.undo_provider().is_undo_possible();
                        }
                        return false;
                    }
                }
            )
        )
        .register_command(
            std::make_unique<Platform::FuncCommand>(
                CommandName::Redo,
                [&]()
                {
                    m_project_interactor.undo_provider().select_snapshot(
                        Biz::UndoSnapshotSelection::Next{}
                    );
                },
                Platform::FuncCommandExtraOpts{
                    .keyboard_shortcuts =
                        Platform::KeyboardShortcuts{
                            Platform::KeyboardShortcut{
                                Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                                Platform::KeyCode::Y
                            },
                        },
                    .enabled = [&]()
                    {
                        if (dynamic_cast<Plater::PlaterRenderModule*>(&m_render_module))
                        {
                            return m_project_interactor.undo_provider().is_redo_possible();
                        }
                        return false;
                    }
                }
            )
        );
}

void MenuCommandRegistrar::register_main_menu_commands(Lua::PluginSystem* plugin_system)
{
    register_main_menu_edit_commands();
    register_main_menu_view_commands();
    if (plugin_system != nullptr) {
        register_main_menu_plugin_commands(*plugin_system);
    }

    m_menu_manager
        // Menu -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu})
        // Menu -> Print host upload queue (2.x Window menu)
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::PrintHostQueue},
            std::make_unique<UIItemCommand>("print-host-queue", [this]() { show_print_host_queue(); })
        )
        // Menu -> Preferences
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Preferences},
            std::make_unique<UIItemCommand>(
                CommandName::Preferences,
                [this]() { m_navigator.set_modal_dialog(ModalDialog::Preferences); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
#ifdef __APPLE__
                        Platform::KeyCode::Comma
#else
                        Platform::KeyCode::P
#endif
                    }}
                }
            )
        );

    register_main_menu_config_commands();

    m_menu_manager
        // Menu -> Separator
        .register_menu_separator_item({MenuItemName::MainMenu});

    register_main_menu_help_commands();

    m_menu_manager
        // Menu -> Exit
        .register_menu_item(
            {MenuItemName::MainMenu, MenuItemName::Exit},
            std::make_unique<UIItemCommand>(
                CommandName::Exit,
                [this]() { m_navigator.close_application(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::Q
                    }}
                }
            )
        );
}

void MenuCommandRegistrar::register_file_menu_import_commands()
{
    m_menu_manager
        // File -> Import -> Import Geometry
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Import, MenuItemName::ImportGeometry},
            std::make_unique<UIItemCommand>(
                CommandName::ImportGeometry,
                [this]()
                {
                    // The undo snapshot is taken inside load_models_to_project().
                    load_object();
                },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::I
                    }},
                    .enabled = [&]()
                    {
                        auto plater_render_module{
                                dynamic_cast<Plater::PlaterRenderModule*>(&m_render_module)
                            };
                        return plater_render_module != nullptr;
                    }
                }
            )
        )
        // File -> Import -> G-code preview (2.x G-code viewer)
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Import, MenuItemName::ImportGcodePreview},
            std::make_unique<UIItemCommand>(
                "import-gcode-preview",
                [this]()
                {
                    IDialogManager::FileCallback callback =
                        [this](const bool success, const std::vector<boost::filesystem::path>& file_paths)
                    {
                        if (!success || file_paths.empty()) {
                            return;
                        }
                        if (const auto error = m_project_interactor.open_gcode_preview(file_paths.front())) {
                            AppServices::instance().dialog_manager().show_error_dialog(*error, _u8L("G-code preview"));
                            return;
                        }
                        m_navigator.navigate_to_module_type(Render::ModuleType::Preview);
                    };
                    AppServices::instance().dialog_manager().show_file_dialog(
                        FileDialogType::Open,
                        _u8L("Open a G-code file to preview"),
                        this->default_dialog_folder(),
                        {},
                        Wildcards::generate_wildcards(
                            Wildcards::TypeFlag::GCode | Wildcards::TypeFlag::BinaryGCode, Wildcards::TypeFlag::GCode
                        ),
                        callback
                    );
                }
            )
        )
        // File -> Import -> Import Config (2.x .ini, G-code, 3.x .json)
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Import, MenuItemName::ImportConfig},
            std::make_unique<UIItemCommand>(
                "import-config",
                [this]()
                {
                    IDialogManager::FileCallback callback =
                        [this](const bool success, const std::vector<boost::filesystem::path>& file_paths)
                    {
                        if (!success || file_paths.empty()) {
                            return;
                        }
                        const auto result = m_project_interactor.import_config(file_paths.front());
                        if (!result) {
                            AppServices::instance().dialog_manager().show_error_dialog(result.error(), _u8L("Import config"));
                        }
                    };
                    AppServices::instance().dialog_manager().show_file_dialog(
                        FileDialogType::Open,
                        _u8L("Select configuration to load:"),
                        this->default_dialog_folder(),
                        {},
                        Wildcards::generate_wildcards(
                            Wildcards::TypeFlag::Ini | Wildcards::TypeFlag::Json | Wildcards::TypeFlag::GCode
                                | Wildcards::TypeFlag::BinaryGCode,
                            Wildcards::TypeFlag::Ini
                        ),
                        callback
                    );
                }
            )
        )
        // File -> Import -> Import ZIP archive
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Import, MenuItemName::ImportZip},
            std::make_unique<UIItemCommand>(
                CommandName::ImportZip,
                [this]() { load_archive(); },
                UIItemCommandExtraOpts{
                    .enabled = [&]()
                    {
                        auto plater_render_module{
                                dynamic_cast<Plater::PlaterRenderModule*>(&m_render_module)
                            };
                        return plater_render_module != nullptr;
                    }
                }
            )
        );
}

void MenuCommandRegistrar::register_file_menu_export_commands()
{
    m_menu_manager
        // File -> Export -> Export G-code
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::ExportGcode},
            std::make_unique<UIItemCommand>(
                CommandName::ExportGcode,
                ExportActions::export_gcode(m_project_interactor),
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::G
                    }},
                    .enabled = [this]() { return ExportActions::can_export(m_project_interactor); }
                }
            )
        )
        // File -> Export -> Send G-code
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::SendGcode},
            std::make_unique<UIItemCommand>(
                CommandName::SendGcode,
                ExportActions::send_gcode_to_connect(m_project_interactor),
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl)
                            | Platform::KeyModifiers(Platform::KeyModifier::Shift),
                        Platform::KeyCode::G
                    }},
                    .enabled =
                        [this]()
                    {
                        return ExportActions::can_export(m_project_interactor)
                            && m_project_interactor.user_account_interactor().is_logged_in();
                    }
                }
            )
        )
        // File -> Export -> Export plate as STL/OBJ
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::ExportPlateAsStl},
            std::make_unique<UIItemCommand>(
                "export-plate-as-stl",
                [this]()
                {
                    const Domain::BedRef bed =
                        m_project_interactor.scene_interactor().bed_selection().last_selected_bed();
                    IDialogManager::FileCallback callback =
                        [this, bed](const bool success, const std::vector<boost::filesystem::path>& file_paths)
                    {
                        if (success && !file_paths.empty()) {
                            MeshExportLogic::export_bed(
                                file_paths.front(), m_project_interactor.scene_interactor(), bed,
                                &AppServices::instance().dialog_manager()
                            );
                        }
                    };
                    AppServices::instance().dialog_manager().show_file_dialog(
                        FileDialogType::Save,
                        _u8L("Export plate as STL/OBJ"),
                        this->default_dialog_folder(),
                        "plate.stl",
                        Wildcards::generate_wildcards(
                            Wildcards::TypeFlag::Stl | Wildcards::TypeFlag::Obj, Wildcards::TypeFlag::Stl
                        ),
                        callback
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = [this]()
                    { return !m_project_interactor.selected_project().model().objects.empty(); }
                }
            )
        )
        // File -> Export -> Export Config
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::ExportConfig},
            std::make_unique<UIItemCommand>(
                "export-config",
                [this]()
                {
                    IDialogManager::FileCallback callback =
                        [this](const bool success, const std::vector<boost::filesystem::path>& file_paths)
                    {
                        if (!success || file_paths.empty()) {
                            return;
                        }
                        boost::filesystem::path path = file_paths.front();
                        if (path.extension() != ".json" && path.extension() != ".ini") {
                            path += ".ini";
                        }
                        if (const auto error = m_project_interactor.export_config(path)) {
                            AppServices::instance().dialog_manager().show_error_dialog(*error, _u8L("Export config"));
                        }
                    };
                    AppServices::instance().dialog_manager().show_file_dialog(
                        FileDialogType::Save,
                        _u8L("Save configuration as:"),
                        this->default_dialog_folder(),
                        "config.ini",
                        Wildcards::generate_wildcards(Wildcards::TypeFlag::Ini | Wildcards::TypeFlag::Json, Wildcards::TypeFlag::Ini),
                        callback
                    );
                }
            )
        )
        // File -> Export -> Export toolpaths as OBJ (preview)
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::ExportToolpathsAsObj},
            std::make_unique<UIItemCommand>(
                "export-toolpaths-as-obj",
                [this]()
                {
                    auto* preview = dynamic_cast<Preview::PreviewRenderModule*>(&m_render_module);
                    if (preview == nullptr || !preview->can_export_toolpaths()) {
                        return;
                    }
                    IDialogManager::FileCallback callback =
                        [preview](const bool success, const std::vector<boost::filesystem::path>& file_paths)
                    {
                        if (success && !file_paths.empty() && !preview->export_toolpaths_to_obj(file_paths.front())) {
                            AppServices::instance().dialog_manager().show_error_dialog(
                                _u8L("Failed to export the toolpaths."), _u8L("Export toolpaths as OBJ")
                            );
                        }
                    };
                    AppServices::instance().dialog_manager().show_file_dialog(
                        FileDialogType::Save,
                        _u8L("Export toolpaths as OBJ"),
                        this->default_dialog_folder(),
                        "toolpaths.obj",
                        Wildcards::generate_wildcards(Wildcards::TypeFlag::Obj, Wildcards::TypeFlag::Obj),
                        callback
                    );
                },
                UIItemCommandExtraOpts{
                    .enabled = [this]()
                    {
                        const auto* preview = dynamic_cast<const Preview::PreviewRenderModule*>(&m_render_module);
                        return preview != nullptr && preview->can_export_toolpaths();
                    }
                }
            )
        )
        // File -> Export -> Export G-code to SD Card / Flash Drive
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::Export, MenuItemName::ExportGcodeToFlash},
            std::make_unique<UIItemCommand>(
                CommandName::ExportGcodeToFlash,
                ExportActions::export_gcode_to_flash(m_project_interactor),
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::U
                    }},
                    .enabled            = [this]()
                    {
                        return ExportActions::can_export(m_project_interactor)
                            && m_project_interactor.removable_drive_service()
                                   .has_removable_drives();
                    }
                }
            )
        );
}

void MenuCommandRegistrar::register_file_menu_commands()
{
    m_menu_manager
        // File -> New Project
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::NewProject},
            std::make_unique<UIItemCommand>(
                CommandName::NewProject,
                [this]() { m_project_interactor.new_project(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::N
                    }}
                }
            )
        )
        // File -> Open Project
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::OpenProject},
            std::make_unique<UIItemCommand>(
                CommandName::OpenProject,
                [this]() { load_project(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::O
                    }}
                }
            )
        )
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::RecentProjects},
            std::make_unique<UIItemCommand>("recent_projects", nullptr, UIItemCommandExtraOpts{})
        )
        // File -> Save Project
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::SaveProject},
            std::make_unique<UIItemCommand>(
                CommandName::SaveProject,
                [this]() { m_project_saver.save_selected_project(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl),
                        Platform::KeyCode::S
                    }}
                }
            )
        )
        // File -> Save Project As
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::SaveProjectAs},
            std::make_unique<UIItemCommand>(
                CommandName::SaveProjectAs,
                [this]() { m_project_saver.save_selected_project_as(); },
                UIItemCommandExtraOpts{
                    .keyboard_shortcuts = Platform::KeyboardShortcuts{Platform::KeyboardShortcut{
                        Platform::KeyModifiers(Platform::KeyModifier::Ctrl)
                            | Platform::KeyModifiers(Platform::KeyModifier::Shift),
                        Platform::KeyCode::S
                    }}
                }
            )
        )
#ifdef SHOW_NOT_IMPLEMENTED_ITEMS
        // Menu -> Separator
        .register_menu_separator_item({MenuItemName::FileMenu})
        // Menu -> Shape Gallery
        .register_menu_item(
            {MenuItemName::FileMenu, MenuItemName::ShapeGallery},
            std::make_unique<UIItemCommand>(
                CommandName::ShapeGallery,
                [this]()
                {
                    // TODO: Implement shape gallery functionality
                }
            )
        )
#endif
        // File -> Separator
        .register_menu_separator_item({MenuItemName::FileMenu});

    register_file_menu_import_commands();
    register_file_menu_export_commands();
}

void MenuCommandRegistrar::update_main_menu_plugin_commands(Lua::PluginSystem& plugin_system)
{
    m_menu_manager.rebuild_submenu(MenuItemName::Plugins, [this, &plugin_system]()
    {
        register_main_menu_plugin_commands(plugin_system);
    });
}

void MenuCommandRegistrar::register_main_menu_plugin_commands(Lua::PluginSystem& plugin_system)
{
    // sort plugins by its menu path
    std::vector<std::tuple<std::string, std::vector<std::string>>> entries;
    std::ranges::transform(
        plugin_system.plugins()
            | std::views::values
            | std::views::filter(
                [](const auto& plugin) { return plugin.meta().type != Lua::PluginType::PrintMode; }
            ),
        std::back_inserter(entries),
        [](const auto& p) -> std::tuple<std::string, std::vector<std::string>>
        { return std::make_tuple(p.meta().id, p.meta().menu); }
    );
    std::ranges::sort(entries, {}, [](const auto& p) { return std::get<1>(p); });
    auto plugin_range =
        entries
        | std::views::transform(
            [&plugin_system](const auto& p) -> const Lua::Plugin&
            { return plugin_system.plugins().at(std::get<0>(p)); }
        );

    // register menu items of plugin entries
    for (auto& plugin : plugin_range) {
        std::vector<UniversalMenuItemName> path;
        if (plugin.meta().menu.empty()) {
            path = {
                MenuItemName::MainMenu,
                MenuItemName::Plugins,
                plugin.meta().id,
            };
        } else {
            path = {
                MenuItemName::MainMenu,
                MenuItemName::Plugins
            };
            for (const auto& menu_item : plugin.meta().menu) {
                path.emplace_back(menu_item);
            }
        }
        m_menu_manager.register_menu_item(
            path,
            std::make_unique<UIItemCommand>(
                std::string(CommandName::PluginExecutePrefix) + plugin.meta().id,
                [this, id = plugin.meta().id, &plugin_system]()
                {
                    m_navigator.navigate_to_module_type(App::Render::ModuleType::Plater);
                    plugin_system.execute_plugin(id);
                }
            )
        );
    }

    // register other plugin related menu items
    if (!entries.empty()) {
        m_menu_manager.register_menu_separator_item({MenuItemName::Plugins});
    }
    m_menu_manager.register_menu_item(
        {MenuItemName::Plugins, MenuItemName::PluginMarketplace},
        std::make_unique<UIItemCommand>(
            CommandName::PluginMarketplace,
            [&]
            {
                Biz::Platform::PlatformServices::instance()
                    .main_thread_dispatcher()
                    .dispatch_on_main_thread(
                        [&]
                        {
                            AppServices::instance().dialog_manager().show_webview_dialog(
                                std::make_unique<Browser::BrowserLogicPluginMarketplace>(
                                    m_project_interactor,
                                    plugin_system
                                ),
                                &m_project_interactor
                            );
                        }
                    );
            }
        )
    );
    m_menu_manager.register_menu_separator_item({MenuItemName::Plugins});
    m_menu_manager.register_menu_item(
        {MenuItemName::Plugins, MenuItemName::PluginRescan},
        std::make_unique<UIItemCommand>(
            CommandName::PluginRescan,
            [&plugin_system]
            {
                // as on Win/Linux this callback is triggered within render loop showing menu items,
                // we are going to recreate, we need to dispatch this action outside render loop
                // to prevent rendering disposed menu items
                Biz::Platform::PlatformServices::instance()
                    .main_thread_dispatcher()
                    .dispatch_on_main_thread([&plugin_system] { plugin_system.rescan(); });
            }
        )
    );
    m_menu_manager.register_menu_item(
        {MenuItemName::Plugins, MenuItemName::PluginInstall},
        std::make_unique<UIItemCommand>(
            CommandName::PluginInstall,
            [&]
            {
                install_plugin(plugin_system);
            }
        )
    );
    m_menu_manager.register_menu_item(
        {MenuItemName::Plugins, MenuItemName::PluginFolder},
        std::make_unique<UIItemCommand>(
            CommandName::PluginFolder,
            [&]
            {
                AppServices::instance().file_explorer_handler().open_folder(data_dir() + "/lua");
            }
        )
    );
}
} // namespace Slic3r::App
