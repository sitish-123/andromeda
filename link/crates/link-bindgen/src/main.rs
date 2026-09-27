//! Generates the Kotlin bindings of `link-ffi`: `uniffi-bindgen generate --library <liblink_ffi.so> --language kotlin`.

fn main() {
    uniffi::uniffi_bindgen_main();
}
