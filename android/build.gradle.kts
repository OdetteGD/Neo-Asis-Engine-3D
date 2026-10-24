// ---------------------------------------------------------------------------
//  Top-level Gradle for the Android player.
//
//  Plugin versions are declared here with `apply false`; the app module then
//  applies them by id only. Without a version in one of the two places Gradle
//  fails with "plugin dependency must include a version number for this
//  source".
// ---------------------------------------------------------------------------
plugins {
    id("com.android.application") version "8.6.1" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
