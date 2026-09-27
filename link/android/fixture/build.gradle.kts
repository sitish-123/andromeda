// Stand-ins for the emulator E2E: a MediaSession that obeys its transport controls, and a chat notification with a
// RemoteInput reply. Google Messages would do for the reply, but Android 15 hides SMS it flags as sensitive from
// listeners that are not trusted, so its content is not deterministic.
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "org.umbriel.link.fixture"
    compileSdk = 36
    defaultConfig {
        applicationId = "org.umbriel.link.fixture"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    jvmToolchain(17)
}
