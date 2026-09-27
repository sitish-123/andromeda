plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

val linkDir: File = rootDir.parentFile
val rustLibs = layout.buildDirectory.dir("rust/jniLibs")
val bindings = layout.buildDirectory.dir("generated/uniffi")
val abis = listOf("arm64-v8a", "x86_64")

android {
    namespace = "org.umbriel.link.core"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        minSdk = 29
        consumerProguardFiles("consumer-rules.pro")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets["main"].jniLibs.srcDir(rustLibs)
    sourceSets["main"].kotlin.srcDir(bindings)
}

kotlin {
    jvmToolchain(17)
}

// link-ffi for every ABI through cargo-ndk.
val cargoNdk by tasks.registering(Exec::class) {
    workingDir = linkDir
    inputs.dir(linkDir.resolve("crates"))
    inputs.file(linkDir.resolve("Cargo.lock"))
    outputs.dir(rustLibs)
    environment("ANDROID_NDK_HOME", android.ndkDirectory.absolutePath)
    val targets = abis.flatMap { listOf("-t", it) }
    commandLine(
        listOf("cargo", "ndk") + targets +
            listOf("-P", "29", "-o", rustLibs.get().asFile.absolutePath, "build", "--release", "-p", "link-ffi"),
    )
}

// Bindgen reads the metadata symbols the stripped release libraries lack, so it reads a host debug build instead.
val hostFfi by tasks.registering(Exec::class) {
    workingDir = linkDir
    inputs.dir(linkDir.resolve("crates"))
    outputs.file(linkDir.resolve("target/debug/liblink_ffi.so"))
    commandLine("cargo", "build", "--quiet", "-p", "link-ffi", "-p", "link-bindgen")
}

val uniffiBindings by tasks.registering(Exec::class) {
    dependsOn(hostFfi, cargoNdk)
    workingDir = linkDir
    val library = linkDir.resolve("target/debug/liblink_ffi.so")
    inputs.file(library)
    outputs.dir(bindings)
    commandLine(
        linkDir.resolve("target/debug/uniffi-bindgen").absolutePath, "generate",
        "--library", library.absolutePath,
        "--config", linkDir.resolve("crates/link-ffi/uniffi.toml").absolutePath,
        "--language", "kotlin", "--no-format",
        "--out-dir", bindings.get().asFile.absolutePath,
    )
}

tasks.named("preBuild") { dependsOn(uniffiBindings) }

dependencies {
    api(libs.kotlinx.coroutines.android)
    implementation("${libs.jna.get()}@aar")
}
