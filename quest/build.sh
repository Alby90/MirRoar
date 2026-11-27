#!/usr/bin/env bash
# Builds the MirRoar Quest APK without Gradle: NDK clang -> aapt2 -> javac/d8 -> zipalign -> apksigner.
# Usage: ./build.sh [install]
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_SDK_ROOT:-/d/Android/sdk}"
NDK="$SDK/ndk/27.2.12479018"
BT="$SDK/build-tools/34.0.0"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
TC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
OXR="../third_party/openxr/prefab/modules"
API=29
OUT=build

if [ ! -f "$OXR/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so" ]; then
  echo "download OpenXR loader..."
  mkdir -p ../third_party/openxr
  curl -sSL -o ../third_party/openxr_loader.aar \
    https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.38/openxr_loader_for_android-1.1.38.aar
  (cd ../third_party/openxr && unzip -oq ../openxr_loader.aar)
fi

rm -rf "$OUT/apk" "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/apk/lib/arm64-v8a" "$OUT/classes" "$OUT/dex" "$OUT/obj"

echo "[1/5] native"
GLUE="$NDK/sources/android/native_app_glue"
"$TC/clang.exe" --target=aarch64-linux-android$API -O2 -fPIC -c "$GLUE/android_native_app_glue.c" -o "$OUT/obj/glue.o"
"$TC/clang++.exe" --target=aarch64-linux-android$API -std=c++17 -O2 -fPIC -shared \
  -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
  -I"$GLUE" -I"$OXR/headers/include" \
  src/*.cpp "$OUT/obj/glue.o" \
  -L"$OXR/openxr_loader/libs/android.arm64-v8a" -lopenxr_loader \
  -landroid -llog -lEGL -lGLESv3 -lmediandk -laaudio \
  -static-libstdc++ -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,-z,max-page-size=16384 -s \
  -o "$OUT/apk/lib/arm64-v8a/libmirroar.so"
cp "$OXR/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so" "$OUT/apk/lib/arm64-v8a/"

echo "[2/5] java"
javac -nowarn -source 8 -target 8 -bootclasspath "$ANDROID_JAR" -d "$OUT/classes" java/com/mirroar/app/*.java 2>&1 | grep -v "warning:" || true
"$BT/d8.bat" --release --min-api $API --lib "$ANDROID_JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
cp "$OUT/dex/classes.dex" "$OUT/apk/"

echo "[3/5] resources + manifest"
"$BT/aapt2.exe" compile --dir res -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" -I "$ANDROID_JAR" --manifest AndroidManifest.xml \
  --min-sdk-version $API --target-sdk-version 32 -R "$OUT/res.zip" --auto-add-overlay

echo "[4/5] package"
python - "$OUT/base.apk" "$OUT/apk" <<'EOF'
import os, sys, zipfile
apk, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root).replace(os.sep, "/"))
EOF
"$BT/zipalign.exe" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"

echo "[5/5] sign"
KS=keystore/debug.keystore
if [ ! -f "$KS" ]; then
  mkdir -p keystore
  keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias mirroar \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=MirRoar Debug" >/dev/null 2>&1
fi
mkdir -p ../dist
"$BT/apksigner.bat" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --out ../dist/MirRoar-Quest.apk "$OUT/aligned.apk"
echo "OK -> dist/MirRoar-Quest.apk ($(du -h ../dist/MirRoar-Quest.apk | cut -f1))"

if [ "${1:-}" = "install" ]; then
  adb install -r ../dist/MirRoar-Quest.apk
fi
