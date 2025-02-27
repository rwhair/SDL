/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_internal.h"

#if SDL_VIDEO_DRIVER_WAYLAND

#include "../SDL_sysvideo.h"
#include "../../events/SDL_windowevents_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../SDL_egl_c.h"
#include "SDL_waylandevents_c.h"
#include "SDL_waylandwindow.h"
#include "SDL_waylandvideo.h"
#include "SDL_waylandtouch.h"
#include "../../SDL_hints_c.h"

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

#ifdef HAVE_LIBDECOR_H
#include <libdecor.h>
#endif


SDL_FORCE_INLINE SDL_bool FloatEqual(float a, float b)
{
    const float diff = SDL_fabsf(a - b);
    const float largest = SDL_max(SDL_fabsf(a), SDL_fabsf(b));

    return diff <= largest * SDL_FLT_EPSILON;
}

static SDL_bool SurfaceScaleIsFractional(SDL_Window *window)
{
    SDL_WindowData *data = window->driverdata;
    const float scale_value = !(window->fullscreen_exclusive) ? data->windowed_scale_factor : window->current_fullscreen_mode.display_scale;
    return !FloatEqual(SDL_roundf(scale_value), scale_value);
}

static SDL_bool WindowNeedsViewport(SDL_Window *window)
{
    SDL_WindowData *wind = window->driverdata;
    SDL_VideoData *video = wind->waylandData;
    SDL_DisplayData *output = SDL_GetDisplayDriverDataForWindow(window);
    const int output_width = wind->requested_window_width ? wind->requested_window_width : output->screen_width;
    const int output_height = wind->requested_window_height ? wind->requested_window_height : output->screen_height;

    /*
     * A viewport is only required when scaling is enabled and:
     *  - The surface scale is fractional.
     *  - A fullscreen mode is being emulated and the mode does not match the logical desktop dimensions.
     */
    if (video->viewporter != NULL) {
        if (SurfaceScaleIsFractional(window)) {
            return SDL_TRUE;
        } else if (window->fullscreen_exclusive) {
            if (window->current_fullscreen_mode.screen_w != output_width || window->current_fullscreen_mode.screen_h != output_height) {
                return SDL_TRUE;
            }
        }
    }

    return SDL_FALSE;
}

static void GetBufferSize(SDL_Window *window, int *width, int *height)
{
    SDL_WindowData *data = window->driverdata;
    int buf_width;
    int buf_height;

    if (window->fullscreen_exclusive) {
        buf_width = window->current_fullscreen_mode.pixel_w;
        buf_height = window->current_fullscreen_mode.pixel_h;
    } else {
        /* Round fractional backbuffer sizes halfway away from zero. */
        buf_width = (int)SDL_lroundf((float)data->requested_window_width * data->windowed_scale_factor);
        buf_height = (int)SDL_lroundf((float)data->requested_window_height * data->windowed_scale_factor);
    }

    if (width) {
        *width = buf_width;
    }
    if (height) {
        *height = buf_height;
    }
}

static void SetDrawSurfaceViewport(SDL_Window *window, int src_width, int src_height, int dst_width, int dst_height)
{
    SDL_WindowData *wind = window->driverdata;
    SDL_VideoData *video = wind->waylandData;

    if (video->viewporter) {
        if (wind->draw_viewport == NULL) {
            wind->draw_viewport = wp_viewporter_get_viewport(video->viewporter, wind->surface);
        }

        wp_viewport_set_source(wind->draw_viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(src_width), wl_fixed_from_int(src_height));
        wp_viewport_set_destination(wind->draw_viewport, dst_width, dst_height);
    }
}

static void UnsetDrawSurfaceViewport(SDL_Window *window)
{
    SDL_WindowData *wind = window->driverdata;

    if (wind->draw_viewport) {
        wp_viewport_destroy(wind->draw_viewport);
        wind->draw_viewport = NULL;
    }
}

static void ConfigureWindowGeometry(SDL_Window *window)
{
    SDL_WindowData *data = window->driverdata;
    SDL_VideoData *viddata = data->waylandData;
    SDL_DisplayData *output = SDL_GetDisplayDriverDataForWindow(window);
    const int old_dw = data->drawable_width;
    const int old_dh = data->drawable_height;
    int window_width, window_height;
    SDL_bool window_size_changed;
    SDL_bool drawable_size_changed;

    /* Set the drawable backbuffer size. */
    GetBufferSize(window, &data->drawable_width, &data->drawable_height);
    drawable_size_changed = data->drawable_width != old_dw || data->drawable_height != old_dh;

    if (data->egl_window && drawable_size_changed) {
        WAYLAND_wl_egl_window_resize(data->egl_window,
                                     data->drawable_width,
                                     data->drawable_height,
                                     0, 0);
    }

    if (window->fullscreen_exclusive) {
        /* If the compositor supplied fullscreen dimensions, use them, otherwise fall back to the display dimensions. */
        const int output_width = data->requested_window_width ? data->requested_window_width : output->screen_width;
        const int output_height = data->requested_window_height ? data->requested_window_height : output->screen_height;
        window_width = window->current_fullscreen_mode.screen_w;
        window_height = window->current_fullscreen_mode.screen_h;

        window_size_changed = window_width != window->w || window_height != window->h ||
            data->wl_window_width != output_width || data->wl_window_height != output_height;

        if (window_size_changed || drawable_size_changed) {
            if (WindowNeedsViewport(window)) {
                /* Set the buffer scale to 1 since a viewport will be used. */
                wl_surface_set_buffer_scale(data->surface, 1);
                SetDrawSurfaceViewport(window, data->drawable_width, data->drawable_height,
                                       output_width, output_height);

                data->wl_window_width = output_width;
                data->wl_window_height = output_height;
            } else {
                /* Always use the mode dimensions for integer scaling. */
                UnsetDrawSurfaceViewport(window);
                wl_surface_set_buffer_scale(data->surface, (int32_t)window->current_fullscreen_mode.display_scale);

                data->wl_window_width = window->current_fullscreen_mode.screen_w;
                data->wl_window_height = window->current_fullscreen_mode.screen_h;
            }

            data->pointer_scale_x = (float)window_width / (float)data->wl_window_width;
            data->pointer_scale_y = (float)window_height / (float)data->wl_window_height;
        }
    } else {
        window_width = data->requested_window_width;
        window_height = data->requested_window_height;

        window_size_changed = window_width != window->w || window_height != window->h;

        if (window_size_changed || drawable_size_changed) {
            if (WindowNeedsViewport(window)) {
                wl_surface_set_buffer_scale(data->surface, 1);
                SetDrawSurfaceViewport(window, data->drawable_width, data->drawable_height,
                                       window_width, window_height);
            } else {
                UnsetDrawSurfaceViewport(window);
                wl_surface_set_buffer_scale(data->surface, (int32_t)data->windowed_scale_factor);
            }

            /* Clamp the physical window size to the system minimum required size. */
            data->wl_window_width = SDL_max(window_width, data->system_min_required_width);
            data->wl_window_height = SDL_max(window_height, data->system_min_required_height);

            data->pointer_scale_x = 1.0f;
            data->pointer_scale_y = 1.0f;
        }
    }

    /*
     * The surface geometry, opaque region and pointer confinement region only
     * need to be recalculated if the output size has changed.
     */
    if (window_size_changed) {
        struct wl_region *region;

        /* libdecor does this internally on frame commits, so it's only needed for xdg surfaces. */
        if (data->shell_surface_type != WAYLAND_SURFACE_LIBDECOR &&
            viddata->shell.xdg && data->shell_surface.xdg.surface != NULL) {
            xdg_surface_set_window_geometry(data->shell_surface.xdg.surface, 0, 0, data->wl_window_width, data->wl_window_height);
        }

        if (!viddata->egl_transparency_enabled) {
            region = wl_compositor_create_region(viddata->compositor);
            wl_region_add(region, 0, 0,
                          data->wl_window_width, data->wl_window_height);
            wl_surface_set_opaque_region(data->surface, region);
            wl_region_destroy(region);
        }

        if (data->confined_pointer) {
            Wayland_input_confine_pointer(viddata->input, window);
        }
    }

    /* Unconditionally send the window and drawable size, the video core will deduplicate when required. */
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, window_width, window_height);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, data->drawable_width, data->drawable_height);
}

static void CommitLibdecorFrame(SDL_Window *window)
{
#ifdef HAVE_LIBDECOR_H
    SDL_WindowData *wind = window->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR && wind->shell_surface.libdecor.frame) {
        struct libdecor_state *state = libdecor_state_new(wind->wl_window_width, wind->wl_window_height);
        libdecor_frame_commit(wind->shell_surface.libdecor.frame, state, NULL);
        libdecor_state_free(state);
    }
#endif
}

