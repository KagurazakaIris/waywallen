//! Pure-function layout module: turn a `(texture_size, display_size,
//! fillmode, align)` tuple into the `source_rect` / `dest_rect` /
//! `clear_color` triple the wire-level `set_config` event carries.
//!
//! Tiled variants need sampler wrap or multiple draws and cannot be
//! expressed as a single `(source_rect, dest_rect)` pair on the
//! current wire format. They are accepted at the API surface (so
//! clients can persist their preference) but degrade to
//! `PreserveAspectFit` inside `compute()` with a one-shot warning.
//! Future work: protocol bump with a wrap flag, or daemon-side
//! staging texture.

use serde::{Deserialize, Serialize};

#[derive(Copy, Clone, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum FillMode {
    #[default]
    Stretched,
    PreserveAspectFit,
    PreserveAspectCrop,
    Tiled,
    TiledOnlyHorizontally,
    TiledOnlyVertically,
    Centered,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum Align {
    TopLeft,
    Top,
    TopRight,
    Left,
    #[default]
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
}

impl Align {
    fn h_factor(self) -> f32 {
        match self {
            Align::TopLeft | Align::Left | Align::BottomLeft => 0.0,
            Align::Top | Align::Center | Align::Bottom => 0.5,
            Align::TopRight | Align::Right | Align::BottomRight => 1.0,
        }
    }
    fn v_factor(self) -> f32 {
        match self {
            Align::TopLeft | Align::Top | Align::TopRight => 0.0,
            Align::Left | Align::Center | Align::Right => 0.5,
            Align::BottomLeft | Align::Bottom | Align::BottomRight => 1.0,
        }
    }
}

#[derive(Copy, Clone, Debug)]
pub struct LayoutInput {
    pub tex_w: f32,
    pub tex_h: f32,
    pub disp_w: f32,
    pub disp_h: f32,
    pub fillmode: FillMode,
    pub align: Align,
    pub clear_rgba: [f32; 4],
}

#[derive(Copy, Clone, Debug, PartialEq)]
pub struct LayoutOutput {
    /// Source rect in texture pixels: (x, y, w, h).
    pub source: (f32, f32, f32, f32),
    /// Destination rect in display pixels: (x, y, w, h).
    pub dest: (f32, f32, f32, f32),
    /// Background fill color (RGBA, sRGB straight alpha).
    pub clear_rgba: [f32; 4],
}

