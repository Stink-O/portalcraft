# Third-party licences of the launcher binary

The Portalcraft launcher (`launcher/`) is MIT-licensed. The binary statically links the Rust crates below (normal dependencies
only, as resolved by `Cargo.lock`; `cargo metadata` for the Linux target `x86_64-unknown-linux-gnu` and the Windows target
`x86_64-pc-windows-gnu`). Each crate keeps its own licence; the SPDX expression is the crate's declared one. Where a crate offers
`A OR B` the launcher is used under either option. Full licence texts are in each crate's source package (`~/.cargo/registry`
after a build, or https://crates.io/crates/<name>).

The launcher also embeds the Barlow typeface (SIL OFL 1.1, `launcher/assets/fonts/OFL.txt`). Prism Launcher (GPL-3.0) is not
part of this binary or this repository: the launcher downloads it on the player's machine from Prism's own GitHub release.

Regenerate with `portalcraft-public-tools/gen_third_party.py`. Crates: 226.

Every crate is under a permissive licence (MIT, Apache-2.0, BSD, ISC, Zlib, Unicode, Boost BSL-1.0, CC0/Unlicense, MPL-2.0 file-level copyleft, CDLA-Permissive-2.0 for webpki-roots data). There is no GPL or LGPL crate.

## MIT OR Apache-2.0 (114)

accesskit 0.24.1, ahash 0.8.12, arboard 3.6.1, arrayvec 0.7.8, as-raw-xcb-connection 1.0.1 [linux only], base64 0.23.1, bitflags 2.13.2, block-buffer 0.10.4, bumpalo 3.20.3, cfg-if 1.0.5, cpufeatures 0.2.17, crc32fast 1.5.2, crypto-common 0.1.7, digest 0.10.7, displaydoc 0.2.7, document-features 0.2.12, ecolor 0.36.2, eframe 0.36.2, egui 0.36.2, egui-wgpu 0.36.2, egui-winit 0.36.2, egui_glow 0.36.2, either 1.19.0, emath 0.36.2, epaint 0.36.2, errno 0.3.14 [linux only], euclid 0.22.14, fdeflate 0.3.7, flate2 1.1.10, font-types 0.12.6, getrandom 0.2.17, getrandom 0.3.4, half 2.7.1, hashbrown 0.17.1, http 1.5.0, httparse 1.10.1, image 0.25.10, itertools 0.15.0, itoa 1.0.18, libc 0.2.190, litrs 1.0.0, lock_api 0.4.14, log 0.4.34, memmap2 0.9.11 [linux only], naga 30.0.1, naga-types 30.0.1, num-traits 0.2.19, once_cell 1.21.4, parking_lot 0.12.5, parking_lot_core 0.9.12, percent-encoding 2.3.2, png 0.17.16, png 0.18.1, polycool 0.4.0, proc-macro2 1.0.107, profiling 1.0.18, quote 1.0.47, read-fonts 0.41.0, renderdoc-sys 1.1.0, rustls-pki-types 1.15.1, scopeguard 1.2.0, serde 1.0.229, serde_core 1.0.229, serde_derive 1.0.229, serde_json 1.0.151, sha2 0.10.9, skrifa 0.44.0, smallvec 1.16.3, smol_str 0.2.2, static_assertions 1.1.0, syn 2.0.119, syn 3.0.7, tar 0.4.46, thiserror 1.0.69 [linux only], thiserror 2.0.21, thiserror-impl 1.0.69 [linux only], thiserror-impl 2.0.21, typenum 1.20.1, unicode-segmentation 1.13.3, unicode-width 0.2.2, ureq 3.4.2, ureq-proto 0.6.4, utf8-zero 0.8.1, web-time 1.1.0, weezl 0.1.12, wgpu 30.0.1, wgpu-core 30.0.1, wgpu-core-deps-windows-linux-android 30.0.1, wgpu-hal 30.0.1, wgpu-naga-bridge 30.0.1, wgpu-types 30.0.1, windows 0.61.3 [windows only], windows-collections 0.2.0 [windows only], windows-core 0.61.2 [windows only], windows-future 0.2.1 [windows only], windows-implement 0.60.2 [windows only], windows-interface 0.59.3 [windows only], windows-link 0.1.3 [windows only], windows-link 0.2.1 [windows only], windows-numerics 0.2.0 [windows only], windows-result 0.3.4 [windows only], windows-strings 0.4.2 [windows only], windows-sys 0.52.0 [windows only], windows-sys 0.59.0 [windows only], windows-sys 0.60.2 [windows only], windows-sys 0.61.2 [windows only], windows-targets 0.52.6 [windows only], windows-targets 0.53.5 [windows only], windows-threading 0.1.0 [windows only], windows_x86_64_gnu 0.52.6 [windows only], windows_x86_64_gnu 0.53.1 [windows only], x11rb 0.13.2 [linux only], x11rb-protocol 0.13.2 [linux only], xattr 1.6.1 [linux only]

## MIT (38)

