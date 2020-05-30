#!/bin/bash
#
# This is an easy-bake script to download and build all UnifyFS's dependencies.
#

ROOT="$(pwd)"

mkdir -p deps
mkdir -p install
INSTALL_DIR=$ROOT/install

if [ ! -e $INSTALL_DIR/lib/libunifyfs_gotcha.so ] ; then
    echo "Please ./boostrap.sh and 'make' and 'make install' UnifyFS first"
    exit
fi

cd deps

repos=(https://github.com/tonyhutter/busybox.git
)

for i in "${repos[@]}" ; do
	# Get just the name of the project (like "mercury")
	name=$(basename $i | sed 's/\.git//g')
	if [ -d $name ] ; then
		echo "$name already exists, skipping it"
	else
		git clone $i
	fi
done

echo "### building busybox ###"
cd busybox
cp -a ../../busybox-files/* .
cp -a ../../busybox-files/.config .

# Some additional config
sed -i '/CONFIG_EXTRA_CFLAGS/d; /CONFIG_EXTRA_LDFLAGS/d; /CONFIG_EXTRA_LDLIBS/d' .config
echo "CONFIG_EXTRA_CFLAGS=\"-I$INSTALL_DIR/include\"" >> .config
echo "CONFIG_EXTRA_LDFLAGS=\"-L$INSTALL_DIR/lib -L$INSTALL_DIR/lib64 -Wl,-rpath,$INSTALL_DIR/lib -Wl,-rpath,$INSTALL_DIR/lib64 \"" >> .config
echo "CONFIG_EXTRA_LDLIBS=\"-lunifyfs_gotcha -lgotcha\"" >> .config 

yes ""  | make oldconfig
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$INSTALL_DIR/lib:$INSTALL_DIR/lib64 make -j $(nproc)
cd ..

cd "$ROOT"

echo "*************************************************************************"
echo "busybox is built."
echo "*************************************************************************"