/// Resolve one layout. Pure; never panics. Degenerate inputs
/// (`tex_w/h <= 0` or `disp_w/h <= 0`) collapse to a Stretched output
/// that the consumer will silently no-op.
pub fn compute(i: LayoutInput) -> LayoutOutput {
    if i.tex_w <= 0.0 || i.tex_h <= 0.0 || i.disp_w <= 0.0 || i.disp_h <= 0.0 {
        return LayoutOutput {
            source: (0.0, 0.0, i.tex_w.max(0.0), i.tex_h.max(0.0)),
            dest: (0.0, 0.0, i.disp_w.max(0.0), i.disp_h.max(0.0)),
            clear_rgba: i.clear_rgba,
        };
    }

    let fillmode = match i.fillmode {
        FillMode::Tiled | FillMode::TiledOnlyHorizontally | FillMode::TiledOnlyVertically => {
            log::warn!(
                "display_layout: fillmode {:?} not yet expressible on the wire; \
                 falling back to PreserveAspectFit",
                i.fillmode
            );
            FillMode::PreserveAspectFit
        }
        other => other,
    };

    match fillmode {
        FillMode::Stretched => LayoutOutput {
            source: (0.0, 0.0, i.tex_w, i.tex_h),
            dest: (0.0, 0.0, i.disp_w, i.disp_h),
            clear_rgba: i.clear_rgba,
        },

        FillMode::PreserveAspectFit => {
            let scale = (i.disp_w / i.tex_w).min(i.disp_h / i.tex_h);
            let dw = i.tex_w * scale;
            let dh = i.tex_h * scale;
            let dx = (i.disp_w - dw) * i.align.h_factor();
            let dy = (i.disp_h - dh) * i.align.v_factor();
            LayoutOutput {
                source: (0.0, 0.0, i.tex_w, i.tex_h),
                dest: (dx, dy, dw, dh),
                clear_rgba: i.clear_rgba,
            }
        }

        FillMode::PreserveAspectCrop => {
            // Pick the source-side rect that, when stretched to fill
            // the display, preserves aspect. The cropped axis is
            // positioned by `align`.
            let scale = (i.disp_w / i.tex_w).max(i.disp_h / i.tex_h);
            let sw = i.disp_w / scale;
            let sh = i.disp_h / scale;
            let sx = (i.tex_w - sw) * i.align.h_factor();
            let sy = (i.tex_h - sh) * i.align.v_factor();
            LayoutOutput {
                source: (sx, sy, sw, sh),
                dest: (0.0, 0.0, i.disp_w, i.disp_h),
                clear_rgba: i.clear_rgba,
            }
        }

        FillMode::Centered => {
            // 1:1 pixel display. If the texture is smaller than the
            // display on a given axis, place it inside according to
            // `align` and letterbox the rest. If larger, crop the
            // texture according to `align`.
            let (sx, sw, dx, dw) = axis_centered(i.tex_w, i.disp_w, i.align.h_factor());
            let (sy, sh, dy, dh) = axis_centered(i.tex_h, i.disp_h, i.align.v_factor());
            LayoutOutput {
                source: (sx, sy, sw, sh),
                dest: (dx, dy, dw, dh),
                clear_rgba: i.clear_rgba,
            }
        }

        // Unreachable: tile* already mapped above.
        FillMode::Tiled | FillMode::TiledOnlyHorizontally | FillMode::TiledOnlyVertically => {
            unreachable!()
        }
    }
}