static void SetMinMaxDimensions(SDL_Window *window, SDL_bool commit)
{
    SDL_WindowData *wind = window->driverdata;
    SDL_VideoData *viddata = wind->waylandData;
    int min_width, min_height, max_width, max_height;

    /* Pop-ups don't get to change size */
    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        /* ... but we still want to commit, particularly for ShowWindow */
        if (commit) {
            wl_surface_commit(wind->surface);
        }
        return;
    }

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        min_width = 0;
        min_height = 0;
        max_width = 0;
        max_height = 0;
    } else if (window->flags & SDL_WINDOW_RESIZABLE) {
        min_width = window->min_w;
        min_height = window->min_h;
        max_width = window->max_w;
        max_height = window->max_h;
    } else {
        min_width = window->windowed.w;
        min_height = window->windowed.h;
        max_width = window->windowed.w;
        max_height = window->windowed.h;
    }

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        libdecor_frame_set_min_content_size(wind->shell_surface.libdecor.frame,
                                            min_width,
                                            min_height);
        libdecor_frame_set_max_content_size(wind->shell_surface.libdecor.frame,
                                            max_width,
                                            max_height);

        if (commit) {
            CommitLibdecorFrame(window);
            wl_surface_commit(wind->surface);
        }
    } else
#endif
        if (viddata->shell.xdg) {
        if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        xdg_toplevel_set_min_size(wind->shell_surface.xdg.roleobj.toplevel,
                                  min_width,
                                  min_height);
        xdg_toplevel_set_max_size(wind->shell_surface.xdg.roleobj.toplevel,
                                  max_width,
                                  max_height);
        if (commit) {
            wl_surface_commit(wind->surface);
        }
    }
}

static void SetFullscreen(SDL_Window *window, struct wl_output *output)
{
    SDL_WindowData *wind = window->driverdata;
    SDL_VideoData *viddata = wind->waylandData;

    /* The desktop may try to enforce min/max sizes here, so turn them off for
     * fullscreen and on (if applicable) for windowed
     */
    SetMinMaxDimensions(window, SDL_FALSE);

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        if (output) {
            if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
                /* Ensure that window is resizable before going into fullscreen.
                 * This triggers a frame commit internally, so a separate one is not necessary.
                 */
                libdecor_frame_set_capabilities(wind->shell_surface.libdecor.frame, LIBDECOR_ACTION_RESIZE);
                wl_surface_commit(wind->surface);
            } else {
                CommitLibdecorFrame(window);
                wl_surface_commit(wind->surface);
            }

            libdecor_frame_set_fullscreen(wind->shell_surface.libdecor.frame, output);
        } else {
            libdecor_frame_unset_fullscreen(wind->shell_surface.libdecor.frame);

            if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
                /* restore previous RESIZE capability */
                libdecor_frame_unset_capabilities(wind->shell_surface.libdecor.frame, LIBDECOR_ACTION_RESIZE);
                wl_surface_commit(wind->surface);
            } else {
                CommitLibdecorFrame(window);
                wl_surface_commit(wind->surface);
            }
        }
    } else
#endif
        if (viddata->shell.xdg) {
        if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }

        wl_surface_commit(wind->surface);

        if (output) {
            xdg_toplevel_set_fullscreen(wind->shell_surface.xdg.roleobj.toplevel, output);
        } else {
            xdg_toplevel_unset_fullscreen(wind->shell_surface.xdg.roleobj.toplevel);
        }
    }
}

static void UpdateWindowFullscreen(SDL_Window *window, SDL_bool fullscreen)
{
    SDL_WindowData *wind = window->driverdata;

    if (fullscreen) {
        if (!(window->flags & SDL_WINDOW_FULLSCREEN)) {
            wind->is_fullscreen = SDL_TRUE;

            wind->in_fullscreen_transition = SDL_TRUE;
            SDL_SetWindowFullscreen(window, SDL_TRUE);
            wind->in_fullscreen_transition = SDL_FALSE;
        }
    } else {
        /* Don't change the fullscreen flags if the window is hidden or being hidden. */
        if ((window->flags & SDL_WINDOW_FULLSCREEN) && !window->is_hiding && !(window->flags & SDL_WINDOW_HIDDEN)) {
            wind->is_fullscreen = SDL_FALSE;

            wind->in_fullscreen_transition = SDL_TRUE;
            SDL_SetWindowFullscreen(window, SDL_FALSE);
            wind->in_fullscreen_transition = SDL_FALSE;

            SetMinMaxDimensions(window, SDL_FALSE);
        }
    }
}

static const struct wl_callback_listener surface_damage_frame_listener;

static void surface_damage_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    SDL_WindowData *wind = (SDL_WindowData *)data;

    /*
     * wl_surface.damage_buffer is the preferred method of setting the damage region
     * on compositor version 4 and above.
     */
    if (wl_compositor_get_version(wind->waylandData->compositor) >= 4) {
        wl_surface_damage_buffer(wind->surface, 0, 0,
                                 wind->drawable_width, wind->drawable_height);
    } else {
        wl_surface_damage(wind->surface, 0, 0,
                          wind->wl_window_width, wind->wl_window_height);
    }

    wl_callback_destroy(cb);
    wind->surface_damage_frame_callback = wl_surface_frame(wind->surface);
    wl_callback_add_listener(wind->surface_damage_frame_callback, &surface_damage_frame_listener, data);
}

static const struct wl_callback_listener surface_damage_frame_listener = {
    surface_damage_frame_done
};

static const struct wl_callback_listener gles_swap_frame_listener;

static void gles_swap_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    SDL_WindowData *wind = (SDL_WindowData *)data;
    SDL_AtomicSet(&wind->swap_interval_ready, 1); /* mark window as ready to present again. */

    /* reset this callback to fire again once a new frame was presented and compositor wants the next one. */
    wind->gles_swap_frame_callback = wl_surface_frame(wind->gles_swap_frame_surface_wrapper);
    wl_callback_destroy(cb);
    wl_callback_add_listener(wind->gles_swap_frame_callback, &gles_swap_frame_listener, data);
}

static const struct wl_callback_listener gles_swap_frame_listener = {
    gles_swap_frame_done
};

static void handle_configure_xdg_shell_surface(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    SDL_WindowData *wind = (SDL_WindowData *)data;
    SDL_Window *window = wind->sdlwindow;

    ConfigureWindowGeometry(window);
    xdg_surface_ack_configure(xdg, serial);

    wind->shell_surface.xdg.initial_configure_seen = SDL_TRUE;
}

static const struct xdg_surface_listener shell_surface_listener_xdg = {
    handle_configure_xdg_shell_surface
};

static void handle_configure_xdg_toplevel(void *data,
                                          struct xdg_toplevel *xdg_toplevel,
                                          int32_t width,
                                          int32_t height,
                                          struct wl_array *states)
{
    SDL_WindowData *wind = (SDL_WindowData *)data;
    SDL_Window *window = wind->sdlwindow;

    enum xdg_toplevel_state *state;
    SDL_bool fullscreen = SDL_FALSE;
    SDL_bool maximized = SDL_FALSE;
    SDL_bool floating = SDL_TRUE;
    SDL_bool focused = SDL_FALSE;
    wl_array_for_each (state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            fullscreen = SDL_TRUE;
            floating = SDL_FALSE;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            maximized = SDL_TRUE;
            floating = SDL_FALSE;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            focused = SDL_TRUE;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            floating = SDL_FALSE;
            break;
        default:
            break;
        }
    }

    UpdateWindowFullscreen(window, fullscreen);

    if (!fullscreen) {
        /* xdg_toplevel spec states that this is a suggestion.
         * Ignore if less than or greater than max/min size.
         */
        if (window->flags & SDL_WINDOW_RESIZABLE) {
            if ((floating && !wind->floating) || width == 0 || height == 0) {
                /* This happens when we're being restored from a
                 * non-floating state, so use the cached floating size here.
                 */
                width = wind->floating_width;
                height = wind->floating_height;
            }

            if (window->max_w > 0) {
                width = SDL_min(width, window->max_w);
            }
            width = SDL_max(width, window->min_w);

            if (window->max_h > 0) {
                height = SDL_min(height, window->max_h);
            }
            height = SDL_max(height, window->min_h);
        } else if (floating) {
            /* If we're a fixed-size window, we know our size for sure.
             * Always assume the configure is wrong.
             */
            width = window->windowed.w;
            height = window->windowed.h;
        }

        /* Store current floating dimensions for restoring */
        if (floating) {
            wind->floating_width = width;
            wind->floating_height = height;
        }

        /* Always send a maximized/restore event; if the event is redundant it will
         * automatically be discarded (see src/events/SDL_windowevents.c)
         *
         * No, we do not get minimize events from xdg-shell.
         */
        SDL_SendWindowEvent(window,
                            maximized ? SDL_EVENT_WINDOW_MAXIMIZED : SDL_EVENT_WINDOW_RESTORED,
                            0, 0);
    } else {
        /* Unconditionally set the output for exclusive fullscreen windows when entering
         * fullscreen from a compositor event, as where the compositor will actually
         * place the fullscreen window is unknown.
         */
        if (window->fullscreen_exclusive && !wind->fullscreen_was_positioned) {
            SDL_VideoDisplay *disp = SDL_GetVideoDisplay(window->current_fullscreen_mode.displayID);
            if (disp) {
                wind->fullscreen_was_positioned = SDL_TRUE;
                xdg_toplevel_set_fullscreen(xdg_toplevel, disp->driverdata->output);
            }
        }
    }

    /* Similar to maximized/restore events above, send focus events too! */
    SDL_SendWindowEvent(window,
                        focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST,
                        0, 0);

    wind->requested_window_width = width;
    wind->requested_window_height = height;
    wind->floating = floating;
}

