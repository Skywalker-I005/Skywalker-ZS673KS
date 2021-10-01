![NieR Kaine](build/NieRKaine.png)

# Compiling on MacOS (Mojave or lower)

*All codeblocks are to be executed in a Terminal window*


## "Installing package managers"

Mojave - Xcode 11.3.1

Install [Xcode.app](https://developer.apple.com/download/all/) (required for headers)

```
xcode-select --install
sudo installer -pkg /Library/Developer/CommandLineTools/Packages/macOS_SDK_headers_for_macOS_10.14.pkg -target /
```

Install [Homebrew](https://brew.sh/)

```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Reboot the system


## "Set up build environment"

```
brew install pyenv
brew install curl findutils coreutils gnu-tar gnu-sed libelf
```


## "Case Sensitive disk image"

Open Disk Utility (Applications -> Utilities -> Disk Utility)

Create a Sparse Image (File -> New Image -> Blank Image)

Set the Image Properties
* Save As: android
* Name: Android
* Size: 50 GB (or more, if working with multiple kernels)
* Format: Mac OS Extended (Case-sensitive, Journaled)

Double clicking the newly created image file will mount it. This image can be moved (or created) on an external drive, so automatically mounting it per the original guide is not recommended.


## "Setting the PATH"

This is done automatically through /etc/paths and activated by the reboot

Additional overrides are done on a per-kernel basis to avoid system corruption. See [build_kernel.sh](build_kernel.sh)


## "Getting the Toolchain"

https://android.googlesource.com/platform/prebuilts

```
cd /Volumes/Android
mkdir toolchains
cd toolchains
git clone https://android.googlesource.com/platform/prebuilts/clang/host/darwin-x86
git clone https://android.googlesource.com/platform/prebuilts/gcc/darwin-x86/aarch64/aarch64-linux-android-4.9
git clone https://android.googlesource.com/platform/prebuilts/gcc/darwin-x86/arm/arm-linux-androideabi-4.9
```


## "Errors..."

```
cat <<EOT >> /usr/local/include/elf.h
#include "../opt/libelf/include/libelf/gelf.h"
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_ARM_NONE 0
#define R_ARM_PC24 1
#define R_ARM_ABS32 2
#define R_MIPS_NONE 0
#define R_MIPS_16 1
#define R_MIPS_32 2
#define R_MIPS_REL32 3
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6
#define R_IA64_IMM64 0x23 /* symbol + addend, mov imm64 */
#define R_PPC_ADDR32 1 /* 32bit absolute address */
#define R_PPC64_ADDR64 38 /* doubleword64 S + A */
#define R_SH_DIR32 1
#define R_SPARC_64 32 /* Direct 64 bit */
#define R_X86_64_64 1 /* Direct 64 bit */
#define R_390_32 4 /* Direct 32 bit. */
#define R_390_64 22 /* Direct 64 bit. */
#define R_MIPS_64 18
#define EF_ARM_EABIMASK 0XFF000000
#define EF_ARM_EABI_VERSION(flags) ((flags) & EF_ARM_EABIMASK)
EOT
```

```
ln -s /usr/include/machine/endian.h /usr/local/include/endian.h
```

Updated and revised from [Building the android kernel (Mac OS)](https://forum.xda-developers.com/t/tutorial-reference-building-the-android-kernel-mac-os.3856676)

## Additional Notes

Android no longer supports building on Darwin (MacOS), so a few kernel changes may also be required


https://github.com/Skywalker-I005/Skywalker-ZS673KS/commit/e9351d4f7f0bb696107c76d2cfde255417fdb0ce

```
commit e9351d4f7f0bb696107c76d2cfde255417fdb0ce
Author: Abandoned Cart <t*************a@gmail.com>
Date:   Tue Sep 14 08:25:02 2021 -0400

    Update build configuration for LLVM

diff --git a/scripts/mod/file2alias.c b/scripts/mod/file2alias.c
index c91eba751804..e756fd80b721 100644
--- a/scripts/mod/file2alias.c
+++ b/scripts/mod/file2alias.c
@@ -38,6 +38,9 @@ typedef struct {
        __u8 b[16];
 } guid_t;

+#ifdef __APPLE__
+#define uuid_t compat_uuid_t
+#endif
 /* backwards compatibility, don't use in new code */
 typedef struct {
        __u8 b[16];
```


https://github.com/Skywalker-I005/Skywalker-ZS673KS/commit/0a75e7fa46248ff0fd51a77d3cc129ab68aa272b

&nbsp;&nbsp;&nbsp;&nbsp;Patch requires https://github.com/Skywalker-I005/Skywalker-ZS673KS/tree/master/darwin/

```
commit 0a75e7fa46248ff0fd51a77d3cc129ab68aa272b
Author: Abandoned Cart <t*************a@gmail.com>
Date:   Mon Jun 14 12:22:22 2021 -0400

    Include Darwin header workaround

diff --git a/Makefile b/Makefile
index d34ea45a503e..ce1deda42be9 100644
--- a/Makefile
+++ b/Makefile
@@ -408,6 +408,9 @@ endif
 KBUILD_HOSTCFLAGS   := -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 \
                -fomit-frame-pointer -std=gnu89 $(HOST_LFS_CFLAGS) \
                $(HOSTCFLAGS)
+ifeq ($(shell uname),Darwin)
+KBUILD_HOSTCFLAGS    += -I$(srctree)/darwin/include
+endif
 KBUILD_HOSTCXXFLAGS := -O2 $(HOST_LFS_CFLAGS) $(HOSTCXXFLAGS)
 KBUILD_HOSTLDFLAGS  := $(HOST_LFS_LDFLAGS) $(HOSTLDFLAGS)
 KBUILD_HOSTLDLIBS   := $(HOST_LFS_LIBS) $(HOSTLDLIBS)
```


https://github.com/Skywalker-I005/Skywalker-ZS673KS/commit/7dbd0a3fc84d93822c3f677675a1e33585f18f89

```
commit 7dbd0a3fc84d93822c3f677675a1e33585f18f89
Author: Abandoned Cart <t*************a@gmail.com>
Date:   Tue Sep 14 06:13:50 2021 -0400

    openssl requires direct reference

diff --git a/scripts/Makefile b/scripts/Makefile
index b4b7d8b58cd6..cc2c2580e480 100644
--- a/scripts/Makefile
+++ b/scripts/Makefile
@@ -10,6 +10,10 @@

 HOST_EXTRACFLAGS += -I$(srctree)/tools/include

+ifeq ($(shell uname),Darwin)
+HOST_EXTRACFLAGS += -I/usr/local/opt/openssl@1.1/include -L/usr/local/opt/openssl@1.1/lib
+endif
+
 CRYPTO_LIBS = $(shell pkg-config --libs libcrypto 2> /dev/null || echo -lcrypto)
 CRYPTO_CFLAGS = $(shell pkg-config --cflags libcrypto 2> /dev/null)
```
