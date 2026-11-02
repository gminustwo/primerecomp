# tools/android/env.sh, sourced by the other tools/android scripts (from the repo root): the Android SDK, NDK and JDK
# to build with. $ANDROID_HOME / $NDK / $JAVA_HOME when set, else what tools/android/setup_sdk.sh put under
# build/android, else Android Studio's: its SDK (SDK Manager: Android SDK Platform 35, NDK 30) and the JDK it ships.
# Sets A (build/android), SDK, NDK, JDK, HOST (the NDK's prebuilt host directory), TC (its clang / llvm tools).
A=$PWD/build/android
first_dir() { for d in "$@"; do [ -n "$d" ] && [ -d "$d" ] && { echo "$d"; return; }; done; }
case "$(uname -s)" in
    MINGW*|MSYS*) HOST=windows-x86_64; AS_SDK=${LOCALAPPDATA:+$(cygpath -u "$LOCALAPPDATA")/Android/Sdk}
                  AS_JBR="/c/Program Files/Android/Android Studio/jbr" ;;
    Darwin)       HOST=darwin-x86_64; AS_SDK=$HOME/Library/Android/sdk
                  AS_JBR="/Applications/Android Studio.app/Contents/jbr/Contents/Home" ;;
    *)            HOST=linux-x86_64; AS_SDK=$HOME/Android/Sdk; AS_JBR=/opt/android-studio/jbr ;;
esac
SDK=${ANDROID_HOME:-$(first_dir "$A/sdk" "$AS_SDK")}
[ -n "$SDK" ] || { echo "no Android SDK: install Android Studio, or run tools/android/setup_sdk.sh"; exit 1; }
NDK=${NDK:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -n "$NDK" ] || { echo "no NDK in $SDK: Android Studio > SDK Manager > SDK Tools > NDK (Side by side)"; exit 1; }
JDK=${JAVA_HOME:-$(first_dir "$A/jdk" "$AS_JBR")}
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
