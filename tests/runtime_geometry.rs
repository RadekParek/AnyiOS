// Exercise the exact pinned-engine coordinate helpers without building the
// Android/SDL runtime. The real engine defines this same orientation enum.
#[derive(Copy, Clone)]
pub enum DeviceOrientation {
    Portrait,
    PortraitUpsideDown,
    LandscapeLeft,
    LandscapeRight,
}
#[path = "../third_party/anyiose-engine/src/window/geometry.rs"]
mod geometry;

#[test]
fn actual_drawable_fills_surface_even_when_reported_window_is_smaller() {
    // The EGL driver may keep a native-sized framebuffer while Android/SDL
    // reports a smaller fixed buffer. The viewport must cover EGL, not SDL.
    for (guest, window, drawable) in [
        ((1440, 3200), (720, 1600), (1440, 3200)),
        ((3200, 1440), (1600, 720), (3200, 1440)),
    ] {
        let viewport = geometry::fit_viewport(guest, drawable);
        assert_eq!(viewport, (0, 0, drawable.0, drawable.1));
        let mouse = geometry::window_to_drawable(
            (window.0 as f32 / 2.0, window.1 as f32 / 2.0),
            window,
            drawable,
        );
        assert_eq!(geometry::normalize_viewport_coords(mouse, viewport), (0.0, 0.0));
    }
}

#[test]
fn honored_reduced_drawables_still_map_to_the_same_full_view_fraction() {
    for (guest, full, reduced) in [
        ((1440, 3200), (1440, 3200), (720, 1600)),
        ((3200, 1440), (3200, 1440), (1600, 720)),
    ] {
        assert_eq!(geometry::fit_viewport(guest, full), (0, 0, full.0, full.1));
        assert_eq!(
            geometry::fit_viewport(guest, reduced),
            (0, 0, reduced.0, reduced.1)
        );
        // A host compositor stretching the complete drawable to the view
        // preserves center and edges, independent of resolution.
        assert_eq!(
            geometry::normalize_viewport_coords(
                (reduced.0 as f32 / 2.0, reduced.1 as f32 / 2.0),
                geometry::fit_viewport(guest, reduced)
            ),
            (0.0, 0.0)
        );
    }
}
