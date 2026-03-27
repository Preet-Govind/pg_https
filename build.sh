#!/usr/bin/env bash

# set -e

echo ">>>>>> Building pg_https..."

make clean || true
rm -rf src/*.so src/*.bc *.bc *.so
make CC=gcc VERBOSE=1

echo ">>>>>> Installing..."
sudo make install

echo ">>>>>> Restarting PostgreSQL..."
sudo systemctl restart postgresql || true

# echo ">>>>>> Done!"

echo ">>>>>> Run:"
echo "CREATE EXTENSION pg_https;"