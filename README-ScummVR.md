# ScummVR

Play classic adventure games in VR on your Meta Quest 2/3/Pro. ScummVR is a fork of [ScummVM](https://www.scummvm.org/) that adds a virtual cinema mode — your games appear on a floating screen in a dark VR environment, with Quest controller support for pointing and clicking.

![Day of the Tentacle running in VR on Meta Quest 2](https://img.shields.io/badge/Tested-Quest_2-blue)

## Supported Devices

- Meta Quest 2
- Meta Quest Pro
- Meta Quest 3

## Quick Install

1. **Enable Developer Mode** on your Quest ([instructions](https://developer.oculus.com/documentation/native/android/mobile-device-setup/))
2. Download the latest `ScummVM-VR-debug.apk` from [Releases](https://github.com/jarhed/ScummVR/releases)
3. Install via ADB:
   ```bash
   adb install ScummVM-VR-debug.apk
   ```
4. Find **ScummVM VR** in your Quest app library under **Unknown Sources**

## Adding Games

1. Connect your Quest to your PC via USB
2. Copy your game files to the Quest:
   ```bash
   adb shell mkdir -p /sdcard/scummvm_games/mygame
   adb push /path/to/game/files/ /sdcard/scummvm_games/mygame/
   ```
3. Launch ScummVM VR on your Quest
4. Click **Add Game...** and browse to `/sdcard/scummvm_games/mygame/`
5. ScummVM will auto-detect the game — click **Start** to play!

### Where to get games

- [Day of the Tentacle Remastered](https://store.steampowered.com/app/388210/) (use classic mode files)
- [Beneath a Steel Sky](https://www.scummvm.org/games/#games-bass) (freeware)
- [Flight of the Amazon Queen](https://www.scummvm.org/games/#games-queen) (freeware)
- See the full [ScummVM compatibility list](https://www.scummvm.org/compatibility/) for 2000+ supported games

## Controls

| Quest Controller | Action |
|---|---|
| Right controller aim | Mouse cursor |
| Right trigger | Left click |
| Right grip | Right click |
| Right B button | ESC (skip cutscenes, open menu) |
| Left thumbstick up/down | Scroll wheel |
| Head movement | Look around the virtual screen |

## Building from Source

### Prerequisites

- Linux (tested on Ubuntu 25.10)
- Android SDK and NDK 23.2.8568313
- Java 17 JDK
- CMake

### Setup

```bash
# Set environment
export ANDROID_SDK_ROOT=$HOME/android-sdk
export ANDROID_NDK_ROOT=$HOME/android-sdk/ndk/23.2.8568313

# Build OpenXR loader
git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git
cd OpenXR-SDK && mkdir build-android && cd build-android
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF
make -j$(nproc) openxr_loader
cd ../..

# Build libogg + libvorbis (for voice acting support)
git clone --depth 1 https://github.com/xiph/ogg.git
cd ogg && mkdir build-android && cd build-android
cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_INSTALL_PREFIX=$HOME/android-libs -DBUILD_SHARED_LIBS=OFF
make -j$(nproc) install && cd ../..

git clone --depth 1 https://github.com/xiph/vorbis.git
cd vorbis && mkdir build-android && cd build-android
cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_INSTALL_PREFIX=$HOME/android-libs -DBUILD_SHARED_LIBS=OFF \
  -DOGG_INCLUDE_DIR=$HOME/android-libs/include \
  -DOGG_LIBRARY=$HOME/android-libs/lib/libogg.a -DBUILD_TESTING=OFF
make -j$(nproc) install && cd ../..
```

### Build ScummVR

```bash
# Configure
CXXFLAGS="-I$HOME/android-libs/include" \
LDFLAGS="-L$HOME/android-libs/lib" \
LIBS="-lvorbisfile -lvorbis -logg" \
./configure --host=android-arm64-v8a --enable-openxr --enable-vorbis \
  --disable-all-engines --enable-engine=scumm

# Add OpenXR paths to config.mk
cat >> config.mk << 'EOF'
OPENXR_CFLAGS := -I$(HOME)/OpenXR-SDK/include
OPENXR_LIBS := -L$(HOME)/OpenXR-SDK/build-android/src/loader -lopenxr_loader
CXXFLAGS += $(OPENXR_CFLAGS)
LIBS += $(OPENXR_LIBS)
LIBS += -lGLESv3
EOF

# Build native library
make -j$(nproc)

# Set up VR APK project
mkdir -p android_project_vr/lib/arm64-v8a
cp android_project/lib/arm64-v8a/libscummvm.so android_project_vr/lib/arm64-v8a/
cp OpenXR-SDK/build-android/src/loader/libopenxr_loader.so android_project_vr/lib/arm64-v8a/
cp $ANDROID_NDK_ROOT/sources/cxx-stl/llvm-libc++/libs/arm64-v8a/libc++_shared.so android_project_vr/lib/arm64-v8a/

# Copy Gradle files from standard Android project
cp -r android_project/gradle android_project_vr/
cp android_project/gradlew android_project_vr/
cp android_project/settings.gradle android_project_vr/
cp android_project/gradle.properties android_project_vr/
cp android_project/mainAssets android_project_vr/ -r
echo "sdk.dir=$ANDROID_SDK_ROOT" > android_project_vr/local.properties
echo "srcdir=$(pwd)" > android_project_vr/src.properties
cp dists/android-vr/build.gradle android_project_vr/

# Build APK
cd android_project_vr && ./gradlew assembleDebug

# Install to Quest
adb install build/outputs/apk/debug/ScummVM-debug.apk
```

### Enable more game engines

Replace `--disable-all-engines --enable-engine=scumm` with `--enable-all-engines` in the configure step to support all 2000+ games.

## How It Works

ScummVR adds a VR rendering layer to ScummVM's existing Android backend:

- **NativeActivity** provides the entry point (`android_main`) required by Quest's OpenXR runtime
- **OpenXR** handles stereo rendering, head tracking, and controller input
- **ScummVM** runs on a background thread with a shared OpenGL ES context
- ScummVM renders to a 1920x1080 PBuffer, which is blitted to a shared texture
- The VR main loop renders this texture on a virtual screen quad for each eye
- Controller ray-plane intersection maps aim pose to screen coordinates

## License

Same as ScummVM — [GNU General Public License v3.0](LICENSE).

## Credits

- [ScummVM](https://www.scummvm.org/) — the incredible engine reimplementation project
- [OpenXR](https://www.khronos.org/openxr/) — Khronos Group's VR/AR API standard
- [quest_xr](https://github.com/cshenton/quest_xr) — minimal Quest OpenXR reference
- Built with Claude Code
