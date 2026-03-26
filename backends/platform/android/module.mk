MODULE := backends/platform/android

MODULE_OBJS := \
	jni-android.o \
	asset-archive.o \
	android.o \
	events.o \
	options.o \
	snprintf.o \
	touchcontrols.o

ifdef NEED_ANDROID_CPUFEATURES
MODULE_OBJS += \
	cpu-features.o
$(MODULE)/android.o: CXXFLAGS += "-I$(ANDROID_NDK_ROOT)/sources/android/cpufeatures"
# We don't configure a C compiler, use a C++ one in C mode
$(MODULE)/cpu-features.o: $(ANDROID_NDK_ROOT)/sources/android/cpufeatures/cpu-features.c
	$(QUIET)$(MKDIR) $(*D)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) -x c -std=c99 -c $(<) -o $@
endif

# We don't use rules.mk but rather manually update OBJS and MODULE_DIRS.
MODULE_OBJS := $(addprefix $(MODULE)/, $(MODULE_OBJS))
OBJS := $(MODULE_OBJS) $(OBJS)
MODULE_DIRS += $(sort $(dir $(MODULE_OBJS)))

ifdef USE_OPENXR
VR_MODULE := backends
VR_OBJS := \
	graphics/android-vr/android-vr-graphics.o \
	platform/android-vr/openxr-session.o \
	platform/android-vr/openxr-input.o \
	platform/android-vr/vr-screen-geometry.o \
	platform/android-vr/android-vr-main.o \
	platform/android-vr/android_native_app_glue.o
VR_OBJS := $(addprefix $(VR_MODULE)/, $(VR_OBJS))
OBJS := $(VR_OBJS) $(OBJS)
MODULE_DIRS += $(sort $(dir $(VR_OBJS)))

# android_native_app_glue comes from the NDK
backends/platform/android-vr/android_native_app_glue.o: $(ANDROID_NDK_ROOT)/sources/android/native_app_glue/android_native_app_glue.c
	$(QUIET)$(MKDIR) $(*D)
	$(QUIET_CXX)$(CXX) $(filter-out -std=gnu++11,$(CXXFLAGS)) $(CPPFLAGS) -x c -std=c99 -c $(<) -o $@

backends/platform/android-vr/android-vr-main.o: CXXFLAGS += -I$(ANDROID_NDK_ROOT)/sources/android/native_app_glue
endif
