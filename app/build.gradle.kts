import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

val localProperties = Properties().apply {
    val file = rootProject.file("local.properties")
    if (file.exists()) {
        file.inputStream().use(::load)
    }
}

val releaseProperties = Properties().apply {
    val file = rootProject.file("release.properties")
    if (file.exists()) {
        file.inputStream().use(::load)
    }
}

val signingProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.exists()) {
        file.inputStream().use(::load)
    }
}

val releaseStoreFilePath = signingProperties.getProperty("storeFile")
    ?: System.getenv("FILTERTRACK_STORE_FILE")
val hasReleaseSigning = !releaseStoreFilePath.isNullOrBlank()

fun secretProperty(name: String): String =
    (localProperties.getProperty(name)
        ?: releaseProperties.getProperty(name)
        ?: signingProperties.getProperty(name)
        ?: System.getenv(name)
        ?: "").replace("\\", "\\\\").replace("\"", "\\\"")

android {
    namespace = "com.filtertrack"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.filtertrack"
        minSdk = 28
        targetSdk = 36
        versionCode = 5
        versionName = "1.2"
        buildConfigField("String", "FILTERTRACK_BI_ADMIN_KEY", "\"${secretProperty("FILTERTRACK_BI_ADMIN_KEY")}\"")
        buildConfigField("String", "FILTERTRACK_BI_USER_KEY", "\"${secretProperty("FILTERTRACK_BI_USER_KEY")}\"")
    }

    buildFeatures {
        buildConfig = true
    }

    signingConfigs {
        create("release") {
            if (hasReleaseSigning) {
                storeFile = rootProject.file(releaseStoreFilePath!!)
                storePassword = signingProperties.getProperty("storePassword")
                    ?: System.getenv("FILTERTRACK_STORE_PASSWORD")
                keyAlias = signingProperties.getProperty("keyAlias")
                    ?: System.getenv("FILTERTRACK_KEY_ALIAS")
                keyPassword = signingProperties.getProperty("keyPassword")
                    ?: System.getenv("FILTERTRACK_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
}
