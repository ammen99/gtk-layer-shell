#include "layer-surface.h"

#include "gtk-layer-shell.h"
#include "simple-conversions.h"
#include "custom-shell-surface.h"
#include "gtk-wayland.h"

#include "wlr-layer-shell-unstable-v1-client.h"
#include "xdg-shell-client.h"

#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

struct _LayerSurface
{
    CustomShellSurface super;

    // Can be set at any time
    gboolean anchors[GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER];
    int margins[GTK_LAYER_SHELL_LAYER_ENTRY_NUMBER];
    int exclusive_zone;
    gboolean auto_exclusive_zone; // if to automatically change the exclusive zone to match the window size
    GtkRequisition current_allocation; // Last size allocation, or (-1, -1) if there hasn't been one
    GtkRequisition cached_layer_size; // Last size sent to zwlr_layer_surface_v1_set_size, or (-1, -1) if never called
    gboolean keyboard_interactivity;

    // Need the surface to be recreated to change
    GdkMonitor *monitor;
    enum zwlr_layer_shell_v1_layer layer;
    const char* name_space; // can be null, freed on destruction

    // The actual layer surface Wayland object (can be NULL)
    struct zwlr_layer_surface_v1 *layer_surface;
};

static GtkRequisition
layer_surface_get_gtk_window_size (LayerSurface *self)
{
    if (self->current_allocation.width >= 0 && self->current_allocation.height >= 0) {
        return self->current_allocation;
    } else {
        GtkWindow *gtk_window = custom_shell_surface_get_gtk_window ((CustomShellSurface *)self);
        GtkRequisition natural_size;
        gtk_widget_get_preferred_size (GTK_WIDGET (gtk_window), NULL, &natural_size);
        return natural_size;
    }
}

static void
layer_surface_handle_configure (void *data,
                                struct zwlr_layer_surface_v1 *surface,
                                uint32_t serial,
                                uint32_t w,
                                uint32_t h)
{
    LayerSurface *self = data;

    if (w > 0 || h > 0) {
        GtkRequisition requested = {
            .width = w,
            .height = h,
        };
        GtkRequisition current_size = layer_surface_get_gtk_window_size (self);
        if (requested.width == 0)
            requested.width = current_size.width;
        if (requested.height == 0)
            requested.height = current_size.height;
        GtkWindow *gtk_window = custom_shell_surface_get_gtk_window ((CustomShellSurface *)self);
        gtk_window_resize (gtk_window, requested.width, requested.height);
    }
    zwlr_layer_surface_v1_ack_configure (surface, serial);
}

static void
layer_surface_handle_closed (void *data,
                             struct zwlr_layer_surface_v1 *_surface)
{
    LayerSurface *self = data;
    (void)_surface;

    GtkWindow *gtk_window = custom_shell_surface_get_gtk_window ((CustomShellSurface *)self);
    gtk_window_close (gtk_window);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void
layer_surface_send_set_anchor (LayerSurface *self)
{
    if (self->layer_surface) {
        uint32_t wlr_anchor = gtk_layer_shell_edge_array_get_zwlr_layer_shell_v1_anchor (self->anchors);
        zwlr_layer_surface_v1_set_anchor (self->layer_surface, wlr_anchor);
    }
}

static void
layer_surface_send_set_margin (LayerSurface *self)
{
    if (self->layer_surface) {
        zwlr_layer_surface_v1_set_margin (self->layer_surface,
                                          self->margins[GTK_LAYER_SHELL_EDGE_TOP],
                                          self->margins[GTK_LAYER_SHELL_EDGE_RIGHT],
                                          self->margins[GTK_LAYER_SHELL_EDGE_BOTTOM],
                                          self->margins[GTK_LAYER_SHELL_EDGE_LEFT]);
    }
}

static void
layer_surface_map (CustomShellSurface *super, struct wl_surface *wl_surface)
{
    LayerSurface *self = (LayerSurface *)super;

    g_return_if_fail (!self->layer_surface);

    struct zwlr_layer_shell_v1 *layer_shell_global = gtk_wayland_get_layer_shell_global ();
    g_return_if_fail (layer_shell_global);

    const char *name_space = self->name_space;
    if (name_space == NULL)
        name_space = "gtk-layer-shell";

    struct wl_output *output = NULL;
    if (self->monitor) {
        output = gdk_wayland_monitor_get_wl_output (self->monitor);
    }

    self->layer_surface = zwlr_layer_shell_v1_get_layer_surface (layer_shell_global,
                                                                 wl_surface,
                                                                 output,
                                                                 self->layer,
                                                                 name_space);
    g_return_if_fail (self->layer_surface);

    zwlr_layer_surface_v1_set_keyboard_interactivity (self->layer_surface, self->keyboard_interactivity);
    zwlr_layer_surface_v1_set_exclusive_zone (self->layer_surface, self->exclusive_zone);
    layer_surface_send_set_anchor (self);
    layer_surface_send_set_margin (self);
    if (self->cached_layer_size.width >= 0 && self->cached_layer_size.height >= 0) {
        zwlr_layer_surface_v1_set_size (self->layer_surface,
                                        self->cached_layer_size.width,
                                        self->cached_layer_size.height);
    }
    zwlr_layer_surface_v1_add_listener (self->layer_surface, &layer_surface_listener, self);
}

static void
layer_surface_unmap (CustomShellSurface *super)
{
    LayerSurface *self = (LayerSurface *)super;

    if (self->layer_surface) {
        zwlr_layer_surface_v1_destroy (self->layer_surface);
        self->layer_surface = NULL;
    }
}

static void
layer_surface_finalize (CustomShellSurface *super)
{
    LayerSurface *self = (LayerSurface *)super;
    layer_surface_unmap (super);
    g_free ((gpointer)self->name_space);
}

static struct xdg_popup *
layer_surface_get_popup (CustomShellSurface *super,
                         struct xdg_surface *popup_xdg_surface,
                         struct xdg_positioner *positioner)
{
    LayerSurface *self = (LayerSurface *)super;

