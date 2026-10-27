plugins { id("com.android.application") }

android {
    namespace = "dev.radek.placeholder"
    compileSdk = 35
    defaultConfig {
        applicationId = "dev.radek.placeholder"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }
    sourceSets.getByName("main").java.srcDir("$rootDir/third_party/anyiose-engine/android/app/src/main/java")
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
