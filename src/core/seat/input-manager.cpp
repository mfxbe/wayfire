#include <cassert>
#include <algorithm>
#include "surface-map-state.hpp"
#include "wayfire/signal-definitions.hpp"
#include "../core-impl.hpp"
#include "../../output/output-impl.hpp"
#include "touch.hpp"
#include "keyboard.hpp"
#include "cursor.hpp"
#include "input-manager.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/workspace-manager.hpp"
#include <wayfire/util/log.hpp>
#include <wayfire/debug.hpp>

#include "switch.hpp"
#include "tablet.hpp"
#include "pointing-device.hpp"

void input_manager::update_capabilities()
{
    uint32_t cap = 0;
    if (pointer_count)
    {
        cap |= WL_SEAT_CAPABILITY_POINTER;
    }

    if (keyboards.size())
    {
        cap |= WL_SEAT_CAPABILITY_KEYBOARD;
    }

    if (touch_count)
    {
        cap |= WL_SEAT_CAPABILITY_TOUCH;
    }

    wlr_seat_set_capabilities(seat, cap);
}

static std::unique_ptr<wf_input_device_internal> create_wf_device_for_device(
    wlr_input_device *device)
{
    switch (device->type)
    {
      case WLR_INPUT_DEVICE_SWITCH:
        return std::make_unique<wf::switch_device_t>(device);

      case WLR_INPUT_DEVICE_POINTER:
        return std::make_unique<wf::pointing_device_t>(device);

      case WLR_INPUT_DEVICE_TABLET_TOOL:
        return std::make_unique<wf::tablet_t>(
            wf::get_core_impl().input->cursor->cursor, device);

      default:
        return std::make_unique<wf_input_device_internal>(device);
    }
}

void input_manager::handle_new_input(wlr_input_device *dev)
{
    LOGI("handle new input: ", dev->name,
        ", default mapping: ", dev->output_name);
    input_devices.push_back(create_wf_device_for_device(dev));

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        keyboards.push_back(std::make_unique<wf_keyboard>(dev));
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->attach_device(dev);
        pointer_count++;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count++;
        // XXX: this should go to cursor as well
        touch->handle_new_device(dev);
    }

    update_capabilities();

    wf::input_device_signal data;
    data.device = nonstd::make_observer(input_devices.back().get());
    wf::get_core().emit_signal("input-device-added", &data);

    refresh_device_mappings();
}

void input_manager::refresh_device_mappings()
{
    // Might trigger motion events which we want to avoid at other stages
    auto state = wf::get_core().get_current_state();
    if (state != wf::compositor_state_t::RUNNING)
    {
        return;
    }

    for (auto& device : this->input_devices)
    {
        wlr_input_device *dev = device->get_wlr_handle();

        auto mapped_output_opt = wf::get_core().config.get_option(
            nonull(dev->name) + std::string("/output"));
        std::string mapped_output = mapped_output_opt ?
            mapped_output_opt->get_value_str() : nonull(dev->output_name);

        auto wo = wf::get_core().output_layout->find_output(mapped_output);
        if (wo)
        {
            wlr_cursor_map_input_to_output(cursor->cursor, dev, wo->handle);
        }
    }
}

void input_manager::handle_input_destroyed(wlr_input_device *dev)
{
    LOGI("remove input: ", dev->name);

    auto it = std::remove_if(input_devices.begin(), input_devices.end(),
        [=] (const std::unique_ptr<wf_input_device_internal>& idev)
    {
        return idev->get_wlr_handle() == dev;
    });

    // devices should be unique
    wf::input_device_signal data;
    data.device = nonstd::make_observer(it->get());
    wf::get_core().emit_signal("input-device-removed", &data);

    input_devices.erase(it, input_devices.end());

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        auto it = std::remove_if(keyboards.begin(), keyboards.end(),
            [=] (const std::unique_ptr<wf_keyboard>& kbd)
        {
            return kbd->device == dev;
        });

        keyboards.erase(it, keyboards.end());
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->detach_device(dev);
        pointer_count--;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count--;
    }

    update_capabilities();
}

void load_locked_mods_from_config(xkb_mod_mask_t& locked_mods)
{
    wf::option_wrapper_t<bool> numlock_state, capslock_state;
    numlock_state.load_option("input/kb_numlock_default_state");
    capslock_state.load_option("input/kb_capslock_default_state");

    if (numlock_state)
    {
        locked_mods |= WF_KB_NUM;
    }

    if (capslock_state)
    {
        locked_mods |= WF_KB_CAPS;
    }
}

input_manager::input_manager()
{
    wf::pointing_device_t::config.load();

    load_locked_mods_from_config(locked_mods);

    input_device_created.set_callback([&] (void *data)
    {
        auto dev = static_cast<wlr_input_device*>(data);
        assert(dev);
        wf::get_core_impl().input->handle_new_input(dev);
    });
    input_device_created.connect(&wf::get_core().backend->events.new_input);

    create_seat();

    config_updated = [=] (wf::signal_data_t*)
    {
        for (auto& dev : input_devices)
        {
            dev->update_options();
        }

        for (auto& kbd : keyboards)
        {
            kbd->reload_input_options();
        }
    };

    wf::get_core().connect_signal("reload-config", &config_updated);

    output_added = [=] (wf::signal_data_t *data)
    {
        auto wo = (wf::output_impl_t*)get_signaled_output(data);
        if (exclusive_client != nullptr)
        {
            wo->inhibit_plugins();
        }

        refresh_device_mappings();
    };
    wf::get_core().output_layout->connect_signal("output-added", &output_added);
}