static void handle_close_xdg_toplevel(void *data, struct xdg_toplevel *xdg_toplevel)
{
    SDL_WindowData *window = (SDL_WindowData *)data;
    SDL_SendWindowEvent(window->sdlwindow, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

static const struct xdg_toplevel_listener toplevel_listener_xdg = {
    handle_configure_xdg_toplevel,
    handle_close_xdg_toplevel
};

static void handle_configure_xdg_popup(void *data,
                                       struct xdg_popup *xdg_popup,
                                       int32_t x,
                                       int32_t y,
                                       int32_t width,
                                       int32_t height)
{
    /* No-op, we don't use x/y and width/height are fixed-size */
}

static void handle_done_xdg_popup(void *data, struct xdg_popup *xdg_popup)
{
    SDL_WindowData *window = (SDL_WindowData *)data;
    SDL_SendWindowEvent(window->sdlwindow, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

static void handle_repositioned_xdg_popup(void *data,
                                          struct xdg_popup *xdg_popup,
                                          uint32_t token)
{
    /* No-op, configure does all the work we care about */
}

static const struct xdg_popup_listener popup_listener_xdg = {
    handle_configure_xdg_popup,
    handle_done_xdg_popup,
    handle_repositioned_xdg_popup
};

#define TOOLTIP_CURSOR_OFFSET 8 /* FIXME: Arbitrary, eyeballed from X tooltip */

static int Wayland_PopupWatch(void *data, SDL_Event *event)
{
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Window *window = (SDL_Window *)data;
        SDL_WindowData *wind = window->driverdata;

        /* Coordinates might be relative to the popup, which we don't want */
        if (event->motion.windowID == wind->shell_surface.xdg.roleobj.popup.parentID) {
            xdg_positioner_set_offset(wind->shell_surface.xdg.roleobj.popup.positioner,
                                      event->motion.x + TOOLTIP_CURSOR_OFFSET,
                                      event->motion.y + TOOLTIP_CURSOR_OFFSET);
            xdg_popup_reposition(wind->shell_surface.xdg.roleobj.popup.popup,
                                 wind->shell_surface.xdg.roleobj.popup.positioner,
                                 0);
        }
    }
    return 1;
}

static void handle_configure_zxdg_decoration(void *data,
                                             struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
                                             uint32_t mode)
{
    SDL_Window *window = (SDL_Window *)data;
    SDL_WindowData *driverdata = window->driverdata;
    SDL_VideoDevice *device = SDL_GetVideoDevice();

    /* If the compositor tries to force CSD anyway, bail on direct XDG support
     * and fall back to libdecor, it will handle these events from then on.
     *
     * To do this we have to fully unmap, then map with libdecor loaded.
     */
    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) {
        if (window->flags & SDL_WINDOW_BORDERLESS) {
            /* borderless windows do request CSD, so we got what we wanted */
            return;
        }
        if (!Wayland_LoadLibdecor(driverdata->waylandData, SDL_TRUE)) {
            /* libdecor isn't available, so no borders for you... oh well */
            return;
        }
        WAYLAND_wl_display_roundtrip(driverdata->waylandData->display);

        Wayland_HideWindow(device, window);
        driverdata->shell_surface_type = WAYLAND_SURFACE_LIBDECOR;

        if (!window->is_hiding && !(window->flags & SDL_WINDOW_HIDDEN)) {
            Wayland_ShowWindow(device, window);
        }
    }
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    handle_configure_zxdg_decoration
};

#ifdef HAVE_LIBDECOR_H
/*
 * XXX: Hack for older versions of libdecor that lack the function to query the
 *      minimum content size limit. The internal limits must always be overridden
 *      to ensure that very small windows don't cause errors or crashes.
 *
 *      On versions of libdecor that expose the function to get the minimum content
 *      size limit, this function is a no-op.
 *
 *      Can be removed if the minimum required version of libdecor is raised
 *      to a version that guarantees the availability of this function.
 */
static void OverrideLibdecorLimits(SDL_Window *window)
{
#if defined(SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR)
    if (libdecor_frame_get_min_content_size == NULL) {
        SetMinMaxDimensions(window, SDL_FALSE);
    }
#elif !defined(SDL_HAVE_LIBDECOR_GET_MIN_MAX)
    SetMinMaxDimensions(window, SDL_FALSE);
#endif
}

/*
 * NOTE: Retrieves the minimum content size limits, if the function for doing so is available.
 *       On versions of libdecor that lack the minimum content size retrieval function, this
 *       function is a no-op.
 *
 *       Can be replaced with a direct call if the minimum required version of libdecor is raised
 *       to a version that guarantees the availability of this function.
 */
static void LibdecorGetMinContentSize(struct libdecor_frame *frame, int *min_w, int *min_h)
{
#if defined(SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR)
    if (libdecor_frame_get_min_content_size != NULL) {
        libdecor_frame_get_min_content_size(frame, min_w, min_h);
    }
#elif defined(SDL_HAVE_LIBDECOR_GET_MIN_MAX)
    libdecor_frame_get_min_content_size(frame, min_w, min_h);
#endif
}

static void decoration_frame_configure(struct libdecor_frame *frame,
                                       struct libdecor_configuration *configuration,
                                       void *user_data)
{
    SDL_WindowData *wind = (SDL_WindowData *)user_data;
    SDL_Window *window = wind->sdlwindow;
    struct libdecor_state *state;

    enum libdecor_window_state window_state;
    int width, height;

    SDL_bool focused = SDL_FALSE;
    SDL_bool fullscreen = SDL_FALSE;
    SDL_bool maximized = SDL_FALSE;
    SDL_bool tiled = SDL_FALSE;
    SDL_bool floating;

    static const enum libdecor_window_state tiled_states = (LIBDECOR_WINDOW_STATE_TILED_LEFT | LIBDECOR_WINDOW_STATE_TILED_RIGHT |
                                                            LIBDECOR_WINDOW_STATE_TILED_TOP | LIBDECOR_WINDOW_STATE_TILED_BOTTOM);

    /* Window State */
    if (libdecor_configuration_get_window_state(configuration, &window_state)) {
        fullscreen = (window_state & LIBDECOR_WINDOW_STATE_FULLSCREEN) != 0;
        maximized = (window_state & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0;
        focused = (window_state & LIBDECOR_WINDOW_STATE_ACTIVE) != 0;
        tiled = (window_state & tiled_states) != 0;
    }
    floating = !(fullscreen || maximized || tiled);

    UpdateWindowFullscreen(window, fullscreen);

    if (!fullscreen) {
        /* Always send a maximized/restore event; if the event is redundant it will
         * automatically be discarded (see src/events/SDL_windowevents.c)
         *
         * No, we do not get minimize events from libdecor.
         */
        SDL_SendWindowEvent(window,
                            maximized ? SDL_EVENT_WINDOW_MAXIMIZED : SDL_EVENT_WINDOW_RESTORED,
                            0, 0);
    }

    /* Similar to maximized/restore events above, send focus events too! */
    SDL_SendWindowEvent(window,
                        focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST,
                        0, 0);

    /* For fullscreen or fixed-size windows we know our size.
     * Always assume the configure is wrong.
     */
    if (fullscreen) {
        /* Unconditionally set the output for exclusive fullscreen windows when entering
         * fullscreen from a compositor event, as where the compositor will actually
         * place the fullscreen window is unknown.
         */
        if (window->fullscreen_exclusive && !wind->fullscreen_was_positioned) {
            SDL_VideoDisplay *disp = SDL_GetVideoDisplay(window->current_fullscreen_mode.displayID);
            if (disp) {
                wind->fullscreen_was_positioned = SDL_TRUE;
                libdecor_frame_set_fullscreen(frame, disp->driverdata->output);
            }
        }

        /* FIXME: We have been explicitly told to respect the fullscreen size
         * parameters here, even though they are known to be wrong on GNOME at
         * bare minimum. If this is wrong, don't blame us, we were explicitly
         * told to do this.
         */
        if (!libdecor_configuration_get_content_size(configuration, frame, &width, &height)) {
            width = 0;
            height = 0;
        }
    } else if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
        width = window->windowed.w;
        height = window->windowed.h;

        OverrideLibdecorLimits(window);
    } else {
        /*
         * XXX: libdecor can send bogus content sizes that are +/- the height
         *      of the title bar when hiding a window or transitioning from
         *      non-floating to floating state, which distorts the window size.
         *
         *      Ignore any size values from libdecor in these scenarios in
         *      favor of the cached window size.
         *
         *      https://gitlab.gnome.org/jadahl/libdecor/-/issues/40
         */
        const SDL_bool use_cached_size = (floating && !wind->floating) ||
                                         (window->is_hiding || !!(window->flags & SDL_WINDOW_HIDDEN));

        /* This will never set 0 for width/height unless the function returns false */
        if (use_cached_size || !libdecor_configuration_get_content_size(configuration, frame, &width, &height)) {
            if (floating) {
                /* This usually happens when we're being restored from a
                 * non-floating state, so use the cached floating size here.
                 */
                width = wind->floating_width;
                height = wind->floating_height;
            } else {
                width = window->w;
                height = window->h;
            }
        }
    }

    /* Store current floating dimensions for restoring */
    if (floating) {
        wind->floating_width = width;
        wind->floating_height = height;
    }

    /* Store the new floating state. */
    wind->floating = floating;

    /* Calculate the new window geometry */
    wind->requested_window_width = width;
    wind->requested_window_height = height;
    ConfigureWindowGeometry(window);

    /* ... then commit the changes on the libdecor side. */
    state = libdecor_state_new(wind->wl_window_width, wind->wl_window_height);
    libdecor_frame_commit(frame, state, configuration);
    libdecor_state_free(state);

    if (!wind->shell_surface.libdecor.initial_configure_seen) {
        LibdecorGetMinContentSize(frame, &wind->system_min_required_width, &wind->system_min_required_height);
        wind->shell_surface.libdecor.initial_configure_seen = SDL_TRUE;
    }

    /* Update the resize capability. Since this will change the capabilities and
     * commit a new frame state with the last known content dimension, this has
     * to be called after the new state has been committed and the new content
     * dimensions were updated.
     */
    Wayland_SetWindowResizable(SDL_GetVideoDevice(), window,
                               window->flags & SDL_WINDOW_RESIZABLE);
}

static void decoration_frame_close(struct libdecor_frame *frame, void *user_data)
{
    SDL_SendWindowEvent(((SDL_WindowData *)user_data)->sdlwindow, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

static void decoration_frame_commit(struct libdecor_frame *frame, void *user_data)
{
    SDL_WindowData *wind = user_data;

    SDL_SendWindowEvent(wind->sdlwindow, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
}

static struct libdecor_frame_interface libdecor_frame_interface = {
    decoration_frame_configure,
    decoration_frame_close,
    decoration_frame_commit,
};
#endif

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
static void handle_onscreen_visibility(void *data,
                                       struct qt_extended_surface *qt_extended_surface, int32_t visible)
{
}

static void handle_set_generic_property(void *data,
                                        struct qt_extended_surface *qt_extended_surface, const char *name,
                                        struct wl_array *value)
{
}

static void handle_close(void *data, struct qt_extended_surface *qt_extended_surface)
{
    SDL_WindowData *window = (SDL_WindowData *)data;
    SDL_SendWindowEvent(window->sdlwindow, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

static const struct qt_extended_surface_listener extended_surface_listener = {
    handle_onscreen_visibility,
    handle_set_generic_property,
    handle_close,
};
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */

static void update_scale_factor(SDL_WindowData *window)
{
    float old_factor = window->windowed_scale_factor;
    float new_factor;
    int i;

    if (!(window->sdlwindow->flags & SDL_WINDOW_ALLOW_HIGHDPI)) {
        /* Scale will always be 1, just ignore this */
        return;
    }

    if (window->num_outputs != 0) {
        /* Check every display's factor, use the highest */
        new_factor = 0.0f;
        for (i = 0; i < window->num_outputs; i++) {
            SDL_DisplayData *driverdata = window->outputs[i];
            new_factor = SDL_max(new_factor, driverdata->scale_factor);
        }
    } else {
        /* No monitor (somehow)? Just fall back. */
        new_factor = old_factor;
    }

    if (!FloatEqual(new_factor, old_factor)) {
        window->windowed_scale_factor = new_factor;
        ConfigureWindowGeometry(window->sdlwindow);
    }
}

/* While we can't get window position from the compositor, we do at least know
 * what monitor we're on, so let's send move events that put the window at the
 * center of the whatever display the wl_surface_listener events give us.
 */
static void Wayland_move_window(SDL_Window *window, SDL_DisplayData *driverdata)
{
    SDL_DisplayID *displays;
    int i;

    displays = SDL_GetDisplays(NULL);
    if (displays) {
        for (i = 0; displays[i]; ++i) {
            if (SDL_GetDisplayDriverData(displays[i]) == driverdata) {
                /* We want to send a very very specific combination here:
                 *
                 * 1. A coordinate that tells the application what display we're on
                 * 2. Exactly (0, 0)
                 *
                 * Part 1 is useful information but is also really important for
                 * ensuring we end up on the right display for fullscreen, while
                 * part 2 is important because numerous applications use a specific
                 * combination of GetWindowPosition and GetGlobalMouseState, and of
                 * course neither are supported by Wayland. Since global mouse will
                 * fall back to just GetMouseState, we need the window position to
                 * be zero so the cursor math works without it going off in some
                 * random direction. See UE5 Editor for a notable example of this!
                 *
                 * This may be an issue some day if we're ever able to implement
                 * SDL_GetDisplayUsableBounds!
                 *
                 * -flibit
                 */
                SDL_Rect bounds;
                SDL_GetDisplayBounds(displays[i], &bounds);

                window->driverdata->last_displayID = displays[i];
                SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_MOVED, bounds.x, bounds.y);
                break;
            }
        }
        SDL_free(displays);
    }
}

static void handle_surface_enter(void *data, struct wl_surface *surface,
                                 struct wl_output *output)
{
    SDL_WindowData *window = data;
    SDL_DisplayData *driverdata = wl_output_get_user_data(output);

    if (!SDL_WAYLAND_own_output(output) || !SDL_WAYLAND_own_surface(surface)) {
        return;
    }

    window->outputs = SDL_realloc(window->outputs,
                                  sizeof(SDL_DisplayData *) * (window->num_outputs + 1));
    window->outputs[window->num_outputs++] = driverdata;

    /* Update the scale factor after the move so that fullscreen outputs are updated. */
    Wayland_move_window(window->sdlwindow, driverdata);

    if (!window->fractional_scale) {
        update_scale_factor(window);
    }
}

static void handle_surface_leave(void *data, struct wl_surface *surface,
                                 struct wl_output *output)
{
    SDL_WindowData *window = data;
    int i, send_move_event = 0;
    SDL_DisplayData *driverdata = wl_output_get_user_data(output);

    if (!SDL_WAYLAND_own_output(output) || !SDL_WAYLAND_own_surface(surface)) {
        return;
    }

    for (i = 0; i < window->num_outputs; i++) {
        if (window->outputs[i] == driverdata) { /* remove this one */
            if (i == (window->num_outputs - 1)) {
                window->outputs[i] = NULL;
                send_move_event = 1;
            } else {
                SDL_memmove(&window->outputs[i],
                            &window->outputs[i + 1],
                            sizeof(SDL_DisplayData *) * ((window->num_outputs - i) - 1));
            }
            window->num_outputs--;
            i--;
        }
    }

    if (window->num_outputs == 0) {
        SDL_free(window->outputs);
        window->outputs = NULL;
    } else if (send_move_event) {
        Wayland_move_window(window->sdlwindow,
                            window->outputs[window->num_outputs - 1]);
    }

    if (!window->fractional_scale) {
        update_scale_factor(window);
    }
}

static const struct wl_surface_listener surface_listener = {
    handle_surface_enter,
    handle_surface_leave
};

int Wayland_GetWindowWMInfo(_THIS, SDL_Window *window, SDL_SysWMinfo *info)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *data = window->driverdata;

    info->subsystem = SDL_SYSWM_WAYLAND;
    info->info.wl.display = data->waylandData->display;
    info->info.wl.surface = data->surface;
    info->info.wl.egl_window = data->egl_window;

#ifdef HAVE_LIBDECOR_H
    if (data->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (data->shell_surface.libdecor.frame != NULL) {
            info->info.wl.xdg_surface = libdecor_frame_get_xdg_surface(data->shell_surface.libdecor.frame);
            info->info.wl.xdg_toplevel = libdecor_frame_get_xdg_toplevel(data->shell_surface.libdecor.frame);
        }
    } else
#endif
        if (viddata->shell.xdg && data->shell_surface.xdg.surface != NULL) {
        SDL_bool popup = (data->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) ? SDL_TRUE : SDL_FALSE;
        info->info.wl.xdg_surface = data->shell_surface.xdg.surface;
        info->info.wl.xdg_toplevel = popup ? NULL : data->shell_surface.xdg.roleobj.toplevel;
        if (popup) {
            info->info.wl.xdg_popup = data->shell_surface.xdg.roleobj.popup.popup;
            info->info.wl.xdg_positioner = data->shell_surface.xdg.roleobj.popup.positioner;
        }
    }

    return 0;
}

int Wayland_SetWindowHitTest(SDL_Window *window, SDL_bool enabled)
{
    return 0; /* just succeed, the real work is done elsewhere. */
}

int Wayland_SetWindowModalFor(_THIS, SDL_Window *modal_window, SDL_Window *parent_window)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *modal_data = modal_window->driverdata;
    SDL_WindowData *parent_data = parent_window->driverdata;

    if (modal_data->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP || parent_data->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return SDL_SetError("Modal/Parent was a popup, not a toplevel");
    }

#ifdef HAVE_LIBDECOR_H
    if (viddata->shell.libdecor) {
        if (modal_data->shell_surface.libdecor.frame == NULL) {
            return SDL_SetError("Modal window was hidden");
        }
        if (parent_data->shell_surface.libdecor.frame == NULL) {
            return SDL_SetError("Parent window was hidden");
        }
        libdecor_frame_set_parent(modal_data->shell_surface.libdecor.frame,
                                  parent_data->shell_surface.libdecor.frame);
    } else
#endif
        if (viddata->shell.xdg) {
        if (modal_data->shell_surface.xdg.roleobj.toplevel == NULL) {
            return SDL_SetError("Modal window was hidden");
        }
        if (parent_data->shell_surface.xdg.roleobj.toplevel == NULL) {
            return SDL_SetError("Parent window was hidden");
        }
        xdg_toplevel_set_parent(modal_data->shell_surface.xdg.roleobj.toplevel,
                                parent_data->shell_surface.xdg.roleobj.toplevel);
    } else {
        return SDL_Unsupported();
    }

    WAYLAND_wl_display_flush(viddata->display);
    return 0;
}

void Wayland_ShowWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *c = _this->driverdata;
    SDL_WindowData *data = window->driverdata;

    /* Detach any previous buffers before resetting everything, otherwise when
     * calling this a second time you'll get an annoying protocol error!
     *
     * FIXME: This was originally moved to HideWindow, which _should_ make
     * sense, but for whatever reason UE5's popups require that this actually
     * be in both places at once? Possibly from renderers making commits? I can't
     * fully remember if this location caused crashes or if I was fixing a pair
     * of Hide/Show calls. In any case, UE gives us a pretty good test and having
     * both detach calls passes. This bug may be relevant if I'm wrong:
     *
     * https://bugs.kde.org/show_bug.cgi?id=448856
     *
     * -flibit
     */
    wl_surface_attach(data->surface, NULL, 0, 0);
    wl_surface_commit(data->surface);

    /* Create the shell surface and map the toplevel/popup */
#ifdef HAVE_LIBDECOR_H
    if (data->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (data->shell_surface.libdecor.frame) {
            /* If the frame already exists, just set the visibility. */
            libdecor_frame_set_visibility(data->shell_surface.libdecor.frame, true);
            libdecor_frame_set_app_id(data->shell_surface.libdecor.frame, c->classname);
        } else {
            data->shell_surface.libdecor.frame = libdecor_decorate(c->shell.libdecor,
                                                                   data->surface,
                                                                   &libdecor_frame_interface,
                                                                   data);
            if (data->shell_surface.libdecor.frame == NULL) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to create libdecor frame!");
            } else {
                libdecor_frame_set_app_id(data->shell_surface.libdecor.frame, c->classname);
                libdecor_frame_map(data->shell_surface.libdecor.frame);
            }
        }
    } else
#endif
        if (c->shell.xdg) {
        data->shell_surface.xdg.surface = xdg_wm_base_get_xdg_surface(c->shell.xdg, data->surface);
        xdg_surface_set_user_data(data->shell_surface.xdg.surface, data);
        xdg_surface_add_listener(data->shell_surface.xdg.surface, &shell_surface_listener_xdg, data);

        if (data->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
            SDL_Mouse *mouse = SDL_GetMouse();
            SDL_Window *focused = SDL_GetMouseFocus();
            SDL_WindowData *focuseddata = focused->driverdata;

            /* This popup may be a child of another popup! */
            data->shell_surface.xdg.roleobj.popup.parentID = SDL_GetWindowID(focused);
            data->shell_surface.xdg.roleobj.popup.child = NULL;
            if (focuseddata->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
                SDL_assert(focuseddata->shell_surface.xdg.roleobj.popup.child == NULL);
                focuseddata->shell_surface.xdg.roleobj.popup.child = window;
            }

            /* Set up the positioner for the popup */
            data->shell_surface.xdg.roleobj.popup.positioner = xdg_wm_base_create_positioner(c->shell.xdg);
            xdg_positioner_set_offset(data->shell_surface.xdg.roleobj.popup.positioner,
                                      mouse->x + TOOLTIP_CURSOR_OFFSET,
                                      mouse->y + TOOLTIP_CURSOR_OFFSET);

            /* Assign the popup role */
            data->shell_surface.xdg.roleobj.popup.popup = xdg_surface_get_popup(data->shell_surface.xdg.surface,
                                                                                focuseddata->shell_surface.xdg.surface,
                                                                                data->shell_surface.xdg.roleobj.popup.positioner);
            xdg_popup_add_listener(data->shell_surface.xdg.roleobj.popup.popup, &popup_listener_xdg, data);

            /* For tooltips, track mouse motion so it follows the cursor */
            if (window->flags & SDL_WINDOW_TOOLTIP) {
                if (xdg_popup_get_version(data->shell_surface.xdg.roleobj.popup.popup) >= 3) {
                    SDL_AddEventWatch(Wayland_PopupWatch, window);
                }
            }
        } else {
            data->shell_surface.xdg.roleobj.toplevel = xdg_surface_get_toplevel(data->shell_surface.xdg.surface);
            xdg_toplevel_set_app_id(data->shell_surface.xdg.roleobj.toplevel, c->classname);
            xdg_toplevel_add_listener(data->shell_surface.xdg.roleobj.toplevel, &toplevel_listener_xdg, data);
        }
    }

    /* Restore state that was set prior to this call */
    Wayland_SetWindowTitle(_this, window);
    if (window->flags & SDL_WINDOW_MAXIMIZED) {
        Wayland_MaximizeWindow(_this, window);
    }
    if (window->flags & SDL_WINDOW_MINIMIZED) {
        Wayland_MinimizeWindow(_this, window);
    }

    /* We have to wait until the surface gets a "configure" event, or use of
     * this surface will fail. This is a new rule for xdg_shell.
     */
#ifdef HAVE_LIBDECOR_H
    if (data->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (data->shell_surface.libdecor.frame) {
            while (!data->shell_surface.libdecor.initial_configure_seen) {
                WAYLAND_wl_display_flush(c->display);
                WAYLAND_wl_display_dispatch(c->display);
            }
        }
    } else
#endif
        if (c->shell.xdg) {
        /* Unlike libdecor we need to call this explicitly to prevent a deadlock.
         * libdecor will call this as part of their configure event!
         * -flibit
         */
        wl_surface_commit(data->surface);
        if (data->shell_surface.xdg.surface) {
            while (!data->shell_surface.xdg.initial_configure_seen) {
                WAYLAND_wl_display_flush(c->display);
                WAYLAND_wl_display_dispatch(c->display);
            }
        }

        /* Create the window decorations */
        if (data->shell_surface_type != WAYLAND_SURFACE_XDG_POPUP && data->shell_surface.xdg.roleobj.toplevel && c->decoration_manager) {
            data->server_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(c->decoration_manager, data->shell_surface.xdg.roleobj.toplevel);
            zxdg_toplevel_decoration_v1_add_listener(data->server_decoration,
                                                     &decoration_listener,
                                                     window);
        }

        /* Set the geometry */
        xdg_surface_set_window_geometry(data->shell_surface.xdg.surface, 0, 0, data->wl_window_width, data->wl_window_height);
    } else {
        /* Nothing to see here, just commit. */
        wl_surface_commit(data->surface);
    }

    /* Unlike the rest of window state we have to set this _after_ flushing the
     * display, because we need to create the decorations before possibly hiding
     * them immediately afterward.
     */
#ifdef HAVE_LIBDECOR_H
    if (data->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        /* ... but don't call it redundantly for libdecor, the decorator
         * may not interpret a redundant call nicely and cause weird stuff to happen
         */
        if (data->shell_surface.libdecor.frame && window->flags & SDL_WINDOW_BORDERLESS) {
            Wayland_SetWindowBordered(_this, window, SDL_FALSE);
        }

        /* Libdecor plugins can enforce minimum window sizes, so adjust if the initial window size is too small. */
        if (window->windowed.w < data->system_min_required_width ||
            window->windowed.h < data->system_min_required_height) {

            /* Warn if the window frame will be larger than the content surface. */
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO,
                        "Window dimensions (%i, %i) are smaller than the system enforced minimum (%i, %i); window borders will be larger than the content surface.",
                        window->windowed.w, window->windowed.h, data->system_min_required_width, data->system_min_required_height);

            data->wl_window_width = SDL_max(window->windowed.w, data->system_min_required_width);
            data->wl_window_height = SDL_max(window->windowed.h, data->system_min_required_height);
            CommitLibdecorFrame(window);
        }
    } else
#endif
    {
        Wayland_SetWindowBordered(_this, window, !(window->flags & SDL_WINDOW_BORDERLESS));
    }

    /* We're finally done putting the window together, raise if possible */
    if (c->activation_manager) {
        /* Note that we don't check for empty strings, as that is still
         * considered a valid activation token!
         */
        const char *activation_token = SDL_getenv("XDG_ACTIVATION_TOKEN");
        if (activation_token) {
            xdg_activation_v1_activate(c->activation_manager,
                                       activation_token,
                                       data->surface);

            /* Clear this variable, per the protocol's request */
            unsetenv("XDG_ACTIVATION_TOKEN");
        }
    }

    /*
     * Roundtrip required to avoid a possible protocol violation when
     * HideWindow was called immediately before ShowWindow.
     */
    WAYLAND_wl_display_roundtrip(c->display);
}

static void Wayland_ReleasePopup(_THIS, SDL_Window *popup)
{
    SDL_WindowData *popupdata;

    /* Basic sanity checks to weed out the weird popup closures */
    if (popup == NULL || popup->magic != &_this->window_magic) {
        return;
    }
    popupdata = popup->driverdata;
    if (popupdata == NULL) {
        return;
    }

    /* This may already be freed by a parent popup! */
    if (popupdata->shell_surface.xdg.roleobj.popup.popup == NULL) {
        return;
    }

    /* Release the child _first_, otherwise a protocol error triggers */
    if (popupdata->shell_surface.xdg.roleobj.popup.child != NULL) {
        Wayland_ReleasePopup(_this, popupdata->shell_surface.xdg.roleobj.popup.child);
        popupdata->shell_surface.xdg.roleobj.popup.child = NULL;
    }

    if (popup->flags & SDL_WINDOW_TOOLTIP) {
        if (xdg_popup_get_version(popupdata->shell_surface.xdg.roleobj.popup.popup) >= 3) {
            SDL_DelEventWatch(Wayland_PopupWatch, popup);
        }
    }
    xdg_popup_destroy(popupdata->shell_surface.xdg.roleobj.popup.popup);
    xdg_positioner_destroy(popupdata->shell_surface.xdg.roleobj.popup.positioner);
    popupdata->shell_surface.xdg.roleobj.popup.popup = NULL;
    popupdata->shell_surface.xdg.roleobj.popup.positioner = NULL;
}

void Wayland_HideWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *data = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;

    if (wind->server_decoration) {
        zxdg_toplevel_decoration_v1_destroy(wind->server_decoration);
        wind->server_decoration = NULL;
    }

    /* Be sure to detach after this is done, otherwise ShowWindow crashes! */
    wl_surface_attach(wind->surface, NULL, 0, 0);
    wl_surface_commit(wind->surface);

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame) {
            libdecor_frame_set_visibility(wind->shell_surface.libdecor.frame, false);
            libdecor_frame_set_app_id(wind->shell_surface.libdecor.frame, data->classname);
        }
    } else