/// One axis of `Centered`. Returns `(source_off, source_len, dest_off, dest_len)`.
fn axis_centered(tex: f32, disp: f32, factor: f32) -> (f32, f32, f32, f32) {
    if tex <= disp {
        // Texture fits — place fully inside the display.
        let dest_len = tex;
        let dest_off = (disp - tex) * factor;
        (0.0, tex, dest_off, dest_len)
    } else {
        // Texture is larger than display — crop a viewport of `disp`
        // pixels out of the texture, positioned by `factor`.
        let src_off = (tex - disp) * factor;
        (src_off, disp, 0.0, disp)
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn input(tex: (f32, f32), disp: (f32, f32), fillmode: FillMode, align: Align) -> LayoutInput {
        LayoutInput {
            tex_w: tex.0,
            tex_h: tex.1,
            disp_w: disp.0,
            disp_h: disp.1,
            fillmode,
            align,
            clear_rgba: [0.0, 0.0, 0.0, 1.0],
        }
    }

    #[test]
    fn stretched_is_identity_regardless_of_align() {
        let out = compute(input((1920.0, 1080.0), (1280.0, 720.0), FillMode::Stretched, Align::TopLeft));
        assert_eq!(out.source, (0.0, 0.0, 1920.0, 1080.0));
        assert_eq!(out.dest, (0.0, 0.0, 1280.0, 720.0));
        let out2 = compute(input((1920.0, 1080.0), (1280.0, 720.0), FillMode::Stretched, Align::BottomRight));
        assert_eq!(out, out2);
    }

    #[test]
    fn fit_wider_texture_letterboxes_top_bottom() {
        // 16:9 texture into 4:3 display => bars top/bottom, dest_w == disp_w
        let out = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectFit, Align::Center));
        assert_eq!(out.source, (0.0, 0.0, 1920.0, 1080.0));
        // scale = min(800/1920, 600/1080) = min(0.4167, 0.5556) = 0.4167
        // dest_w = 1920 * 0.4167 = 800; dest_h = 1080 * 0.4167 = 450
        // dy = (600 - 450) * 0.5 = 75
        assert!((out.dest.0 - 0.0).abs() < 1e-3);
        assert!((out.dest.1 - 75.0).abs() < 1e-3);
        assert!((out.dest.2 - 800.0).abs() < 1e-3);
        assert!((out.dest.3 - 450.0).abs() < 1e-3);
    }

    #[test]
    fn fit_taller_texture_letterboxes_left_right() {
        // 1:1 texture into 16:9 display => bars left/right
        let out = compute(input((1000.0, 1000.0), (1920.0, 1080.0), FillMode::PreserveAspectFit, Align::Center));
        // scale = min(1920/1000, 1080/1000) = 1.08
        // dest_w = 1080, dest_h = 1080
        // dx = (1920 - 1080)*0.5 = 420
        assert!((out.dest.0 - 420.0).abs() < 1e-3);
        assert!((out.dest.1 - 0.0).abs() < 1e-3);
        assert!((out.dest.2 - 1080.0).abs() < 1e-3);
        assert!((out.dest.3 - 1080.0).abs() < 1e-3);
    }

    #[test]
    fn fit_top_left_align_pins_to_corner() {
        let out = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectFit, Align::TopLeft));
        assert!((out.dest.0 - 0.0).abs() < 1e-3);
        assert!((out.dest.1 - 0.0).abs() < 1e-3);
    }

    #[test]
    fn fit_bottom_right_align_pins_to_corner() {
        let out = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectFit, Align::BottomRight));
        // dest_w = 800, dest_h = 450; dx = (800-800)*1=0, dy=(600-450)*1=150
        assert!((out.dest.0 - 0.0).abs() < 1e-3);
        assert!((out.dest.1 - 150.0).abs() < 1e-3);
    }

    #[test]
    fn crop_wider_texture_crops_horizontally() {
        // 16:9 tex into 4:3 disp: scale = max(800/1920, 600/1080) = max(0.417, 0.556) = 0.556
        // sw = 800/0.556 = 1440, sh = 600/0.556 = 1080
        // sx = (1920-1440)*0.5 = 240, sy = 0
        let out = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectCrop, Align::Center));
        assert!((out.source.0 - 240.0).abs() < 1e-3);
        assert!((out.source.1 - 0.0).abs() < 1e-3);
        assert!((out.source.2 - 1440.0).abs() < 1e-3);
        assert!((out.source.3 - 1080.0).abs() < 1e-3);
        assert_eq!(out.dest, (0.0, 0.0, 800.0, 600.0));
    }

    #[test]
    fn crop_top_left_align_keeps_top_left_of_texture() {
        let out = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectCrop, Align::TopLeft));
        assert!((out.source.0 - 0.0).abs() < 1e-3);
        assert!((out.source.1 - 0.0).abs() < 1e-3);
    }

    #[test]
    fn crop_taller_texture_crops_vertically() {
        // 1:1 tex into 16:9 disp: scale = max(1920/1000, 1080/1000) = 1.92
        // sw = 1920/1.92 = 1000; sh = 1080/1.92 = 562.5
        // sx = 0, sy = (1000-562.5)*0.5 = 218.75
        let out = compute(input((1000.0, 1000.0), (1920.0, 1080.0), FillMode::PreserveAspectCrop, Align::Center));
        assert!((out.source.0 - 0.0).abs() < 1e-3);
        assert!((out.source.1 - 218.75).abs() < 1e-3);
        assert!((out.source.2 - 1000.0).abs() < 1e-3);
        assert!((out.source.3 - 562.5).abs() < 1e-3);
        assert_eq!(out.dest, (0.0, 0.0, 1920.0, 1080.0));
    }

    #[test]
    fn centered_smaller_texture_letterboxes_around_native_size() {
        // 800x600 tex into 1920x1080 disp, Center align.
        // dest_x = (1920-800)*0.5 = 560, dest_y = (1080-600)*0.5 = 240
        let out = compute(input((800.0, 600.0), (1920.0, 1080.0), FillMode::Centered, Align::Center));
        assert_eq!(out.source, (0.0, 0.0, 800.0, 600.0));
        assert!((out.dest.0 - 560.0).abs() < 1e-3);
        assert!((out.dest.1 - 240.0).abs() < 1e-3);
        assert!((out.dest.2 - 800.0).abs() < 1e-3);
        assert!((out.dest.3 - 600.0).abs() < 1e-3);
    }

    #[test]
    fn centered_larger_texture_crops_to_display_pixel_for_pixel() {
        // 4000x3000 tex into 1920x1080 disp, Center align.
        // sx = (4000-1920)*0.5 = 1040, sy = (3000-1080)*0.5 = 960, sw=1920, sh=1080
        let out = compute(input((4000.0, 3000.0), (1920.0, 1080.0), FillMode::Centered, Align::Center));
        assert!((out.source.0 - 1040.0).abs() < 1e-3);
        assert!((out.source.1 - 960.0).abs() < 1e-3);
        assert!((out.source.2 - 1920.0).abs() < 1e-3);
        assert!((out.source.3 - 1080.0).abs() < 1e-3);
        assert_eq!(out.dest, (0.0, 0.0, 1920.0, 1080.0));
    }

    #[test]
    fn centered_top_left_pins_smaller_texture_to_corner() {
        let out = compute(input((800.0, 600.0), (1920.0, 1080.0), FillMode::Centered, Align::TopLeft));
        assert_eq!(out.dest, (0.0, 0.0, 800.0, 600.0));
    }

    #[test]
    fn centered_top_left_keeps_top_left_of_larger_texture() {
        let out = compute(input((4000.0, 3000.0), (1920.0, 1080.0), FillMode::Centered, Align::TopLeft));
        assert_eq!(out.source, (0.0, 0.0, 1920.0, 1080.0));
        assert_eq!(out.dest, (0.0, 0.0, 1920.0, 1080.0));
    }

    #[test]
    fn tiled_degrades_to_fit() {
        let fit = compute(input((1920.0, 1080.0), (800.0, 600.0), FillMode::PreserveAspectFit, Align::Center));
        for fm in [FillMode::Tiled, FillMode::TiledOnlyHorizontally, FillMode::TiledOnlyVertically] {
            let out = compute(input((1920.0, 1080.0), (800.0, 600.0), fm, Align::Center));
            assert_eq!(out.source, fit.source, "tile variant {fm:?} should match fit");
            assert_eq!(out.dest, fit.dest, "tile variant {fm:?} should match fit");
        }
    }

    #[test]
    fn degenerate_zero_input_does_not_panic() {
        let out = compute(input((0.0, 0.0), (1920.0, 1080.0), FillMode::PreserveAspectFit, Align::Center));
        assert_eq!(out.dest, (0.0, 0.0, 1920.0, 1080.0));
        let out = compute(input((1920.0, 1080.0), (0.0, 0.0), FillMode::PreserveAspectFit, Align::Center));
        assert_eq!(out.source, (0.0, 0.0, 1920.0, 1080.0));
    }

    #[test]
    fn equal_aspect_fit_and_crop_match_stretched() {
        // 16:9 into 16:9: identity for all three modes
        let s = compute(input((1920.0, 1080.0), (3840.0, 2160.0), FillMode::Stretched, Align::Center));
        let f = compute(input((1920.0, 1080.0), (3840.0, 2160.0), FillMode::PreserveAspectFit, Align::Center));
        let c = compute(input((1920.0, 1080.0), (3840.0, 2160.0), FillMode::PreserveAspectCrop, Align::Center));
        assert_eq!(s.dest, f.dest);
        assert_eq!(s.dest, c.dest);
        assert_eq!(s.source, f.source);
        assert_eq!(s.source, c.source);
    }

    #[derive(Serialize, Deserialize, PartialEq, Eq, Debug)]
    struct Wrap {
        fillmode: FillMode,
        align: Align,
    }

    #[test]
    fn enums_round_trip_serde_snake_case() {
        let w = Wrap { fillmode: FillMode::PreserveAspectFit, align: Align::BottomRight };
        let s = toml::to_string(&w).unwrap();
        assert!(s.contains("preserve_aspect_fit"), "got {s}");
        assert!(s.contains("bottom_right"), "got {s}");
        let parsed: Wrap = toml::from_str(&s).unwrap();
        assert_eq!(parsed, w);
    }
}
