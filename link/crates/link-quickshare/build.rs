fn main() -> std::io::Result<()> {
    let dir = "../../../protocol/quickshare";
    let protos = [
        "offline_wire_formats.proto",
        "wire_format.proto",
        "ukey.proto",
        "securegcm.proto",
        "securemessage.proto",
        "device_to_device_messages.proto",
    ]
    .map(|name| format!("{dir}/{name}"));
    println!("cargo:rerun-if-changed={dir}");
    prost_build::Config::new().compile_protos(&protos, &[dir])
}
