// ---------------------------------------------------------------------------
//  Top-level Gradle for the Android player
// ---------------------------------------------------------------------------
buildscript {
    repositories {
        google()
        mavenCentral()
    }
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
