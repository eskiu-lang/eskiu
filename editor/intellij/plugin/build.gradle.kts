// Eskiu IntelliJ plugin: the TextMate grammar plus the Eskiu language server through LSP4IJ.
// Build: ./gradlew buildPlugin  -> build/distributions/eskiu-intellij-<version>.zip
//
// Uses the IntelliJ Platform Gradle Plugin (2.x) and JDK 17+. It builds against 2024.1,
// the first platform with the TextMateBundleProvider API, and sets no upper bound, so
// the same zip installs in every JetBrains IDE from 2024.1 on.

plugins {
    kotlin("jvm") version "2.4.20"
    id("org.jetbrains.intellij.platform") version "2.19.0"
}

group = providers.gradleProperty("pluginGroup").get()
version = providers.gradleProperty("pluginVersion").get()

repositories {
    mavenCentral()
    intellijPlatform { defaultRepositories() }
}

dependencies {
    intellijPlatform {
        create(
            providers.gradleProperty("platformType").get(),
            providers.gradleProperty("platformVersion").get(),
        )
        // Contribute our bundle to the platform's built-in TextMate engine.
        bundledPlugin(providers.gradleProperty("platformBundledPlugins").get())
        // The LSP client the language server runs in.
        plugin("com.redhat.devtools.lsp4ij", providers.gradleProperty("lsp4ijVersion").get())
    }
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "241"
            untilBuild = provider { null }
        }
    }
    buildSearchableOptions = false
    // `./gradlew verifyPlugin -PverifyIde=/path/to/IDE.app` checks the plugin against an
    // installed IDE (binary compatibility with its platform and LSP4IJ).
    // `./gradlew publishPlugin` (CI: .github/workflows/editors.yml) with the token in the
    // intellijPlatformPublishingToken Gradle property.
    publishing {
        token = providers.gradleProperty("intellijPlatformPublishingToken")
    }
    pluginVerification {
        ides {
            providers.gradleProperty("verifyIde").orNull?.let { local(file(it)) }
        }
    }
}

// TextMate reads a bundle from disk, so ship it as files in the plugin directory, and
// the language server (`editor/vscode/server.js` with the `flatten.js` it requires, and the
// package.json it reads its version from) next to it.
tasks.prepareSandbox {
    from(layout.projectDirectory.dir("src/main/resources/bundles")) {
        into(intellijPlatform.projectName.map { "$it/bundles" })
    }
    from(layout.projectDirectory.dir("../../vscode")) {
        include("server.js", "flatten.js", "package.json")
        into(intellijPlatform.projectName.map { "$it/server" })
    }
}

kotlin { jvmToolchain(17) }
