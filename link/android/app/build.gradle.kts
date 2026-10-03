plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "org.umbriel.link"
    compileSdk = 36
    defaultConfig {
        applicationId = "org.umbriel.link"
        minSdk = 29
        targetSdk = 36
        // The release workflow sets these from the tag and run number.
        versionCode = System.getenv("LINK_VERSION_CODE")?.toInt() ?: 1
        versionName = System.getenv("LINK_VERSION_NAME") ?: "0.1.0"
    }
    // The key lives outside the repository; without LINK_KEYSTORE a release build is left unsigned.
    val keystore = System.getenv("LINK_KEYSTORE")
    signingConfigs {
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("LINK_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("LINK_KEY_ALIAS") ?: "link"
                keyPassword = System.getenv("LINK_KEY_PASSWORD") ?: System.getenv("LINK_KEYSTORE_PASSWORD")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (keystore != null) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    implementation(project(":core"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.process)
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material.icons)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.camerax.camera2)
    implementation(libs.camerax.lifecycle)
    implementation(libs.camerax.view)
    implementation(libs.zxing.core)
}