#endif
        if (data->shell.xdg) {
        if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
            Wayland_ReleasePopup(_this, window);
        } else if (wind->shell_surface.xdg.roleobj.toplevel) {
            xdg_toplevel_destroy(wind->shell_surface.xdg.roleobj.toplevel);
            wind->shell_surface.xdg.roleobj.toplevel = NULL;
        }
        if (wind->shell_surface.xdg.surface) {
            xdg_surface_destroy(wind->shell_surface.xdg.surface);
            wind->shell_surface.xdg.surface = NULL;
        }
    }

    /*
     * Roundtrip required to avoid a possible protocol violation when
     * ShowWindow is called immediately after HideWindow.
     */
    WAYLAND_wl_display_roundtrip(data->display);
}

static void handle_xdg_activation_done(void *data,
                                       struct xdg_activation_token_v1 *xdg_activation_token_v1,
                                       const char *token)
{
    SDL_WindowData *window = data;
    if (xdg_activation_token_v1 == window->activation_token) {
        xdg_activation_v1_activate(window->waylandData->activation_manager,
                                   token,
                                   window->surface);
        xdg_activation_token_v1_destroy(window->activation_token);
        window->activation_token = NULL;
    }
}

static const struct xdg_activation_token_v1_listener activation_listener_xdg = {
    handle_xdg_activation_done
};

