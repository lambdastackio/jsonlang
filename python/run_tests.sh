#!/bin/bash

TEST_SNIPPET="$(cat test.jsonlang)"

echo "Python testing Jsonlang snippet..."
OUTPUT="$(python jsonlang_test_snippet.py "${TEST_SNIPPET}")"
if [ "$?" != "0" ] ; then
    echo "Jsonlang execution failed:"
    echo "$OUTPUT"
    exit 1
fi
if [ "$OUTPUT" != "true" ] ; then
    echo "Got bad output:"
    echo "$OUTPUT"
    exit 1
fi

echo "Python testing Jsonlang file..."
OUTPUT="$(python jsonlang_test_file.py "test.jsonlang")"
if [ "$?" != "0" ] ; then
    echo "Jsonlang execution failed:"
    echo "$OUTPUT"
    exit 1
fi
if [ "$OUTPUT" != "true" ] ; then
    echo "Got bad output:"
    echo "$OUTPUT"
    exit 1
fi

echo "Python test passed."
