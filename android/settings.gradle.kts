// ---------------------------------------------------------------------------
//  Gradle settings for the Android player.
//
//  pluginManagement MUST declare the repositories: the Android Gradle Plugin
//  lives in Google's Maven repo, not the Gradle Plugin Portal, so without this
//  block every plugin resolution fails with "was not found in any of the
//  following sources".
// ---------------------------------------------------------------------------
pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "NeoAsisEngine"
include(":app")
