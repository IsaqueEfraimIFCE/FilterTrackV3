# Android Play Internal Testing

Use this checklist when preparing FilterTrack for Google Play internal testing.

## Current App Identity

- Application id: `com.filtertrack`
- Version code: `2`
- Version name: `1.0`
- Current signed AAB path: `app/build/outputs/bundle/release/app-release.aab`
- Release artifact task: `.\gradlew.bat :app:bundleRelease`
- Debug validation task: `.\gradlew.bat :app:assembleDebug`

The Play Console app expects package `com.filtertrack`. Keep that package name
for all future uploads.

## Required Local Secrets

Do not commit real keys.

For BI auto-login, copy `release.properties.example` to `release.properties` and
set the admin key:

```properties
FILTERTRACK_BI_ADMIN_KEY=...
```

`release.properties` is ignored by git. It currently holds the local BI admin
key used to build the app. Do not copy its value into documentation or prompts.

For signed release bundles, copy `keystore.properties.example` to
`keystore.properties` and set the upload keystore values:

```properties
storeFile=release/filtertrack-upload.jks
storePassword=...
keyAlias=filtertrack-upload
keyPassword=...
```

Environment variable alternatives:

- `FILTERTRACK_BI_USER_KEY`
- `FILTERTRACK_BI_ADMIN_KEY`
- `FILTERTRACK_STORE_FILE`
- `FILTERTRACK_STORE_PASSWORD`
- `FILTERTRACK_KEY_ALIAS`
- `FILTERTRACK_KEY_PASSWORD`

## Build

```powershell
.\gradlew.bat :app:assembleDebug
.\gradlew.bat :app:bundleRelease
```

Upload the signed bundle:

```text
app/build/outputs/bundle/release/app-release.aab
```

If no keystore is configured, Gradle can still produce a release bundle for local
validation, but that bundle is not suitable for Play upload.

The current development machine has an upload keystore at:

```text
release/filtertrack-upload.jks
```

and signing credentials in ignored `keystore.properties`. Losing this upload key
after Play accepts the app can block normal app updates or require an upload key
reset in Play Console.

## Versioning

Play Console rejected `versionCode = 1` because it had already been uploaded.
The current release uses:

```text
versionCode = 2
versionName = 1.0
```

Increment `versionCode` for every replacement AAB uploaded to Play Console,
including internal testing builds.

## Store Readiness Notes

- Release WebView debugging is disabled through `BuildConfig.DEBUG`.
- Cleartext network traffic is disabled.
- Local WebView/BLE/session data is excluded from Android backup and device
  transfer.
- The BI admin key is embedded into the app at build time from ignored local
  properties or environment variables.
- The app requires Bluetooth LE and uses BLE scan/connect permissions.
- Privacy policy URL for Play Console:
  `https://filtertrack-api.fly.dev/privacy`.

## Play Console Tasks

- Create the app using package `com.filtertrack`.
- Enroll in Play App Signing and use an upload key for `.aab` uploads.
- Create an internal testing release and add tester accounts or a tester list.
- Complete Data safety, app access, target audience, content rating, privacy
  policy, app category, store listing, and permissions declarations as required
  by Play Console.
- Increment `versionCode` before uploading any replacement bundle.

## Validation Commands

Check package/version metadata in the generated release manifest:

```powershell
Get-Content app\build\intermediates\merged_manifests\release\processReleaseManifest\AndroidManifest.xml |
  Select-String 'package=|versionCode|versionName'
```

Check that the AAB is signed:

```powershell
jar tf app\build\outputs\bundle\release\app-release.aab | Select-String '^META-INF/'
```

Expected signature files include:

```text
META-INF/FILTERTR.SF
META-INF/FILTERTR.RSA
META-INF/MANIFEST.MF
```
