#!/usr/bin/env bash

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'

cd $(dirname ${BASH_SOURCE[0]})/

# download dpdk
if [ "$#" -ne 1 ];
then
	export RTE_SDK=$PWD/dpdk
else
	export RTE_SDK=$1
fi

printf "${GREEN}Checking DPDK version...\n$NC"

# Compile DPDK
if [ -f "$RTE_SDK/meson.build" ]; then
	printf "${GREEN}Detected new DPDK verision(Meson build system)\n$NC"

	export RTE_TARGET=build
	BUILD_DIR=$RTE_SDK/$RTE_TARGET


	if [ ! -f "$BUUILD_DIR/build.ninja" ]; then
		printf "${GREEN}Configuring DPDK with Meson...\n$NC"
		cd $RTE_SDK
		meson setup $RTE_TARGET
		cd -
	fi

	printf "${GREEN}Building DPDK with Ninja...\n$NC"
	cd $RTE_SDK
	ninja -C $RTE_TARGET
	cd -
fi

printf "${GREEN}Setting up environment variables...\n$NC"
cd $RTE_SDK

if [ -f "meson.build" ]; then
	export RTE_TARGET=build
	export PKG_CONFIG_PATH=$RTE_SDK/$RTE_TARGET/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
	export LD_LIBRARY_PATH=$RTE_SDK/$RTE_TARGET/lib/x86_64-linux-gnu:$LD_LIBRARTY_PATH
fi

cd -
printf "Set ${GREEN}RTE_SDK$NC env variable as $RTE_SDK\n"
printf "Set ${GREEN}RTE_TARGET$NC env variable as $RTE_TARGET\n"

if [ -f "$RTE_SDK/meson.build" ]; then
	printf "${GREEN}For DPDK appliucations, use: pkg-config --libs --cflags libdpdk\n$NC"
	printf "${GREEN}Or set: PKG_CONFIG_PATH=$RTE_SDK/$RTE_TARGET/lib/x856_64-linux-gnu/pkgconfig\n$NC"
fi

# CHeck if you are using an Intel NIC
while true; do
	read -p "Are you using an Inel NIC (y/n)? " response
	case $response in
		[Yy]* )
			printf "Creating ${GREEN}dpdk$NC interface entries\n"
			if [ -d "dpdk-iface-kmod" ]; then
				cd dpdk-iface-kmod
				if [ -f "Makefile" ]; then
					make
					if lsmod | grep dpdk_iface &> /dev/null ; then
						:
					else
						sudo insmod ./dpdk_iface.ko
					fi
					sudo -E make run
				fi
				cd ..
			else
				printf "${YELLOW}dpdk-iface-kmod directory not found, skipping interface creation\n$NC"
			fi
			break;;
		[Nn]* )
			exit;;
		* )
			printf "${RED}Please anser y or n\n$NC";;
	esac
done

printf "${GREEN}DPDK setup completed successfully!\n$NC"