/* The xdg-activation protocol considers "activation" to be one of two things:
 *
 * 1: Raising a window to the top and flashing the titlebar
 * 2: Flashing the titlebar while keeping the window where it is
 *
 * As you might expect from Wayland, the general policy is to go with #2 unless
 * the client can prove to the compositor beyond a reasonable doubt that raising
 * the window will not be malicuous behavior.
 *
 * For SDL this means RaiseWindow and FlashWindow both use the same protocol,
 * but in different ways: RaiseWindow will provide as _much_ information as
 * possible while FlashWindow will provide as _little_ information as possible,
 * to nudge the compositor into doing what we want.
 *
 * This isn't _strictly_ what the protocol says will happen, but this is what
 * current implementations are doing (as of writing, YMMV in the far distant
 * future).
 *
 * -flibit
 */
static void Wayland_activate_window(SDL_VideoData *data, SDL_WindowData *wind,
                                    struct wl_surface *surface,
                                    uint32_t serial, struct wl_seat *seat)
{
    if (data->activation_manager) {
        if (wind->activation_token != NULL) {
            /* We're about to overwrite this with a new request */
            xdg_activation_token_v1_destroy(wind->activation_token);
        }

        wind->activation_token = xdg_activation_v1_get_activation_token(data->activation_manager);
        xdg_activation_token_v1_add_listener(wind->activation_token,
                                             &activation_listener_xdg,
                                             wind);

        /* Note that we are not setting the app_id or serial here.
         *
         * Hypothetically we could set the app_id from data->classname, but
         * that part of the API is for _external_ programs, not ourselves.
         *
         * -flibit
         */
        if (surface != NULL) {
            xdg_activation_token_v1_set_surface(wind->activation_token, surface);
        }
        if (seat != NULL) {
            xdg_activation_token_v1_set_serial(wind->activation_token, serial, seat);
        }
        xdg_activation_token_v1_commit(wind->activation_token);
    }
}

void Wayland_RaiseWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *wind = window->driverdata;

    /* FIXME: This Raise event is arbitrary and doesn't come from an event, so
     * it's actually very likely that this token will be ignored! Maybe add
     * support for passing serials (and the associated seat) so this can have
     * a better chance of actually raising the window.
     * -flibit
     */
    Wayland_activate_window(_this->driverdata,
                            wind,
                            wind->surface,
                            0,
                            NULL);
}

int Wayland_FlashWindow(_THIS, SDL_Window *window, SDL_FlashOperation operation)
{
    Wayland_activate_window(_this->driverdata,
                            window->driverdata,
                            NULL,
                            0,
                            NULL);
    return 0;
}

static void handle_preferred_scale_changed(void *data,
                                    struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
                                    uint preferred_scale)
{
    SDL_WindowData *window = data;
    float old_factor = window->windowed_scale_factor;
    float new_factor = preferred_scale / 120.; /* 120 is a magic number defined in the spec as a common denominator*/

    if (!(window->sdlwindow->flags & SDL_WINDOW_ALLOW_HIGHDPI)) {
        /* Scale will always be 1, just ignore this */
        return;
    }

    if (!FloatEqual(new_factor, old_factor)) {
        window->windowed_scale_factor = new_factor;
        ConfigureWindowGeometry(window->sdlwindow);
    }
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    handle_preferred_scale_changed
};

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
static void SDLCALL QtExtendedSurface_OnHintChanged(void *userdata, const char *name,
                                                    const char *oldValue, const char *newValue)
{
    struct qt_extended_surface *qt_extended_surface = userdata;
    int i;

    static struct
    {
        const char *name;
        int32_t value;
    } orientations[] = {
        { "portrait", QT_EXTENDED_SURFACE_ORIENTATION_PRIMARYORIENTATION },
        { "landscape", QT_EXTENDED_SURFACE_ORIENTATION_LANDSCAPEORIENTATION },
        { "inverted-portrait", QT_EXTENDED_SURFACE_ORIENTATION_INVERTEDPORTRAITORIENTATION },
        { "inverted-landscape", QT_EXTENDED_SURFACE_ORIENTATION_INVERTEDLANDSCAPEORIENTATION }
    };

    if (name == NULL) {
        return;
    }

    if (SDL_strcmp(name, SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION) == 0) {
        int32_t orientation = QT_EXTENDED_SURFACE_ORIENTATION_PRIMARYORIENTATION;

        if (newValue != NULL) {
            const char *value_attempt = newValue;
            while (value_attempt != NULL && *value_attempt != 0) {
                const char *value_attempt_end = SDL_strchr(value_attempt, ',');
                size_t value_attempt_len = (value_attempt_end != NULL) ? (value_attempt_end - value_attempt)
                                                                       : SDL_strlen(value_attempt);

                for (i = 0; i < SDL_arraysize(orientations); i += 1) {
                    if ((value_attempt_len == SDL_strlen(orientations[i].name)) &&
                        (SDL_strncasecmp(orientations[i].name, value_attempt, value_attempt_len) == 0)) {
                        orientation |= orientations[i].value;
                        break;
                    }
                }

                value_attempt = (value_attempt_end != NULL) ? (value_attempt_end + 1) : NULL;
            }
        }

        qt_extended_surface_set_content_orientation(qt_extended_surface, orientation);
    } else if (SDL_strcmp(name, SDL_HINT_QTWAYLAND_WINDOW_FLAGS) == 0) {
        uint32_t flags = 0;

        if (newValue != NULL) {
            char *tmp = SDL_strdup(newValue);
            char *saveptr = NULL;

            char *flag = SDL_strtokr(tmp, " ", &saveptr);
            while (flag) {
                if (SDL_strcmp(flag, "OverridesSystemGestures") == 0) {
                    flags |= QT_EXTENDED_SURFACE_WINDOWFLAG_OVERRIDESSYSTEMGESTURES;
                } else if (SDL_strcmp(flag, "StaysOnTop") == 0) {
                    flags |= QT_EXTENDED_SURFACE_WINDOWFLAG_STAYSONTOP;
                } else if (SDL_strcmp(flag, "BypassWindowManager") == 0) {
                    // See https://github.com/qtproject/qtwayland/commit/fb4267103d
                    flags |= 4 /* QT_EXTENDED_SURFACE_WINDOWFLAG_BYPASSWINDOWMANAGER */;
                }

                flag = SDL_strtokr(NULL, " ", &saveptr);
            }

            SDL_free(tmp);
        }

        qt_extended_surface_set_window_flags(qt_extended_surface, flags);
    }
}

static void QtExtendedSurface_Subscribe(struct qt_extended_surface *surface, const char *name)
{
    SDL_AddHintCallback(name, QtExtendedSurface_OnHintChanged, surface);
}