input_manager::~input_manager()
{
    wf::get_core().disconnect_signal("reload-config", &config_updated);
    wf::get_core().output_layout->disconnect_signal(
        "output-added", &output_added);
}

uint32_t input_manager::get_modifiers()
{
    uint32_t mods = 0;
    auto keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard)
    {
        mods = wlr_keyboard_get_modifiers(keyboard);
    }

    return mods;
}

bool input_manager::grab_input(wf::plugin_grab_interface_t *iface)
{
    if (!iface || !iface->is_grabbed())
    {
        return false;
    }

    assert(!active_grab); // cannot have two active input grabs!
    touch->set_grab(iface);

    active_grab = iface;

    auto kbd  = wlr_seat_get_keyboard(seat);
    auto mods = kbd ? kbd->modifiers : wlr_keyboard_modifiers{0, 0, 0, 0};
    mods.depressed = 0;
    wlr_seat_keyboard_send_modifiers(seat, &mods);

    set_keyboard_focus(nullptr, seat);
    lpointer->set_enable_focus(false);
    wf::get_core().set_cursor("default");

    return true;
}

void input_manager::ungrab_input()
{
    active_grab = nullptr;
    if (wf::get_core().get_active_output())
    {
        wf::get_core().set_active_view(
            wf::get_core().get_active_output()->get_active_view());
    }

    /* We must update cursor focus, however, if we update "too soon", the current
     * pointer event (button press/release, maybe something else) will be sent to
     * the client, which shouldn't happen (at the time of the event, there was
     * still an active input grab) */
    idle_update_cursor.run_once([&] ()
    {
        touch->set_grab(nullptr);
        lpointer->set_enable_focus(true);
    });
}

bool input_manager::input_grabbed()
{
    return active_grab;
}

bool input_manager::can_focus_surface(wf::surface_interface_t *surface)
{
    if (exclusive_client && (surface->get_client() != exclusive_client))
    {
        /* We have exclusive focus surface, for ex. a lockscreen.
         * The only kind of things we can focus are OSKs and similar */
        auto view = (wf::view_interface_t*)surface->get_main_surface();
        if (view && view->get_output())
        {
            auto layer =
                view->get_output()->workspace->get_view_layer(view->self());

            return layer == wf::LAYER_DESKTOP_WIDGET;
        }

        return false;
    }

    return true;
}

wf::surface_interface_t*input_manager::input_surface_at(wf::pointf_t global,
    wf::pointf_t& local)
{
    auto output = wf::get_core().output_layout->get_output_coords_at(global, global);
    /* If the output at these coordinates was just destroyed or some other edge case
     * */
    if (!output)
    {
        return nullptr;
    }

    auto og = output->get_layout_geometry();
    global.x -= og.x;
    global.y -= og.y;

    for (auto& v : output->workspace->get_views_in_layer(wf::VISIBLE_LAYERS))
    {
        for (auto& view : v->enumerate_views())
        {
            if (!view->minimized && can_focus_surface(view.get()))
            {
                auto surface = view->map_input_coordinates(global, local);
                if (surface)
                {
                    return surface;
                }
            }
        }
    }

    return nullptr;
}

void input_manager::set_exclusive_focus(wl_client *client)
{
    exclusive_client = client;
    for (auto& wo : wf::get_core().output_layout->get_outputs())
    {
        auto impl = (wf::output_impl_t*)wo;
        if (client)
        {
            impl->inhibit_plugins();
        } else
        {
            impl->uninhibit_plugins();
        }
    }

    /* We no longer have an exclusively focused client, so we should restore
     * focus to the topmost view */
    if (!client)
    {
        wf::get_core().get_active_output()->refocus(nullptr);
    }
}

wf::bindings_repository_t& input_manager::get_active_bindings()
{
    auto wo   = wf::get_core().get_active_output();
    auto impl = dynamic_cast<wf::output_impl_t*>(wo);
    if (!impl)
    {
        static wf::bindings_repository_t dummy_repo;
        return dummy_repo;
    }

    return impl->get_bindings();
}

wf::SurfaceMapStateListener::SurfaceMapStateListener()
{
    on_surface_map_state_change = [=] (void *data)
    {
        if (this->callback)
        {
            auto ev = static_cast<surface_map_state_changed_signal*>(data);
            this->callback(ev ? ev->surface : nullptr);
        }
    };

    wf::get_core().connect_signal("surface-mapped",
        &on_surface_map_state_change);
    wf::get_core().connect_signal("surface-unmapped",
        &on_surface_map_state_change);
}

wf::SurfaceMapStateListener::~SurfaceMapStateListener()
{
    wf::get_core().disconnect_signal("surface-mapped",
        &on_surface_map_state_change);
    wf::get_core().disconnect_signal("surface-unmapped",
        &on_surface_map_state_change);
}

void wf::SurfaceMapStateListener::set_callback(Callback call)
{
    this->callback = call;
}
