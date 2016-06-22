# Copyright 2015 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

################################################################################
# User-servicable parts:
################################################################################

# C/C++ compiler -- clang also works
CXX ?= g++
CC ?= gcc

# Emscripten -- For Jsonlang in the browser
EMCXX ?= em++
EMCC ?= emcc

CP ?= cp
OD ?= od

OPT ?= -O3

CXXFLAGS ?= -g $(OPT) -Wall -Wextra -Woverloaded-virtual -pedantic -std=c++0x -fPIC -Iinclude
CFLAGS ?= -g $(OPT) -Wall -Wextra -pedantic -std=c99 -fPIC -Iinclude
EMCXXFLAGS = $(CXXFLAGS) --memory-init-file 0 -s DISABLE_EXCEPTION_CATCHING=0
EMCFLAGS = $(CFLAGS) --memory-init-file 0 -s DISABLE_EXCEPTION_CATCHING=0
LDFLAGS ?=

SHARED_LDFLAGS ?= -shared

################################################################################
# End of user-servicable parts
################################################################################

LIB_SRC = \
	core/desugarer.cpp \
	core/formatter.cpp \
	core/lexer.cpp \
	core/libjsonlang.cpp \
	core/parser.cpp \
	core/static_analysis.cpp \
	core/string_utils.cpp \
	core/vm.cpp

LIB_OBJ = $(LIB_SRC:.cpp=.o)

LIB_CPP_SRC = \
	cpp/libjsonlang++.cpp

LIB_CPP_OBJ = $(LIB_OBJ) $(LIB_CPP_SRC:.cpp=.o)

ALL = \
	jsonlang \
	libjsonlang.so \
	libjsonlang++.so \
	libjsonlang_test_snippet \
	libjsonlang_test_file \
	libjsonlang.js \
	doc/js/libjsonlang.js \
	$(LIB_OBJ)

ALL_HEADERS = \
	core/ast.h \
	core/desugarer.h \
	core/formatter.h \
	core/lexer.h \
	core/parser.h \
	core/state.h \
	core/static_analysis.h \
	core/static_error.h \
	core/string_utils.h \
	core/vm.h \
	core/std.jsonlang.h \
	include/libjsonlang.h \
	include/libjsonlang++.h

default: jsonlang

all: $(ALL)

TEST_SNIPPET = "std.assertEqual(({ x: 1, y: self.x } { x: 2 }).y, 2)"
test: jsonlang libjsonlang.so libjsonlang_test_snippet libjsonlang_test_file
	./jsonlang -e $(TEST_SNIPPET)
	LD_LIBRARY_PATH=. ./libjsonlang_test_snippet $(TEST_SNIPPET)
	LD_LIBRARY_PATH=. ./libjsonlang_test_file "test_suite/object.jsonlang"
	cd examples ; ./check.sh
	cd examples/terraform ; ./check.sh
	cd test_suite ; ./run_tests.sh
	cd test_suite ; ./run_fmt_tests.sh

MAKEDEPEND_SRCS = \
	cmd/jsonlang.cpp \
	core/libjsonlang_test_snippet.c \
	core/libjsonlang_test_file.c

depend:
	makedepend -f- $(LIB_SRC) $(MAKEDEPEND_SRCS) > Makefile.depend

core/desugarer.cpp: core/std.jsonlang.h

# Object files
%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $< -o $@

# Commandline executable.
jsonlang: cmd/jsonlang.cpp $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< $(LIB_SRC:.cpp=.o) -o $@

# C binding.
libjsonlang.so: $(LIB_OBJ)
	$(CXX) $(LDFLAGS) $(LIB_OBJ) $(SHARED_LDFLAGS) -o $@

libjsonlang++.so: $(LIB_CPP_OBJ)
	$(CXX) $(LDFLAGS) $(LIB_CPP_OBJ) $(SHARED_LDFLAGS) -o $@

# Javascript build of C binding
JS_EXPORTED_FUNCTIONS = 'EXPORTED_FUNCTIONS=["_jsonlang_make", "_jsonlang_evaluate_snippet", "_jsonlang_realloc", "_jsonlang_destroy"]'

libjsonlang.js: $(LIB_SRC) $(ALL_HEADERS)
	$(EMCXX) -s $(JS_EXPORTED_FUNCTIONS) $(EMCXXFLAGS) $(LDFLAGS) $(LIB_SRC) -o $@

# Copy javascript build to doc directory
doc/js/libjsonlang.js: libjsonlang.js
	$(CP) $^ $@

# Tests for C binding.
LIBJSONLANG_TEST_SNIPPET_SRCS = \
	core/libjsonlang_test_snippet.c \
	libjsonlang.so \
	include/libjsonlang.h

libjsonlang_test_snippet: $(LIBJSONLANG_TEST_SNIPPET_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -L. -ljsonlang -o $@

LIBJSONLANG_TEST_FILE_SRCS = \
	core/libjsonlang_test_file.c \
	libjsonlang.so \
	include/libjsonlang.h

libjsonlang_test_file: $(LIBJSONLANG_TEST_FILE_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -L. -ljsonlang -o $@

# Encode standard library for embedding in C
core/%.jsonlang.h: stdlib/%.jsonlang
	(($(OD) -v -Anone -t u1 $< \
		| tr " " "\n" \
		| grep -v "^$$" \
		| tr "\n" "," ) && echo "0") > $@
	echo >> $@

clean:
	rm -vf */*~ *~ .*~ */.*.swp .*.swp $(ALL) *.o core/*.jsonlang.h Make.depend

-include Makefile.depend
