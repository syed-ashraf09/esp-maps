plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.espmaps"

    // 35, not 37, deliberately - see docs/ANDROID_SETUP.md section 2.
    //
    // AGP 8.9.0 reports "tested up to compileSdk = 35" and cannot read this
    // machine's SDK metadata at API 37 ("only understands SDK XML versions up
    // to 3 but ... version 4 was encountered"). It also looks for
    // platforms/android-37 while the installed directory is android-37.0.
    //
    // Nothing in this app uses an API above 33, so targeting 35 costs
    // nothing, and AGP downloads the platform itself on first build. Moving
    // to 37 means AGP 9.3 + Gradle 9.5, which is a separate upgrade.
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.espmaps"
        // API 26 is the floor for LE 2M PHY, which the tile stream depends on.
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        buildConfigField("String", "MAPTILER_KEY",
            "\"${project.findProperty("MAPTILER_KEY") ?: ""}\"")
        buildConfigField("String", "GRAPHHOPPER_KEY",
            "\"${project.findProperty("GRAPHHOPPER_KEY") ?: ""}\"")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.lifecycle:lifecycle-service:2.8.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")

    // Fused location — better bearing and speed than raw GPS, which matters
    // because the ESP32 has no magnetometer and relies on course-over-ground.
    implementation("com.google.android.gms:play-services-location:21.3.0")

    // GuidanceParser is deliberately free of Android types so it can be
    // tested on the JVM, without an emulator or Robolectric.
    testImplementation("junit:junit:4.13.2")
}
