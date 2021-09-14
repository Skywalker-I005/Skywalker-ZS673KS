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