bytes 1.12.1, calloop 0.13.0 [linux only], calloop 0.14.5 [linux only], calloop-wayland-source 0.3.0 [linux only], calloop-wayland-source 0.4.1 [linux only], dlib 0.5.3 [linux only], fax 0.2.7, generic-array 0.14.7, glutin-winit 0.5.0, harfrust 0.12.0, libm 0.2.16, quick-xml 0.41.0 [linux only], simd-adler32 0.3.10, slab 0.4.12 [linux only], smithay-client-toolkit 0.19.2 [linux only], smithay-client-toolkit 0.20.0 [linux only], smithay-clipboard 0.7.3 [linux only], sysinfo 0.37.2 [windows only], tiff 0.11.3, tracing 0.1.44, tracing-core 0.1.36, wayland-backend 0.3.17 [linux only], wayland-client 0.31.15 [linux only], wayland-csd-frame 0.3.0 [linux only], wayland-cursor 0.31.14 [linux only], wayland-protocols 0.32.13 [linux only], wayland-protocols-experimental 20250721.0.1 [linux only], wayland-protocols-misc 0.3.12 [linux only], wayland-protocols-plasma 0.3.12 [linux only], wayland-protocols-wlr 0.3.12 [linux only], wayland-scanner 0.31.11 [linux only], wayland-sys 0.31.11 [linux only], winreg 0.55.0 [windows only], x11-dl 2.21.0 [linux only], xcursor 0.3.11 [linux only], xkbcommon-dl 0.4.2 [linux only], zip 2.4.2, zmij 1.0.23

## Apache-2.0 OR MIT (19)

bit-set 0.10.0, bit-vec 0.9.1, color 0.3.3, equivalent 1.0.2, fearless_simd 0.4.1, glifo 0.2.0, indexmap 2.14.2, kurbo 0.13.1, linebender_resource_handle 0.1.1, nohash-hasher 0.2.0, ntapi 0.4.3 [windows only], peniko 0.6.1, pin-project-lite 0.2.17, polling 3.11.0 [linux only], rustc-hash 2.1.3, uuid 1.28.0, vello_common 0.1.0, vello_cpu 0.1.0, zeroize 1.9.1

## Apache-2.0 (9)

codespan-reporting 0.13.1, gethostname 1.1.0 [linux only], glutin 0.32.3, glutin_egl_sys 0.7.1, glutin_glx_sys 0.6.1 [linux only], glutin_wgl_sys 0.6.1 [windows only], unicode-general-category 1.1.0, winit 0.30.13, zopfli 0.8.3

## MIT/Apache-2.0 (9)

bitflags 1.3.2, downcast-rs 1.2.1 [linux only], filetime 0.2.29, guillotiere 0.7.0, quick-error 2.0.1, scoped-tls 1.0.1 [linux only], type-map 0.5.1, winapi 0.3.9 [windows only], winapi-x86_64-pc-windows-gnu 0.4.0 [windows only]

## MIT OR Apache-2.0 OR Zlib (6)

cursor-icon 1.2.0, glow 0.17.0, raw-window-handle 0.6.2, xkeysym 0.2.1 [linux only], zune-core 0.5.3, zune-jpeg 0.5.15

## Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT (4)

linux-raw-sys 0.12.1 [linux only], linux-raw-sys 0.4.15 [linux only], rustix 0.38.44 [linux only], rustix 1.1.5 [linux only]

## ISC (3)

libloading 0.8.9, rustls-webpki 0.103.15, untrusted 0.9.0

## BSD-2-Clause OR Apache-2.0 OR MIT (2)

zerocopy 0.8.62, zerocopy-derive 0.8.62

## BSD-3-Clause OR Apache-2.0 (2)

moxcms 0.8.1, pxfm 0.1.30

## BSL-1.0 (2)

clipboard-win 5.4.1 [windows only], error-code 3.4.0 [windows only]

## MIT OR Zlib OR Apache-2.0 (2)

miniz_oxide 0.8.9, miniz_oxide 0.9.1

## Unlicense OR MIT (2)

byteorder-lite 0.1.0, memchr 2.8.3

## Zlib (2)

foldhash 0.2.0, zlib-rs 0.6.8

## Zlib OR Apache-2.0 OR MIT (2)

bytemuck 1.25.2, bytemuck_derive 1.12.1

## (MIT OR Apache-2.0) AND OFL-1.1 AND Ubuntu-font-1.0 (1)

epaint_default_fonts 0.36.2

## (MIT OR Apache-2.0) AND Unicode-3.0 (1)

unicode-ident 1.0.26

## 0BSD OR MIT OR Apache-2.0 (1)

adler2 2.0.1

## Apache-2.0 AND ISC (1)

ring 0.17.14

## Apache-2.0 AND MIT (1)

dpi 0.1.2

## Apache-2.0 OR GPL-2.0-only (1)

self_cell 1.3.0

## Apache-2.0 OR ISC OR MIT (1)

rustls 0.23.45

## Apache-2.0/MIT (1)

rustc-hash 1.1.0

## BSD-3-Clause (1)

subtle 2.6.1

## CDLA-Permissive-2.0 (1)

webpki-roots 1.0.9