    if (!self->layer_surface) {
        g_critical ("layer_surface_get_popup () called when the layer surface wayland object has not yet been created");
        return NULL;
    }

    struct xdg_popup *xdg_popup = xdg_surface_get_popup (popup_xdg_surface, NULL, positioner);
    zwlr_layer_surface_v1_get_popup (self->layer_surface, xdg_popup);
    return xdg_popup;
}

static GdkRectangle
layer_surface_get_logical_geom (CustomShellSurface *super)
{
    (void)super;
    return (GdkRectangle){0, 0, 0, 0};
}

static const CustomShellSurfaceVirtual layer_surface_virtual = {
    .map = layer_surface_map,
    .unmap = layer_surface_unmap,
    .finalize = layer_surface_finalize,
    .get_popup = layer_surface_get_popup,
    .get_logical_geom = layer_surface_get_logical_geom,
};

static void
layer_surface_update_size (LayerSurface *self)
{
    GtkWindow *gtk_window = custom_shell_surface_get_gtk_window ((CustomShellSurface *)self);

    if (self->auto_exclusive_zone) {
        gboolean horiz = (self->anchors[GTK_LAYER_SHELL_EDGE_LEFT] ==
                          self->anchors[GTK_LAYER_SHELL_EDGE_RIGHT]);
        gboolean vert = (self->anchors[GTK_LAYER_SHELL_EDGE_TOP] ==
                         self->anchors[GTK_LAYER_SHELL_EDGE_BOTTOM]);
        int new_exclusive_zone = -1;
        if (horiz && !vert) {
            new_exclusive_zone = self->current_allocation.height;
            if (!self->anchors[GTK_LAYER_SHELL_EDGE_TOP])
                new_exclusive_zone += self->margins[GTK_LAYER_SHELL_EDGE_TOP];
            if (!self->anchors[GTK_LAYER_SHELL_EDGE_BOTTOM])
                new_exclusive_zone += self->margins[GTK_LAYER_SHELL_EDGE_BOTTOM];
        } else if (vert && !horiz) {
            new_exclusive_zone = self->current_allocation.width;
            if (!self->anchors[GTK_LAYER_SHELL_EDGE_LEFT])
                new_exclusive_zone += self->margins[GTK_LAYER_SHELL_EDGE_LEFT];
            if (!self->anchors[GTK_LAYER_SHELL_EDGE_RIGHT])
                new_exclusive_zone += self->margins[GTK_LAYER_SHELL_EDGE_RIGHT];
        }
        if (new_exclusive_zone >= 0 && self->exclusive_zone != new_exclusive_zone) {
            self->exclusive_zone = new_exclusive_zone;
            if (self->layer_surface) {
                zwlr_layer_surface_v1_set_exclusive_zone (self->layer_surface, self->exclusive_zone);
            }
        }
    }

    GtkRequisition request_size;
    gtk_widget_get_preferred_size (GTK_WIDGET (gtk_window), NULL, &request_size);

    if ((self->anchors[GTK_LAYER_SHELL_EDGE_LEFT]) &&
        (self->anchors[GTK_LAYER_SHELL_EDGE_RIGHT])) {

        request_size.width = 0;
    }
    if ((self->anchors[GTK_LAYER_SHELL_EDGE_TOP]) &&
        (self->anchors[GTK_LAYER_SHELL_EDGE_BOTTOM])) {

        request_size.height = 0;
    }

    GtkRequisition window_default_size;
    gtk_window_get_default_size (gtk_window,
                                 &window_default_size.width,
                                 &window_default_size.height);
    if (window_default_size.width >= 0)
        request_size.width = window_default_size.width;
    if (window_default_size.height >= 0)
        request_size.height = window_default_size.height;

    if (request_size.width != self->cached_layer_size.width ||
        request_size.height != self->cached_layer_size.height) {

        self->cached_layer_size = request_size;
        if (self->layer_surface) {
            zwlr_layer_surface_v1_set_size (self->layer_surface,
                                            self->cached_layer_size.width,
                                            self->cached_layer_size.height);
        }
    }

    if (self->cached_layer_size.width > 0 &&
        self->cached_layer_size.height > 0) {

        gtk_window_resize (gtk_window,
                           self->cached_layer_size.width,
                           self->cached_layer_size.height);
    }
}

static void
layer_surface_on_size_allocate (GtkWidget *_gtk_window,
                                GdkRectangle *allocation,
                                LayerSurface *self)
{
    (void)_gtk_window;

    if (self->current_allocation.width != allocation->width ||
        self->current_allocation.height != allocation->height) {

        self->current_allocation.width = allocation->width;
        self->current_allocation.height = allocation->height;

        layer_surface_update_size (self);
    }
}

LayerSurface *
layer_surface_new (GtkWindow *gtk_window)
{
    g_return_val_if_fail (gtk_wayland_get_layer_shell_global (), NULL);

    LayerSurface *self = g_new0 (LayerSurface, 1);
    self->super.virtual = &layer_surface_virtual;
    custom_shell_surface_init ((CustomShellSurface *)self, gtk_window);

    self->current_allocation = (GtkRequisition) {
        .width = -1,
        .height = -1,
    };
    self->cached_layer_size = self->current_allocation;
    self->monitor = NULL;
    self->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    self->name_space = NULL;
    self->exclusive_zone = 0;
    self->auto_exclusive_zone = FALSE;
    self->keyboard_interactivity = FALSE;
    self->layer_surface = NULL;

    gtk_window_set_decorated (gtk_window, FALSE);
    g_signal_connect (gtk_window, "size-allocate", G_CALLBACK (layer_surface_on_size_allocate), self);

    return self;
}

LayerSurface *
custom_shell_surface_get_layer_surface (CustomShellSurface *shell_surface)
{
    if (shell_surface && shell_surface->virtual == &layer_surface_virtual)
        return (LayerSurface *)shell_surface;
    else
        return NULL;
}

void
layer_surface_set_layer (LayerSurface *self, enum zwlr_layer_shell_v1_layer layer)
{
    if (self->layer != layer) {
        self->layer = layer;
        if (self->layer_surface) {
            custom_shell_surface_remap ((CustomShellSurface *)self);
        }
    }
}

void
layer_surface_set_monitor (LayerSurface *self, GdkMonitor *monitor)
{
    if (monitor) g_return_if_fail (GDK_IS_WAYLAND_MONITOR (monitor));
    if (monitor != self->monitor) {
        self->monitor = monitor;
        if (self->layer_surface) {
            custom_shell_surface_remap ((CustomShellSurface *)self);
        }
    }
}

void
layer_surface_set_name_space (LayerSurface *self, char const* name_space)
{
    if (g_strcmp0(self->name_space, name_space) != 0) {
        g_free ((gpointer)self->name_space);
        self->name_space = g_strdup (name_space);
        if (self->layer_surface) {
            custom_shell_surface_remap ((CustomShellSurface *)self);
        }
    }
}

void
layer_surface_set_anchor (LayerSurface *self, GtkLayerShellEdge edge, gboolean anchor_to_edge)
{
    g_return_if_fail (edge >= 0 && edge < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER);
    if (anchor_to_edge != self->anchors[edge]) {
        self->anchors[edge] = anchor_to_edge;
        if (self->layer_surface) {
            layer_surface_send_set_anchor (self);
            layer_surface_update_size (self);
            custom_shell_surface_needs_commit ((CustomShellSurface *)self);
        }
    }
}

void
layer_surface_set_margin (LayerSurface *self, GtkLayerShellEdge edge, int margin_size)
{
    g_return_if_fail (edge >= 0 && edge < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER);
    if (margin_size != self->margins[edge]) {
        self->margins[edge] = margin_size;
        layer_surface_send_set_margin (self);
        layer_surface_update_size (self); // Auto exclusive zone might need to change
        custom_shell_surface_needs_commit ((CustomShellSurface *)self);
    }
}

void
layer_surface_set_exclusive_zone (LayerSurface *self, int exclusive_zone)
{
    self->auto_exclusive_zone = FALSE;
    if (self->exclusive_zone != exclusive_zone) {
        self->exclusive_zone = exclusive_zone;
        if (self->layer_surface) {
            zwlr_layer_surface_v1_set_exclusive_zone (self->layer_surface, self->exclusive_zone);
            custom_shell_surface_needs_commit ((CustomShellSurface *)self);
        }
    }
}

void
layer_surface_auto_exclusive_zone_enable (LayerSurface *self)
{
    if (!self->auto_exclusive_zone) {
        self->auto_exclusive_zone = TRUE;
        layer_surface_update_size (self);
    }
}

void
layer_surface_set_keyboard_interactivity (LayerSurface *self, gboolean interactivity)
{
    if (self->keyboard_interactivity != interactivity) {
        self->keyboard_interactivity = interactivity;
        if (self->layer_surface) {
            zwlr_layer_surface_v1_set_keyboard_interactivity (self->layer_surface, self->keyboard_interactivity);
            custom_shell_surface_needs_commit ((CustomShellSurface *)self);
        }
    }
}
