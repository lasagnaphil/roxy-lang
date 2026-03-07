plugins {
    id("org.jetbrains.intellij.platform") version "2.6.0"
    kotlin("jvm") version "2.1.0"
}

group = "com.roxy"
version = "0.1.0"

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

kotlin {
    jvmToolchain(21)
}

dependencies {
    intellijPlatform {
        clion("2024.3")
        bundledPlugin("org.jetbrains.plugins.textmate")
        instrumentationTools()
    }
}

intellijPlatform {
    pluginConfiguration {
        id = "com.roxy.intellij"
        name = "Roxy Language"
        version = project.version.toString()
        description = "Language support for the Roxy programming language via LSP"
        vendor {
            name = "Roxy"
        }
        ideaVersion {
            sinceBuild = "243"
        }
    }
}

tasks {
    prepareSandbox {
        from("src/main/resources/textmate") {
            into("${project.name}/textmate")
        }
    }
}
