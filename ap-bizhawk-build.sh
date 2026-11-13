#!/bin/sh

# terminate the script if any commands return a non-zero error code
set -e

echo "************************************ Creating directories"
rm -rf ap-bizhawk-source
mkdir ap-bizhawk-source

if [ -z "$M64P_COMPONENTS" ]; then
	M64P_COMPONENTS="core rom ui-console audio-sdl input-sdl rsp-hle video-rice video-glide64mk2"
fi

for component in ${M64P_COMPONENTS}; do
	repo_prefix="mupen64plus"
	repo_branch="master"
	if [ "${component}" = "core" ]; then
		component_type="library"
		repo_prefix="Ryan1729"
		repo_branch="Ryan1729/AP-BizHawk"
	elif  [ "${component}" = "rom" ]; then
		component_type=""
	elif  [ "${component}" = "ui-console" ]; then
		component_type="front-end"
		repo_prefix="Ryan1729"
		repo_branch="Ryan1729/AP-BizHawk"
	else
		component_type="plugin"
	fi

	echo "************************************ Downloading ${component} ${component_type}"
	git clone https://github.com/$repo_prefix/mupen64plus-${component}.git ap-bizhawk-source/mupen64plus-${component} -b $repo_branch --single-branch $@
done


if [ -z "$MAKE" ]; then
	MAKE=make
fi
if [ -z "$M64P_COMPONENTS" ]; then
	M64P_COMPONENTS="core rom ui-console audio-sdl input-sdl rsp-hle video-rice video-glide64mk2"
fi
if [ -z "$FEATURE_FLAGS" ]; then
	FEATURE_FLAGS="AP_BIZHAWK=1"
fi

mkdir -p ./build/
MAKE_INSTALL="PLUGINDIR= SHAREDIR= BINDIR= MANDIR= LIBDIR= APPSDIR= ICONSDIR=icons INCDIR=api LDCONFIG=true "

for component in ${M64P_COMPONENTS}; do
	if [ "${component}" = "core" ]; then
		component_type="library"
    elif  [ "${component}" = "rom" ]; then
		echo "************************************ Building test ROM"
		cp ap-bizhawk-source/mupen64plus-rom/m64p_test_rom.v64 ./build/
		continue
	elif  [ "${component}" = "ui-console" ]; then
		component_type="front-end"
	else
		component_type="plugin"
	fi

	echo "************************************ Building ${component} ${component_type}"
	if [ -n "$CLEAN" ]; then
		"$MAKE" -C ap-bizhawk-source/mupen64plus-${component}/projects/unix clean $@
	fi
	"$MAKE" -C ap-bizhawk-source/mupen64plus-${component}/projects/unix all "$FEATURE_FLAGS" $@
	"$MAKE" -C ap-bizhawk-source/mupen64plus-${component}/projects/unix install $@ ${MAKE_INSTALL} DESTDIR="$(pwd)/build/"

	mkdir -p ./build/doc
	for doc in LICENSES README RELEASE; do
		if [ -e "ap-bizhawk-source/mupen64plus-${component}/${doc}" ]; then
			cp "ap-bizhawk-source/mupen64plus-${component}/${doc}" "./build/doc/${doc}-mupen64plus-${component}"
		fi
	done
	for subdoc in gpl-license font-license lgpl-license module-api-versions.txt; do
		if [ -e "ap-bizhawk-source/mupen64plus-${component}/doc/${subdoc}" ]; then
			cp "ap-bizhawk-source/mupen64plus-${component}/doc/${subdoc}" ./build/doc/
		fi
	done
done

