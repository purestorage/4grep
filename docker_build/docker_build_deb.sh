mkdir build
cp -r * build
cd build
make
dch -v "1.0.0-$COMMIT_COUNT" "$COMMIT_HASH"
debuild -i -I -us -uc -b
cd ..
rm -rf build
