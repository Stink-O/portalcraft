//! The launcher's icon: docs/branding/portalcraft-icon.svg (a blue portal, and the orange portal with the Minecraft C
//! coming out of it, from the Portalcraft logo), rendered to assets/branding/icon-256.png.

const PNG: &[u8] = include_bytes!("../assets/branding/icon-256.png");

/// The icon's RGBA pixels and size.
pub fn rgba() -> (Vec<u8>, u32, u32) {
    let dec = png::Decoder::new(std::io::Cursor::new(PNG));
    let Ok(mut r) = dec.read_info() else { return (vec![0; 4], 1, 1) };
    let mut buf = vec![0; r.output_buffer_size()];
    match r.next_frame(&mut buf) {
        Ok(info) if info.color_type == png::ColorType::Rgba => { buf.truncate(info.buffer_size()); (buf, info.width, info.height) }
        _ => (vec![0; 4], 1, 1),
    }
}

/// The icon as a PNG file (for the application-menu entry).
pub fn write_png(path: &std::path::Path) -> std::io::Result<()> {
    std::fs::write(path, PNG)
}
