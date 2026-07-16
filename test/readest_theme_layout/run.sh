#!/bin/sh
set -eu

cd "$(dirname "$0")"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic -I../../src ReadestThemeLayoutTest.cpp -o ReadestThemeLayoutTest
./ReadestThemeLayoutTest
rm ReadestThemeLayoutTest
