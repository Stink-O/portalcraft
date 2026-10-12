**What this changes and why:**

**Tested on:** Windows / Linux (which one, and which maps?)

- [ ] Builds: Linux `make -C bridge`; Windows `make -C bridge -f Makefile.win` (or say which one you could not build)
- [ ] Mod: `(cd bridge/fabric && ./gradlew build)`
- [ ] Launcher, if touched: `CARGO_TARGET_DIR=target-launcher cargo test --release --manifest-path launcher/Cargo.toml`
- [ ] Visual change: a screenshot, or the `bridge.log` lines that show it working
- [ ] No game files, no decompiled code (see CONTRIBUTING.md)