static void QtExtendedSurface_Unsubscribe(struct qt_extended_surface *surface, const char *name)
{
    SDL_DelHintCallback(name, QtExtendedSurface_OnHintChanged, surface);
}
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */

void Wayland_SetWindowFullscreen(_THIS, SDL_Window *window,
                                 SDL_VideoDisplay *display, SDL_bool fullscreen)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;
    struct wl_output *output = display->driverdata->output;

    /* Called from within a configure event or the window is a popup, drop it. */
    if (wind->in_fullscreen_transition || wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        if (!fullscreen) {
            /* Clear the fullscreen positioned flag. */
            wind->fullscreen_was_positioned = SDL_FALSE;
        }
        return;
    }

    /* Don't send redundant fullscreen set/unset events. */
    if (wind->is_fullscreen != fullscreen) {
        wind->is_fullscreen = fullscreen;
        wind->fullscreen_was_positioned = fullscreen ? SDL_TRUE : SDL_FALSE;
        SetFullscreen(window, fullscreen ? output : NULL);

        /* Roundtrip required to receive the updated window dimensions */
        WAYLAND_wl_display_roundtrip(viddata->display);
    } else if (wind->is_fullscreen) {
        /*
         * If the window is already fullscreen, this is likely a request to switch between
         * fullscreen and fullscreen desktop, change outputs, or change the video mode.
         *
         * If the window is already positioned on the target output, just update the
         * window geometry.
         */
        if (wind->last_displayID != display->id) {
            wind->fullscreen_was_positioned = SDL_TRUE;
            SetFullscreen(window, output);
        } else {
            ConfigureWindowGeometry(window);
            CommitLibdecorFrame(window);
        }

        /* Roundtrip required to receive the updated window dimensions */
        WAYLAND_wl_display_roundtrip(viddata->display);
    }
}

void Wayland_RestoreWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return;
    }

    /* Set this flag now even if we never actually maximized, eventually
     * ShowWindow will take care of it along with the other window state.
     */
    window->flags &= ~SDL_WINDOW_MAXIMIZED;

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        libdecor_frame_unset_maximized(wind->shell_surface.libdecor.frame);
    } else
#endif
        /* Note that xdg-shell does NOT provide a way to unset minimize! */
        if (viddata->shell.xdg) {
            if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
                return; /* Can't do anything yet, wait for ShowWindow */
            }
            xdg_toplevel_unset_maximized(wind->shell_surface.xdg.roleobj.toplevel);
        }

    WAYLAND_wl_display_roundtrip(viddata->display);
}

void Wayland_SetWindowBordered(_THIS, SDL_Window *window, SDL_bool bordered)
{
    SDL_WindowData *wind = window->driverdata;
    const SDL_VideoData *viddata = (const SDL_VideoData *)_this->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return;
    }

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame) {
            libdecor_frame_set_visibility(wind->shell_surface.libdecor.frame, bordered);
        }
    } else
#endif
        if ((viddata->decoration_manager) && (wind->server_decoration)) {
        const enum zxdg_toplevel_decoration_v1_mode mode = bordered ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
        zxdg_toplevel_decoration_v1_set_mode(wind->server_decoration, mode);
    }
}

void Wayland_SetWindowResizable(_THIS, SDL_Window *window, SDL_bool resizable)
{
#ifdef HAVE_LIBDECOR_H
    const SDL_WindowData *wind = window->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        if (resizable) {
            libdecor_frame_set_capabilities(wind->shell_surface.libdecor.frame, LIBDECOR_ACTION_RESIZE);
        } else {
            libdecor_frame_unset_capabilities(wind->shell_surface.libdecor.frame, LIBDECOR_ACTION_RESIZE);
        }
    } else
#endif
    {
        SetMinMaxDimensions(window, SDL_TRUE);
    }
}

void Wayland_MaximizeWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return;
    }

    if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
        return;
    }

    /* Set this flag now even if we don't actually maximize yet, eventually
     * ShowWindow will take care of it along with the other window state.
     */
    window->flags |= SDL_WINDOW_MAXIMIZED;

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        libdecor_frame_set_maximized(wind->shell_surface.libdecor.frame);
    } else
#endif
        if (viddata->shell.xdg) {
        if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        xdg_toplevel_set_maximized(wind->shell_surface.xdg.roleobj.toplevel);
    }

    WAYLAND_wl_display_roundtrip(viddata->display);
}

void Wayland_MinimizeWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *viddata = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;

    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return;
    }

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        libdecor_frame_set_minimized(wind->shell_surface.libdecor.frame);
    } else
#endif
        if (viddata->shell.xdg) {
        if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        xdg_toplevel_set_minimized(wind->shell_surface.xdg.roleobj.toplevel);
    }

    WAYLAND_wl_display_flush(viddata->display);
}

void Wayland_SetWindowMouseRect(_THIS, SDL_Window *window)
{
    SDL_VideoData *data = _this->driverdata;

    /* This may look suspiciously like SetWindowGrab, despite SetMouseRect not
     * implicitly doing a grab. And you're right! Wayland doesn't let us mess
     * around with mouse focus whatsoever, so it just happens to be that the
     * work that we can do in these two functions ends up being the same.
     *
     * Just know that this call lets you confine with a rect, SetWindowGrab
     * lets you confine without a rect.
     */
    if (SDL_RectEmpty(&window->mouse_rect) && !(window->flags & SDL_WINDOW_MOUSE_GRABBED)) {
        Wayland_input_unconfine_pointer(data->input, window);
    } else {
        Wayland_input_confine_pointer(data->input, window);
    }
}

void Wayland_SetWindowMouseGrab(_THIS, SDL_Window *window, SDL_bool grabbed)
{
    SDL_VideoData *data = _this->driverdata;

    if (grabbed) {
        Wayland_input_confine_pointer(data->input, window);
    } else if (SDL_RectEmpty(&window->mouse_rect)) {
        Wayland_input_unconfine_pointer(data->input, window);
    }
}

void Wayland_SetWindowKeyboardGrab(_THIS, SDL_Window *window, SDL_bool grabbed)
{
    SDL_VideoData *data = _this->driverdata;

    if (grabbed) {
        Wayland_input_grab_keyboard(window, data->input);
    } else {
        Wayland_input_ungrab_keyboard(window);
    }
}

int Wayland_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data;
    SDL_VideoData *c;

    data = SDL_calloc(1, sizeof(*data));
    if (data == NULL) {
        return SDL_OutOfMemory();
    }

    c = _this->driverdata;
    window->driverdata = data;

    if (!(window->flags & SDL_WINDOW_VULKAN)) {
        if (!(window->flags & SDL_WINDOW_OPENGL)) {
            SDL_GL_LoadLibrary(NULL);
            window->flags |= SDL_WINDOW_OPENGL;
        }
    }

    if (window->x == SDL_WINDOWPOS_UNDEFINED) {
        window->x = 0;
    }
    if (window->y == SDL_WINDOWPOS_UNDEFINED) {
        window->y = 0;
    }

    data->waylandData = c;
    data->sdlwindow = window;

    data->windowed_scale_factor = 1.0f;

    if (window->flags & SDL_WINDOW_ALLOW_HIGHDPI) {
        int i;
        for (i = 0; i < _this->num_displays; i++) {
            float scale = _this->displays[i].driverdata->scale_factor;
            data->windowed_scale_factor = SDL_max(data->windowed_scale_factor, scale);
        }
    }

    data->outputs = NULL;
    data->num_outputs = 0;

    data->requested_window_width = window->w;
    data->requested_window_height = window->h;
    data->floating_width = window->windowed.w;
    data->floating_height = window->windowed.h;

    data->surface =
        wl_compositor_create_surface(c->compositor);
    wl_surface_add_listener(data->surface, &surface_listener, data);

    SDL_WAYLAND_register_surface(data->surface);

    /* Must be called before EGL configuration to set the drawable backbuffer size. */
    ConfigureWindowGeometry(window);

    /* Fire a callback when the compositor wants a new frame rendered.
     * Right now this only matters for OpenGL; we use this callback to add a
     * wait timeout that avoids getting deadlocked by the compositor when the
     * window isn't visible.
     */
    if (window->flags & SDL_WINDOW_OPENGL) {
        data->gles_swap_frame_event_queue = WAYLAND_wl_display_create_queue(data->waylandData->display);
        data->gles_swap_frame_surface_wrapper = WAYLAND_wl_proxy_create_wrapper(data->surface);
        WAYLAND_wl_proxy_set_queue((struct wl_proxy *)data->gles_swap_frame_surface_wrapper, data->gles_swap_frame_event_queue);
        data->gles_swap_frame_callback = wl_surface_frame(data->gles_swap_frame_surface_wrapper);
        wl_callback_add_listener(data->gles_swap_frame_callback, &gles_swap_frame_listener, data);
    }

    /* Fire a callback when the compositor wants a new frame to set the surface damage region. */
    data->surface_damage_frame_callback = wl_surface_frame(data->surface);
    wl_callback_add_listener(data->surface_damage_frame_callback, &surface_damage_frame_listener, data);

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
    if (c->surface_extension) {
        data->extended_surface = qt_surface_extension_get_extended_surface(
            c->surface_extension, data->surface);

        QtExtendedSurface_Subscribe(data->extended_surface, SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION);
        QtExtendedSurface_Subscribe(data->extended_surface, SDL_HINT_QTWAYLAND_WINDOW_FLAGS);
    }
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */

    if (window->flags & SDL_WINDOW_OPENGL) {
        data->egl_window = WAYLAND_wl_egl_window_create(data->surface, data->drawable_width, data->drawable_height);

#if SDL_VIDEO_OPENGL_EGL
        /* Create the GLES window surface */
        data->egl_surface = SDL_EGL_CreateSurface(_this, (NativeWindowType)data->egl_window);

        if (data->egl_surface == EGL_NO_SURFACE) {
            return -1; /* SDL_EGL_CreateSurface should have set error */
        }
#endif
    }

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
    if (data->extended_surface) {
        qt_extended_surface_set_user_data(data->extended_surface, data);
        qt_extended_surface_add_listener(data->extended_surface,
                                         &extended_surface_listener, data);
    }
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */

    if (c->relative_mouse_mode) {
        Wayland_input_lock_pointer(c->input);
    }

    if (c->fractional_scale_manager) {
        data->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(c->fractional_scale_manager, data->surface);
        wp_fractional_scale_v1_add_listener(data->fractional_scale,
                                            &fractional_scale_listener, data);
    }

    /* Moved this call to ShowWindow: wl_surface_commit(data->surface); */
    WAYLAND_wl_display_flush(c->display);

    /* We may need to create an idle inhibitor for this new window */
    Wayland_SuspendScreenSaver(_this);

