#!/bin/bash

export ASUS_BUILD_PROJECT="ZS673KS"
export TARGET_BUILD_VARIANT="user"

PLATFORM=$(uname -s | tr '[:upper:]' '[:lower:]')-x86
if [ $PLATFORM == "darwin-x86" ]; then
    export LC_CTYPE=C
	export PATH="/usr/local/opt/curl/bin:$PATH"
	export PATH="/usr/local/opt/findutils/libexec/gnubin:$PATH"
	export PATH="/usr/local/opt/coreutils/libexec/gnubin:$PATH"
	export PATH="/usr/local/opt/openssl@1.1/bin:$PATH"
	export PATH="/usr/local/opt/gnu-tar/libexec/gnubin:$PATH"
	export PATH="/usr/local/opt/gnu-sed/libexec/gnubin:$PATH"
	export PKG_CONFIG_PATH=/"usr/local/opt/openssl@1.1/lib/pkgconfig:$PKG_CONFIG_PATH"

    BUILDROOT=/Volumes/Android
    PREBUILT=$BUILDROOT/$PLATFORM/toolchains
    CLANGVER=clang-r416183d
    CC_BIN=${PREBUILT}/aarch64-linux-android-4.9/bin
    CCC_BIN=${PREBUILT}/arm-linux-androideabi-4.9/bin
else
	BUILDROOT=/mnt/hgfs/Android
    PREBUILT=$BUILDROOT/$PLATFORM/toolchains
    CLANGVER=clang-r416183d
fi
CLANG_BIN=${PREBUILT}/$PLATFORM/$CLANGVER/bin

cd $BUILDROOT/SAUS-ZS673KS

echo
echo "Clean Repository"
echo

make clean & make mrproper
git checkout -- *.i
#git clean -xfd # failsafe

find . -name "*.orig" -type f -delete

if [ -d out ]; then
	rm -rf out;
fi

if [ -f "release/dtb" ]; then
	rm release/dtb;
fi
if [ -f "release/dtbo.img" ]; then
    rm release/dtbo.img;
fi
if [ -f "release/Image.gz" ]; then
    rm release/Image.gz
fi
if [ -f "release/Image" ]; then
    rm release/Image
fi
if compgen -G "release/modules/system/vendor/lib/modules/*.ko" > /dev/null; then
	rm release/modules/system/vendor/lib/modules/*.ko;
fi
#find "release/modules/system/vendor/lib/modules" -name "*.ko" -type f -delete

if compgen -G "release/ZS673KS-*-overlay.dtbo" > /dev/null; then
	rm release/ZS673KS-*-overlay.dtbo;
fi
#find "release" -name "ZS673KS-*-overlay.dtbo" -type f -delete

if compgen -G "release/*.zip" > /dev/null; then
	rm release/*.zip;
fi
#find "release" -name "*.zip" -type f -delete

echo
echo "Configure Build"
echo

mkdir -p out
mkdir -p release/modules/system/vendor/lib/modules

if [ $PLATFORM == "darwin-x86" ]; then
    export PATH=${CLANG_BIN}:${CC_BIN}:${CCC_BIN}:${PATH}
    export LD_LIBRARY_PATH=${PREBUILT}/$PLATFORM/$CLANGVER/lib64:$LD_LIBRARY_PATH
    export CLANG_TRIPLE=aarch64-linux-gnu-
    export CROSS_COMPILE=aarch64-linux-android-
    export CROSS_COMPILE_COMPAT=arm-linux-androideabi-
    #export DTC_EXT=${PREBUILT}/prebuilts-master/dtc
else
    export PATH=${CLANG_BIN}:${PATH}
    export CROSS_COMPILE=aarch64-linux-gnu-
    export CROSS_COMPILE_COMPAT=arm-linux-gnueabi-
fi

echo
echo "Set DEFCONFIG"
echo 

if [ $PLATFORM == "darwin-x86" ]; then
    make LLVM=1 vendor/SAUS-perf_defconfig
else
    make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip vendor/SAUS-perf_defconfig
fi

echo
echo "Compile Source"
echo

CORES=$([ $(uname) = 'Darwin' ] && sysctl -n hw.logicalcpu_max || lscpu -p | egrep -v '^#' | wc -l)
THREADS=$([ $(uname) = 'Darwin' ] && sysctl -n hw.physicalcpu_max || lscpu -p | egrep -v '^#' | sort -u -t, -k 2,4 | wc -l)
CPU_JOB_NUM=$(expr $CORES \* $THREADS)

if [ $PLATFORM == "darwin-x86" ]; then
    make LLVM=1 -j$CPU_JOB_NUM
else
    make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip -j$CPU_JOB_NUM
fi

cat out/arch/arm64/boot/dts/vendor/qcom/lahaina-v2.1.dtb \
   out/arch/arm64/boot/dts/vendor/qcom/lahaina-v2.dtb \
   out/arch/arm64/boot/dts/vendor/qcom/lahaina.dtb \
   > out/arch/arm64/boot/dtb

echo
echo "Package Kernel"
echo

if [ -f out/arch/arm64/boot/Image ]; then
	cp -f out/arch/arm64/boot/dtb release/
    if [ -f out/arch/arm64/boot/Image.gz ]; then
        cp -f out/arch/arm64/boot/Image.gz release/
    else
        cp -f out/arch/arm64/boot/Image release/
    fi
	find out -type f -name "*.ko" -exec cp -Rf "{}" release/modules/system/vendor/lib/modules/ \;
    if [ -f out/arch/arm64/boot/dtbo.img ]; then
        cp -f out/arch/arm64/boot/dtbo.img release/
    else
        find out/arch/arm64/boot/dts/vendor/qcom -type f -name "*.dtbo" -exec cp -Rf "{}" release/ \;
    fi

	VERSION=$(cat build/firmware_build)
    HASH=$(git rev-parse --short HEAD)
	
	cd release
	zip -r9 "SAUS-ZS673KS-$VERSION-$HASH.zip" * -x *.DS_Store .git* README.md
	cd ../
fi
