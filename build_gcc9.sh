#!/bin/bash
rm .version
# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

clear

# Resources
THREAD="-j$(grep -c ^processor /proc/cpuinfo)"
KERNEL="Image"
DTBIMAGE="dtb"

#export CLANG_PATH=~/android/Toolchains/clang/clang-r328903/bin/
#export PATH=${CLANG_PATH}:${PATH}
#export CLANG_TRIPLE=aarch64-linux-gnu-
export SPL="2019-05"
export CROSS_COMPILE=${HOME}/android/Toolchains/gcc9arm64/bin/aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=${HOME}/android/Toolchains/gcc9eabi/bin/arm-eabi-
#export KBUILD_COMPILER_STRING=$(~/android/Toolchains/clang/clang-r328903/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')
DEFCONFIG="smurf_defconfig"

# Kernel Details
VER=".1.3.0t2"

# Paths
KERNEL_DIR=`pwd`
REPACK_DIR="${HOME}/android/AnyKernel2"
PATCH_DIR="${HOME}/android/AnyKernel2/patch"
MODULES_DIR="${HOME}/android/AnyKernel2/modules"
ZIP_MOVE="${HOME}/android/AK-releases"
ZIMAGE_DIR="${HOME}/android/SmurfKernelOP7/arch/arm64/boot/"

# Functions
function clean_all {
		#ccache -C
		rm -rf $MODULES_DIR/*
		rm -rf ~/android/SmurfKernelOP7/out/*
		#git reset --hard > /dev/null 2>&1
		#git clean -f -d > /dev/null 2>&1
		cd $KERNEL_DIR
		echo
		make clean && make mrproper
}

function make_kernel {
	      cp ~/android/SmurfKernelOP7/Makefile.gcc9 ~/android/SmurfKernelOP7/Makefile
	      echo
              make ARCH=arm64 O=out $DEFCONFIG
              make ARCH=arm64 O=out $THREAD
}

function make_modules {
		find $KERNEL_DIR -name '*.ko' -exec cp -v {} $MODULES_DIR \;
}

function make_dtb {
		$REPACK_DIR/tools/dtbToolCM -2 -o $REPACK_DIR/$DTBIMAGE -s 2048 -p scripts/dtc/ arch/arm64/boot/
}

function make_boot {
		cp -vr ~/android/SmurfKernelOP7/out/arch/arm64/boot/Image-dtb ~/android/AnyKernel2/Image-dtb
}

function move_boot {
		mv ~/android/AnyKernel2/Image-dtb ~/android/AnyKernel2/oos/
}

function make_zip {
		cd ~/android/AnyKernel2
		zip -r9 `echo $AK_VER`.zip *
		mv  `echo $AK_VER`.zip $ZIP_MOVE
		cd $KERNEL_DIR
}


DATE_START=$(date +"%s")


echo -e "${green}"
echo "-----------------"
echo "Making Kernel:"
echo "-----------------"
echo -e "${restore}"


# Vars
BASE_AK_VER="SmurfKernel"
AK_VER="$BASE_AK_VER$VER"
export LOCALVERSION=~`echo $AK_VER`
export LOCALVERSION=~`echo $AK_VER`
export ARCH=arm64
export SUBARCH=arm64
export KBUILD_BUILD_USER=pappschlumpf
export KBUILD_BUILD_HOST=Gargamel

echo

while read -p "Do you want to clean stuffs (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		clean_all
		echo
		echo "All Cleaned now."
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done

echo

while read -p "Do you want to build?" dchoice
do
case "$dchoice" in
	y|Y )
		make_kernel
		make_modules
		make_boot
		move_boot
		make_zip
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done


echo -e "${green}"
echo "-------------------"
echo "Build Completed in:"
echo "-------------------"
echo -e "${restore}"

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))
echo "Time: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo
