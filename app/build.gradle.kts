import java.net.URI
import java.security.MessageDigest

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// ABIs to build. Override with -Pmf.abis=x86_64 for a fast emulator-only build.
val mfAbis = (project.findProperty("mf.abis") as String? ?: "arm64-v8a,x86_64").split(",").map { it.trim() }

// Offline speech model bundled into the APK so captions work with no network.
val whisperModelName = "ggml-tiny.en-q5_1.bin"
val whisperModelSha256 = "c77c5766f1cef09b6b7d47f21b546cbddd4157886b3b5d6d4f709e91e66c7c2b"
val generatedAssets = layout.buildDirectory.dir("generated/mfassets")

val prepareBundledAssets by tasks.registering {
    description = "Collects fonts, sample media and the offline whisper model into generated assets."
    val outDir = generatedAssets
    outputs.dir(outDir)
    doLast {
        val out = outDir.get().asFile
        val models = File(out, "models").apply { mkdirs() }
        val fonts = File(out, "fonts").apply { mkdirs() }
        val samples = File(out, "samples").apply { mkdirs() }
        rootProject.file("assets/fonts").listFiles()?.forEach { it.copyTo(File(fonts, it.name), overwrite = true) }
        rootProject.file("third_party/whisper.cpp/samples/jfk.wav").copyTo(File(samples, "speech.wav"), overwrite = true)
        val model = File(models, whisperModelName)
        fun sha(f: File): String = MessageDigest.getInstance("SHA-256").digest(f.readBytes()).joinToString("") { "%02x".format(it) }
        if (!model.exists() || sha(model) != whisperModelSha256) {
            val cached = rootProject.file(".cache/models/$whisperModelName")
            if (cached.exists()) cached.copyTo(model, overwrite = true)
            else URI("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$whisperModelName").toURL().openStream().use { input ->
                model.outputStream().use { input.copyTo(it) }
            }
            check(sha(model) == whisperModelSha256) { "Whisper model checksum mismatch" }
        }
    }
}

android {
    namespace = "com.motionforge.app"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.motionforge.mobile"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += mfAbis }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared", "-DMF_WITH_WHISPER=ON", "-DMF_BUILD_TESTS=OFF")
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets["main"].assets.srcDir(generatedAssets)

    buildTypes {
        debug {
            isMinifyEnabled = false
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
        release {
            isMinifyEnabled = false
            // Signed with the local debug key so the APK installs for testing; use your own key for distribution.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true }
    packaging {
        jniLibs { useLegacyPackaging = false }
        resources { excludes += "/META-INF/{AL2.0,LGPL2.1}" }
    }
    androidResources { noCompress += listOf("bin", "wav") }
    testOptions { unitTests.isIncludeAndroidResources = true }
}

tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }.configureEach { dependsOn(prepareBundledAssets) }
tasks.matching { it.name.startsWith("generate") && it.name.endsWith("Assets") }.configureEach { dependsOn(prepareBundledAssets) }

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2025.06.00")
    implementation(composeBom)
    androidTestImplementation(composeBom)
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.1")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.1")
    implementation("androidx.lifecycle:lifecycle-process:2.9.1")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.documentfile:documentfile:1.1.0")
    implementation("androidx.exifinterface:exifinterface:1.4.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test:rules:1.6.1")
    androidTestImplementation("androidx.test.uiautomator:uiautomator:2.3.0")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
}
tasks.matching { it.name.contains("Lint", ignoreCase = true) }.configureEach { dependsOn(prepareBundledAssets) }