#define IS_POPUP(window) \
    (window->flags & (SDL_WINDOW_TOOLTIP | SDL_WINDOW_POPUP_MENU))
#ifdef HAVE_LIBDECOR_H
    if (c->shell.libdecor && !IS_POPUP(window)) {
        data->shell_surface_type = WAYLAND_SURFACE_LIBDECOR;
    } else
#endif
        if (c->shell.xdg) {
        if (IS_POPUP(window)) {
            data->shell_surface_type = WAYLAND_SURFACE_XDG_POPUP;
        } else {
            data->shell_surface_type = WAYLAND_SURFACE_XDG_TOPLEVEL;
        }
    } /* All other cases will be WAYLAND_SURFACE_UNKNOWN */
#undef IS_POPUP

    return 0;
}

void Wayland_SetWindowMinimumSize(_THIS, SDL_Window *window)
{
    SetMinMaxDimensions(window, SDL_TRUE);
}

void Wayland_SetWindowMaximumSize(_THIS, SDL_Window *window)
{
    SetMinMaxDimensions(window, SDL_TRUE);
}

void Wayland_SetWindowSize(_THIS, SDL_Window *window)
{
    SDL_WindowData *wind = window->driverdata;

    /*
     * Unconditionally store the floating size, as it will need
     * to be applied when returning from a non-floating state.
     */
    wind->floating_width = window->windowed.w;
    wind->floating_height = window->windowed.h;

    /* Don't change the size of static (non-floating) windows. */
    if (wind->floating) {
        wind->requested_window_width = window->windowed.w;
        wind->requested_window_height = window->windowed.h;

        ConfigureWindowGeometry(window);
        CommitLibdecorFrame(window);
    }
}

void Wayland_GetWindowSizeInPixels(_THIS, SDL_Window *window, int *w, int *h)
{
    SDL_WindowData *data;
    if (window->driverdata) {
        data = window->driverdata;
        *w = data->drawable_width;
        *h = data->drawable_height;
    }
}

void Wayland_SetWindowTitle(_THIS, SDL_Window *window)
{
    SDL_WindowData *wind = window->driverdata;
    SDL_VideoData *viddata = _this->driverdata;
    const char *title = window->title ? window->title : "";

    if (wind->shell_surface_type == WAYLAND_SURFACE_XDG_POPUP) {
        return;
    }

#ifdef HAVE_LIBDECOR_H
    if (wind->shell_surface_type == WAYLAND_SURFACE_LIBDECOR) {
        if (wind->shell_surface.libdecor.frame == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        libdecor_frame_set_title(wind->shell_surface.libdecor.frame, title);
    } else
#endif
        if (viddata->shell.xdg) {
        if (wind->shell_surface.xdg.roleobj.toplevel == NULL) {
            return; /* Can't do anything yet, wait for ShowWindow */
        }
        xdg_toplevel_set_title(wind->shell_surface.xdg.roleobj.toplevel, title);
    }

    WAYLAND_wl_display_flush(viddata->display);
}

int Wayland_SuspendScreenSaver(_THIS)
{
    SDL_VideoData *data = _this->driverdata;

#if SDL_USE_LIBDBUS
    if (SDL_DBus_ScreensaverInhibit(_this->suspend_screensaver)) {
        return 0;
    }
#endif

    /* The idle_inhibit_unstable_v1 protocol suspends the screensaver
       on a per wl_surface basis, but SDL assumes that suspending
       the screensaver can be done independently of any window.

       To reconcile these differences, we propagate the idle inhibit
       state to each window. If there is no window active, we will
       be able to inhibit idle once the first window is created.
    */
    if (data->idle_inhibit_manager) {
        SDL_Window *window = _this->windows;
        while (window) {
            SDL_WindowData *win_data = window->driverdata;

            if (_this->suspend_screensaver && !win_data->idle_inhibitor) {
                win_data->idle_inhibitor =
                    zwp_idle_inhibit_manager_v1_create_inhibitor(data->idle_inhibit_manager,
                                                                 win_data->surface);
            } else if (!_this->suspend_screensaver && win_data->idle_inhibitor) {
                zwp_idle_inhibitor_v1_destroy(win_data->idle_inhibitor);
                win_data->idle_inhibitor = NULL;
            }

            window = window->next;
        }
    }

    return 0;
}

void Wayland_DestroyWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *data = _this->driverdata;
    SDL_WindowData *wind = window->driverdata;

    if (data) {
#if SDL_VIDEO_OPENGL_EGL
        if (wind->egl_surface) {
            SDL_EGL_DestroySurface(_this, wind->egl_surface);
        }
#endif
        if (wind->egl_window) {
            WAYLAND_wl_egl_window_destroy(wind->egl_window);
        }

        if (wind->idle_inhibitor) {
            zwp_idle_inhibitor_v1_destroy(wind->idle_inhibitor);
        }

        if (wind->activation_token) {
            xdg_activation_token_v1_destroy(wind->activation_token);
        }

        if (wind->draw_viewport) {
            wp_viewport_destroy(wind->draw_viewport);
        }

        if (wind->fractional_scale) {
            wp_fractional_scale_v1_destroy(wind->fractional_scale);
        }

        SDL_free(wind->outputs);

        if (wind->gles_swap_frame_callback) {
            WAYLAND_wl_event_queue_destroy(wind->gles_swap_frame_event_queue);
            WAYLAND_wl_proxy_wrapper_destroy(wind->gles_swap_frame_surface_wrapper);
            wl_callback_destroy(wind->gles_swap_frame_callback);
        }

        if (wind->surface_damage_frame_callback) {
            wl_callback_destroy(wind->surface_damage_frame_callback);
        }

#ifdef SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH
        if (wind->extended_surface) {
            QtExtendedSurface_Unsubscribe(wind->extended_surface, SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION);
            QtExtendedSurface_Unsubscribe(wind->extended_surface, SDL_HINT_QTWAYLAND_WINDOW_FLAGS);
            qt_extended_surface_destroy(wind->extended_surface);
        }
#endif /* SDL_VIDEO_DRIVER_WAYLAND_QT_TOUCH */
        wl_surface_destroy(wind->surface);

        SDL_free(wind);
        WAYLAND_wl_display_flush(data->display);
    }
    window->driverdata = NULL;
}

static void EGLTransparencyChangedCallback(void *userdata, const char *name, const char *oldValue, const char *newValue)
{
    const SDL_bool oldval = SDL_GetStringBoolean(oldValue, SDL_FALSE);
    const SDL_bool newval = SDL_GetStringBoolean(newValue, SDL_FALSE);

    if (oldval != newval) {
        SDL_Window *window;
        SDL_VideoData *viddata = (SDL_VideoData *)userdata;
        SDL_VideoDevice *dev = SDL_GetVideoDevice();

        viddata->egl_transparency_enabled = newval;

        /* Iterate over all windows and update the surface opaque regions */
        for (window = dev->windows; window != NULL; window = window->next) {
            SDL_WindowData *wind = window->driverdata;

            if (!newval) {
                struct wl_region *region = wl_compositor_create_region(wind->waylandData->compositor);
                wl_region_add(region, 0, 0, wind->wl_window_width, wind->wl_window_height);
                wl_surface_set_opaque_region(wind->surface, region);
                wl_region_destroy(region);
            } else {
                wl_surface_set_opaque_region(wind->surface, NULL);
            }
        }
    }
}

void Wayland_InitWin(SDL_VideoData *data)
{
    data->egl_transparency_enabled = SDL_GetHintBoolean(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, SDL_FALSE);
    SDL_AddHintCallback(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, EGLTransparencyChangedCallback, data);
}

void Wayland_QuitWin(SDL_VideoData *data)
{
    SDL_DelHintCallback(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, EGLTransparencyChangedCallback, data);
}

#endif /* SDL_VIDEO_DRIVER_WAYLAND */
