// Version set is pinned to what compileSdk 37 (Android 17) actually requires.
//
// Per developer.android.com/about/versions/17/setup-sdk, a project must be on
// AGP 8.9.0-rc01 or higher to build against Android 17. AGP 8.9 in turn
// requires Gradle 8.11.1+ (see gradle/wrapper/gradle-wrapper.properties), and
// Kotlin 1.9.x does not officially support Gradle that new - hence 2.0.x.
//
// Staying on AGP 8.x rather than jumping to 9.x deliberately: 9.x brings
// breaking DSL changes (kotlinOptions, packagingOptions) for no benefit here.
plugins {
    id("com.android.application") version "8.9.0" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
}
