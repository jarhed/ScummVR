# Android VR specific build targets (Meta Quest)
PATH_DIST_VR = $(srcdir)/dists/android-vr

PATH_BUILD_VR = ./android_project_vr
PATH_BUILD_VR_LIB = $(PATH_BUILD_VR)/lib/$(ABI)
PATH_BUILD_VR_LIBSCUMMVM = $(PATH_BUILD_VR)/lib/$(ABI)/libscummvm.so

APK_VR_MAIN = ScummVM-VR-debug.apk
APK_VR_MAIN_RELEASE = ScummVM-VR-release-unsigned.apk

# Reuse the standard android project setup but with VR-specific files
$(PATH_BUILD_VR):
	$(MKDIR) $(PATH_BUILD_VR)

$(PATH_BUILD_VR)/build.gradle: $(PATH_DIST_VR)/build.gradle | $(PATH_BUILD_VR)
	$(INSTALL) -c -m 644 $< $@

$(PATH_BUILD_VR)/settings.gradle: $(PATH_DIST)/settings.gradle | $(PATH_BUILD_VR)
	$(INSTALL) -c -m 644 $< $@

$(PATH_BUILD_VR)/gradle.properties: $(PATH_DIST)/gradle.properties | $(PATH_BUILD_VR)
	$(INSTALL) -c -m 644 $< $@

$(PATH_BUILD_VR)/local.properties: configure.stamp | $(PATH_BUILD_VR)
	$(ECHO) "sdk.dir=$(realpath $(ANDROID_SDK_ROOT))\n" > $@

$(PATH_BUILD_VR)/src.properties: configure.stamp | $(PATH_BUILD_VR)
	$(ECHO) "srcdir=$(realpath $(srcdir))\n" > $@

$(PATH_BUILD_VR)/gradle/.timestamp: $(GRADLE_FILES) | $(PATH_BUILD_VR)
	$(MKDIR) $(PATH_BUILD_VR)/gradle
	$(CP) -r $(PATH_DIST)/gradle/. $(PATH_BUILD_VR)/gradle/
	touch "$@"

$(PATH_BUILD_VR)/gradlew: $(PATH_DIST)/gradlew | $(PATH_BUILD_VR)
	$(INSTALL) -c -m 755 $< $@

$(PATH_BUILD_VR_LIBSCUMMVM): libscummvm.so | $(PATH_BUILD_VR)
	$(INSTALL) -d $(PATH_BUILD_VR_LIB)
	$(INSTALL) -c -m 644 libscummvm.so $(PATH_BUILD_VR_LIBSCUMMVM)

PATH_BUILD_VR_GRADLE = $(PATH_BUILD_VR)/gradle/.timestamp $(PATH_BUILD_VR)/gradlew $(PATH_BUILD_VR)/build.gradle $(PATH_BUILD_VR)/settings.gradle $(PATH_BUILD_VR)/gradle.properties $(PATH_BUILD_VR)/local.properties $(PATH_BUILD_VR)/src.properties

# Reuse assets from the standard build
PATH_BUILD_VR_ASSETS = $(PATH_BUILD_VR)/mainAssets/src/main/assets

$(PATH_BUILD_VR)/mainAssets/build.gradle: $(PATH_DIST)/mainAssets.gradle | $(PATH_BUILD_VR_ASSETS)/MD5SUMS
	$(INSTALL) -c -m 644 $< $@

$(PATH_BUILD_VR_ASSETS)/MD5SUMS: $(PATH_BUILD_ASSETS)/MD5SUMS | $(PATH_BUILD_VR)
	$(MKDIR) $(PATH_BUILD_VR_ASSETS)
	$(CP) -r $(PATH_BUILD_ASSETS)/. $(PATH_BUILD_VR_ASSETS)/

$(APK_VR_MAIN): $(PATH_BUILD_VR_GRADLE) $(PATH_BUILD_VR_ASSETS)/MD5SUMS $(PATH_BUILD_VR_LIBSCUMMVM) $(PATH_BUILD_VR)/mainAssets/build.gradle | $(PATH_BUILD_VR)
	(cd $(PATH_BUILD_VR); ./gradlew assembleDebug)
	$(CP) $(PATH_BUILD_VR)/build/outputs/apk/debug/$(APK_VR_MAIN) $@

$(APK_VR_MAIN_RELEASE): $(PATH_BUILD_VR_GRADLE) $(PATH_BUILD_VR_ASSETS)/MD5SUMS $(PATH_BUILD_VR_LIBSCUMMVM) $(PATH_BUILD_VR)/mainAssets/build.gradle | $(PATH_BUILD_VR)
	(cd $(PATH_BUILD_VR); ./gradlew assembleRelease)
	$(CP) $(PATH_BUILD_VR)/build/outputs/apk/release/$(APK_VR_MAIN_RELEASE) $@

android-vr-debug: $(APK_VR_MAIN)
android-vr-release: $(APK_VR_MAIN_RELEASE)

android-vr-test: $(APK_VR_MAIN)
	adb install -r $(APK_VR_MAIN)

android-vr-clean:
	$(RM) -rf $(PATH_BUILD_VR) $(APK_VR_MAIN) $(APK_VR_MAIN_RELEASE)

.PHONY: android-vr-debug android-vr-release android-vr-test android-vr-clean
